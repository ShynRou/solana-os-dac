// Solana OS audio service: the portable engine (core/) on a mixer task, over
// audio_port (the badge's I2S + DAC, or the emulator's WebAudio). See
// include/audio.h for the API and README.md for the design.
//
// Lifetime (task spec "Runtime"): nothing runs until an app's first audio
// call. That call allocates the engine (hot state in internal RAM, the rest
// and the sample pool in PSRAM), starts the output and the mixer task. When
// the app exits, the OS calls audio_release(): silence, stop the task, stop
// the output, free everything. Settings' test tone uses the same path.
//
// Threads. App calls run on the app's task, Settings on the OS task;
// s_lock serialises them with audio_release() (the engine allows one
// producer at a time). The mixer task never takes s_lock: it only renders,
// so a call never waits for a block, and a block never waits for a call.
//
// The DAC sleeps after CONFIG_SOLANA_AUDIO_SILENCE_S of silence. A sleeping
// mixer stops rendering (the engine's clock pauses with it, since nothing is
// sounding or scheduled) and wakes on the next call.

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "audio.h"
#include "audio_port.h"

#ifndef CONFIG_SOLANA_AUDIO_POOL_KB
#define CONFIG_SOLANA_AUDIO_POOL_KB     1024
#define CONFIG_SOLANA_AUDIO_RESERVE_KB  512
#define CONFIG_SOLANA_AUDIO_SILENCE_S   10
#define CONFIG_SOLANA_AUDIO_TASK_PRIO   6
#endif

static const char *TAG = "audio";

#define BLOCK           256                 // frames per render and per DMA block
#define DEFAULT_RATE    32000
#define DEFAULT_CHANS   2
#define POOL_MIN        (64 * 1024)
#define TASK_STACK      4096
#define WRITE_TIMEOUT   200                 // ms; the ring drains a block in 5 to 12 ms
#define SLEEP_POLL_MS   100

static SemaphoreHandle_t s_lock;
static bool s_avail;

static aud_engine_t *s_eng;
static void *s_cold, *s_pool;
static size_t s_pool_size;
static uint32_t s_rate = DEFAULT_RATE;
static int s_chans = DEFAULT_CHANS;
static bool s_used;                         // the app made something: the format is fixed

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static atomic_bool s_quit, s_asleep;
static atomic_uint s_kick;                  // bumped by every call: work may be waiting
static atomic_uint s_gain;                  // master gain Q16 (Settings x app)
static int16_t s_buf[BLOCK * 2];

// Stats the mixer writes (CPU in 1/100 %).
static atomic_uint s_cpu, s_cpu_peak;

// Settings
static uint8_t s_volume = 8;              // -6 dB
static bool s_muted;
static uint32_t s_app_vol = 65536;
static int s_tone_patch;

// ---------------------------------------------------------------------------
// Settings

// 3 dB per step; 10 is full scale.
static const uint32_t s_vol_q16[AUDIO_VOLUME_MAX + 1] = {
    0, 2927, 4135, 5841, 8250, 11654, 16462, 23253, 32846, 46396, 65536,
};

static void update_gain(void)
{
    uint32_t g = s_muted ? 0 : (uint32_t)(((uint64_t)s_vol_q16[s_volume] * s_app_vol) >> 16);
    atomic_store(&s_gain, g);
}

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= AUDIO_VOLUME_MAX) {
            s_volume = v;
        }
        if (nvs_get_u8(h, "mute", &v) == ESP_OK) {
            s_muted = v != 0;
        }
        nvs_close(h);
    }
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", s_volume);
        nvs_set_u8(h, "mute", s_muted);
        nvs_commit(h);
        nvs_close(h);
    }
}

int audio_setting_volume(void)
{
    return s_volume;
}

void audio_setting_set_volume(int v)
{
    s_volume = (uint8_t)(v < 0 ? 0 : v > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : v);
    update_gain();
    settings_save();
}

bool audio_setting_muted(void)
{
    return s_muted;
}

void audio_setting_set_muted(bool m)
{
    s_muted = m;
    update_gain();
    settings_save();
}

// ---------------------------------------------------------------------------
// Mixer task

static void mixer_task(void *arg)
{
    uint32_t block_us = (uint32_t)((uint64_t)BLOCK * 1000000 / s_rate);
    uint32_t silence = (uint32_t)CONFIG_SOLANA_AUDIO_SILENCE_S * s_rate;
    uint32_t avg = 0;
    while (!atomic_load(&s_quit)) {
        if (atomic_load(&s_asleep)) {
            unsigned k = atomic_load(&s_kick);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SLEEP_POLL_MS));
            if (atomic_load(&s_kick) == k || atomic_load(&s_quit)) {
                continue;
            }
            audio_port_sleep(false);
            atomic_store(&s_asleep, false);
        }
        unsigned k = atomic_load(&s_kick);
        aud_set_master(s_eng, atomic_load(&s_gain));
        int64_t t0 = esp_timer_get_time();
        aud_render(s_eng, s_buf, BLOCK);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        // CPU: render time over block time, smoothed over ~16 blocks.
        uint32_t pct = (uint32_t)((uint64_t)us * 10000 / block_us);
        avg = avg ? avg + ((int32_t)(pct - avg) >> 4) : pct;
        atomic_store(&s_cpu, avg);
        if (pct > atomic_load(&s_cpu_peak)) {
            atomic_store(&s_cpu_peak, pct);
        }
        audio_port_write(s_buf, BLOCK, WRITE_TIMEOUT);
        if (aud_idle_frames(s_eng) >= silence && atomic_load(&s_kick) == k) {
            atomic_store(&s_asleep, true);
            // A call that raced the store above bumped s_kick: the next
            // pass sees it and wakes straight away.
            audio_port_sleep(true);
        }
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Start and stop (under s_lock)

static void free_all(void)
{
    heap_caps_free(s_eng);
    heap_caps_free(s_cold);
    heap_caps_free(s_pool);
    s_eng = NULL;
    s_cold = s_pool = NULL;
    s_pool_size = 0;
}

static void stop_locked(void)
{
    if (s_task) {
        atomic_store(&s_quit, true);
        xTaskNotifyGive(s_task);
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_task = NULL;
    }
    if (s_eng) {
        audio_port_stop();
    }
    free_all();
    atomic_store(&s_asleep, false);
    atomic_store(&s_cpu, 0);
    atomic_store(&s_cpu_peak, 0);
    s_used = false;
    s_tone_patch = 0;
    s_app_vol = 65536;
    update_gain();
}

static void *alloc_ext(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static int start_locked(void)
{
    if (s_task) {
        return AUD_OK;
    }
    s_eng = heap_caps_malloc(aud_engine_size(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_cold = alloc_ext(aud_cold_size());
    // The pool: the configured size, but never so much that PSRAM falls
    // under the OS's reserve (task spec "Memory risk", option 1).
    size_t want = (size_t)CONFIG_SOLANA_AUDIO_POOL_KB * 1024;
    size_t reserve = (size_t)CONFIG_SOLANA_AUDIO_RESERVE_KB * 1024;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t free_ = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t room = free_ > reserve ? free_ - reserve : 0;
    if (want > room) {
        want = room;
    }
    if (want > largest) {
        want = largest;
    }
    want &= ~(size_t)15;
    if (want >= POOL_MIN) {
        s_pool = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_eng || !s_cold || !s_pool) {
        ESP_LOGE(TAG, "out of memory (engine %u, cold %u, pool %u)", (unsigned)aud_engine_size(),
                 (unsigned)aud_cold_size(), (unsigned)want);
        free_all();
        return AUD_ERR_NOMEM;
    }
    s_pool_size = want;
    aud_init(s_eng, s_cold, s_pool, s_pool_size, s_rate, s_chans);
    if (audio_port_start(s_rate, s_chans, BLOCK) != 0) {
        free_all();
        return AUD_ERR_NODEV;
    }
    atomic_store(&s_quit, false);
    atomic_store(&s_asleep, false);
    update_gain();
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    if (xTaskCreate(mixer_task, "audio", TASK_STACK, NULL, CONFIG_SOLANA_AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        audio_port_stop();
        free_all();
        return AUD_ERR_NOMEM;
    }
    ESP_LOGI(TAG, "started: %lu Hz, %d ch, pool %u KB", (unsigned long)s_rate, s_chans,
             (unsigned)(s_pool_size / 1024));
    return AUD_OK;
}

// Every app call: the lock, and the engine running.
static int enter(void)
{
    if (!s_avail || !s_lock) {
        return AUD_ERR_NODEV;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = start_locked();
    if (rc != AUD_OK) {
        xSemaphoreGive(s_lock);
    }
    return rc;
}

static void leave(void)
{
    atomic_fetch_add(&s_kick, 1);
    if (atomic_load(&s_asleep) && s_task) {
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
}

#define ENTER()     do { int _rc = enter(); if (_rc != AUD_OK) return _rc; } while (0)
#define ENTER0()    do { if (enter() != AUD_OK) return 0; } while (0)

static uint32_t ms(uint32_t delay_ms)
{
    return aud_ms_to_samples(s_eng, delay_ms);
}

static int32_t q16(float v)
{
    if (!(v == v)) {
        return 0;               // NaN
    }
    float x = v * 65536.0f;
    return x >= 2147483520.0f ? INT32_MAX : x <= -2147483520.0f ? INT32_MIN : (int32_t)lrintf(x);
}

static int ok(bool b)
{
    return b ? AUD_OK : AUD_ERR_FULL;
}

// ---------------------------------------------------------------------------
// OS side

void audio_boot(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    settings_load();
    update_gain();
    s_avail = audio_port_probe();
}

bool audio_available(void)
{
    return s_avail;
}

bool audio_running(void)
{
    return s_task != NULL;
}

void audio_release(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task && s_eng) {
        // Let the 2 ms kill fade reach the jack before the output stops.
        aud_all_off(s_eng, true);
        atomic_fetch_add(&s_kick, 1);
        if (atomic_load(&s_asleep)) {
            xTaskNotifyGive(s_task);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    stop_locked();
    xSemaphoreGive(s_lock);
}

int audio_test_tone(bool on)
{
    if (!on) {
        audio_release();
        return AUD_OK;
    }
    ENTER();
    if (!s_tone_patch) {
        aud_patch_t p;
        aud_patch_default(&p);
        p.p[AUD_P_VOLUME] = 65536 / 2;     // -6 dB on one voice's level, then Settings' volume
        s_tone_patch = aud_patch_new(s_eng, &p);
        if (s_tone_patch > 0) {
            aud_assign(s_eng, 0, s_tone_patch);
            aud_note_on(s_eng, 0, 69, 100, 0, NULL, 0);    // A4, held until off
        }
    }
    int rc = s_tone_patch > 0 ? AUD_OK : s_tone_patch;
    leave();
    return rc;
}

// ---------------------------------------------------------------------------
// App side

int audio_init(int rate, int channels)
{
    if (rate != 22050 && rate != 32000 && rate != 48000) {
        return AUD_ERR_ARG;
    }
    if (channels != 1 && channels != 2) {
        return AUD_ERR_ARG;
    }
    if (!s_avail || !s_lock) {
        return AUD_ERR_NODEV;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = AUD_OK;
    if ((uint32_t)rate != s_rate || channels != s_chans) {
        if (s_used) {
            rc = AUD_ERR_STATE;     // first call wins once the app made something
        } else {
            if (s_task) {
                stop_locked();
            }
            s_rate = (uint32_t)rate;
            s_chans = channels;
        }
    }
    if (rc == AUD_OK) {
        rc = start_locked();
    }
    s_used = s_used || rc == AUD_OK;
    xSemaphoreGive(s_lock);
    return rc;
}

int audio_reset(void)
{
    if (!s_avail) {
        return AUD_ERR_NODEV;
    }
    audio_release();
    return AUD_OK;
}

void audio_synth_default(audio_synth_t *s)
{
    aud_patch_t p;
    aud_patch_default(&p);
    memset(s, 0, sizeof(*s));
    s->wave = AUD_WAVE_SINE;
    s->pw = 0.5f;
    s->attack = (float)p.p[AUD_P_ATTACK];
    s->decay = (float)p.p[AUD_P_DECAY];
    s->sustain = p.p[AUD_P_SUSTAIN] / 65536.0f;
    s->release = (float)p.p[AUD_P_RELEASE];
    s->cutoff = 1.0f;
    s->lfo_rate = 5.0f;
    s->volume = 1.0f;
}

static void to_patch(const audio_synth_t *s, aud_patch_t *p)
{
    aud_patch_default(p);
    p->wave = (uint8_t)(s->wave >= 0 && s->wave < AUD_WAVE_COUNT ? s->wave : AUD_WAVE_SINE);
    p->filter = s->filter;
    p->lfo_targets = (uint8_t)(s->lfo_targets ? s->lfo_targets : AUD_LFO_PITCH | AUD_LFO_AMP | AUD_LFO_CUTOFF);
    const struct { int param; float v; } m[] = {
        { AUD_P_VOLUME, s->volume }, { AUD_P_PAN, s->pan }, { AUD_P_CUTOFF, s->cutoff },
        { AUD_P_RES, s->res }, { AUD_P_ATTACK, s->attack }, { AUD_P_DECAY, s->decay },
        { AUD_P_SUSTAIN, s->sustain }, { AUD_P_RELEASE, s->release }, { AUD_P_VIB_RATE, s->lfo_rate },
        { AUD_P_VIB_DEPTH, s->vibrato }, { AUD_P_VIB_DELAY, s->lfo_delay }, { AUD_P_TREM, s->tremolo },
        { AUD_P_PW, s->pw }, { AUD_P_DETUNE, s->detune }, { AUD_P_FENV, s->fenv },
        { AUD_P_PORTA, s->portamento },
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        p->p[m[i].param] = aud_param_from_q16(m[i].param, q16(m[i].v));
    }
    p->key_track = q16(s->key_track);
    p->lfo_cutoff = q16(s->lfo_cutoff);
    p->noise = (uint8_t)(s->noise >= 0 && s->noise < AUD_NOISE_COUNT ? s->noise : AUD_NOISE_WHITE);
    p->voices = (uint8_t)(s->voices < 0 ? 0 : s->voices > AUD_VOICES ? AUD_VOICES : s->voices);
    p->legato = s->legato;
}

int audio_synth_new(const audio_synth_t *s)
{
    ENTER();
    aud_patch_t p;
    to_patch(s, &p);
    int h = aud_patch_new(s_eng, &p);
    s_used = true;
    leave();
    return h;
}

int audio_synth_set(int handle, const audio_synth_t *s)
{
    ENTER();
    aud_patch_t p;
    to_patch(s, &p);
    int rc = aud_patch_set(s_eng, handle, &p);
    leave();
    return rc;
}

void audio_sample_default(audio_sample_opts_t *o)
{
    aud_sample_t s;
    aud_sample_default(&s);
    memset(o, 0, sizeof(*o));
    o->root = s.root;
    o->key_lo = s.key_lo;
    o->key_hi = s.key_hi;
    o->vel_lo = s.vel_lo;
    o->vel_hi = s.vel_hi;
    o->volume = 1.0f;
    o->attack = (float)s.adsr[0];
    o->decay = (float)s.adsr[1];
    o->sustain = s.adsr[2] / 65536.0f;
    o->release = (float)s.adsr[3];
}

int audio_bank_new(void)
{
    ENTER();
    int h = aud_bank_new(s_eng);
    s_used = true;
    leave();
    return h;
}

static uint8_t u7(int v)
{
    return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}

static uint32_t ms_clamp(float v)
{
    return v <= 0 ? 0 : v >= 600000.0f ? 600000 : (uint32_t)v;
}

int audio_bank_add(int bank, aud_read_fn read, void *ud, const audio_sample_opts_t *o)
{
    audio_sample_opts_t def;
    if (!o) {
        audio_sample_default(&def);
        o = &def;
    }
    ENTER();
    int rc = AUD_ERR_ARG;
    if (aud_handle_is_bank(s_eng, bank)) {
        aud_sample_t s;
        aud_sample_default(&s);
        rc = aud_wav_load(s_eng, read, ud, &s);
        if (rc == AUD_OK) {
            s.root = u7(o->root);
            s.key_lo = u7(o->key_lo);
            s.key_hi = u7(o->key_hi);
            s.vel_lo = u7(o->vel_lo);
            s.vel_hi = u7(o->vel_hi);
            s.layer = u7(o->layer);
            s.fine = (int16_t)(o->fine < -100 ? -100 : o->fine > 100 ? 100 : o->fine);
            s.volume = q16(o->volume < 0 ? 0 : o->volume > 2 ? 2 : o->volume);
            s.adsr[0] = (int32_t)ms_clamp(o->attack);
            s.adsr[1] = (int32_t)ms_clamp(o->decay);
            s.adsr[2] = q16(o->sustain < 0 ? 0 : o->sustain > 1 ? 1 : o->sustain);
            s.adsr[3] = (int32_t)ms_clamp(o->release);
            if (o->loop_end > o->loop_start && o->loop_start >= 0) {
                s.loop_start = (uint32_t)o->loop_start;
                s.loop_end = (uint32_t)o->loop_end < s.len ? (uint32_t)o->loop_end : s.len;
            }
            rc = aud_bank_add(s_eng, bank, &s);
        }
    }
    leave();
    return rc;
}

static size_t file_read(void *ud, void *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ud);
}

int audio_bank_add_file(int bank, const char *path, const audio_sample_opts_t *o)
{
    if (!s_avail) {
        return AUD_ERR_NODEV;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return AUD_ERR_ARG;
    }
    int rc = audio_bank_add(bank, file_read, f, o);
    fclose(f);
    return rc;
}

int audio_program(int bank_msb, int program, int handle)
{
    ENTER();
    int rc = aud_program_set(s_eng, bank_msb, program, handle);
    leave();
    return rc;
}

int audio_assign(int ch, int handle)
{
    ENTER();
    int rc = ok(aud_assign(s_eng, ch, handle));
    leave();
    return rc;
}

uint32_t audio_note_on(int ch, int note, int vel, uint32_t delay_ms, const aud_pv_t *bundle, int n)
{
    ENTER0();
    uint32_t h = aud_note_on(s_eng, ch, note, vel, ms(delay_ms), bundle, n);
    leave();
    return h;
}

int audio_chord(int ch, const int *notes, int count, int vel, uint32_t delay_ms, uint32_t strum_ms, uint32_t *out)
{
    memset(out, 0, sizeof(*out) * (size_t)(count > 0 ? count : 0));
    ENTER0();
    int started = 0;
    for (int i = 0; i < count; i++) {
        uint32_t h = aud_note_on(s_eng, ch, notes[i], vel, ms(delay_ms + strum_ms * (uint32_t)i), NULL, 0);
        out[i] = h;
        started += h != 0;
    }
    leave();
    return started;
}

int audio_note_off(int ch, int note, uint32_t delay_ms)
{
    ENTER();
    int rc = ok(aud_note_off(s_eng, ch, note, ms(delay_ms)));
    leave();
    return rc;
}

int audio_note_off_h(uint32_t handle, uint32_t delay_ms)
{
    ENTER();
    int rc = ok(aud_note_off_h(s_eng, handle, ms(delay_ms)));
    leave();
    return rc;
}

int audio_cc(int ch, int cc, int value, uint32_t delay_ms)
{
    ENTER();
    int rc = ok(aud_cc(s_eng, ch, cc, value, ms(delay_ms)));
    leave();
    return rc;
}

int audio_bend(int ch, float amount, uint32_t delay_ms)
{
    float a = amount < -1 ? -1 : amount > 1 ? 1 : amount;
    int v = (int)lrintf(a * 8192.0f);
    ENTER();
    int rc = ok(aud_bend(s_eng, ch, v > 8191 ? 8191 : v, ms(delay_ms)));
    leave();
    return rc;
}

int audio_pressure(int ch, float amount, uint32_t delay_ms)
{
    float a = amount < 0 ? 0 : amount > 1 ? 1 : amount;
    ENTER();
    int rc = ok(aud_pressure(s_eng, ch, (int)lrintf(a * 127.0f), ms(delay_ms)));
    leave();
    return rc;
}

int audio_midi(uint8_t status, uint8_t d1, uint8_t d2, uint32_t delay_ms)
{
    ENTER();
    int rc = ok(aud_midi(s_eng, status, d1, d2, ms(delay_ms)));
    leave();
    return rc;
}

int audio_all_off(bool kill)
{
    ENTER();
    int rc = ok(aud_all_off(s_eng, kill));
    leave();
    return rc;
}

int32_t audio_param_value(int param, float value)
{
    return aud_param_from_q16(param, q16(value));
}

int audio_param(int ch, int param, float value, uint32_t delay_ms)
{
    ENTER();
    int rc = ok(aud_param(s_eng, ch, param, audio_param_value(param, value), ms(delay_ms)));
    leave();
    return rc;
}

int audio_note_param(uint32_t handle, int param, float value, uint32_t delay_ms)
{
    ENTER();
    int rc = ok(aud_note_param(s_eng, handle, param, audio_param_value(param, value), ms(delay_ms)));
    leave();
    return rc;
}

int audio_note_ramp(uint32_t handle, int param, float value, uint32_t ms_)
{
    ENTER();
    int rc = ok(aud_note_ramp(s_eng, handle, param, audio_param_value(param, value), ms_, 0));
    leave();
    return rc;
}

int audio_note_bend(uint32_t handle, float semitones, uint32_t delay_ms)
{
    float s = semitones < -96 ? -96 : semitones > 96 ? 96 : semitones;
    ENTER();
    int rc = ok(aud_note_bend(s_eng, handle, (int32_t)lrintf(s * 256.0f), ms(delay_ms)));
    leave();
    return rc;
}

int audio_note_pressure(uint32_t handle, float amount, uint32_t delay_ms)
{
    float a = amount < 0 ? 0 : amount > 1 ? 1 : amount;
    ENTER();
    int rc = ok(aud_note_pressure(s_eng, handle, q16(a), ms(delay_ms)));
    leave();
    return rc;
}

// Data entry: 0..127 as the MSB alone; more (to 16383) as MSB and LSB.
static bool data_entry(int ch, int value)
{
    value = value < 0 ? 0 : value > 16383 ? 16383 : value;
    if (value < 128) {
        return aud_cc(s_eng, ch, 6, value, 0);
    }
    return aud_cc(s_eng, ch, 6, value >> 7, 0) && aud_cc(s_eng, ch, 38, value & 127, 0);
}

int audio_nrpn(int ch, int lsb, int value)
{
    ENTER();
    bool b = aud_cc(s_eng, ch, 99, 1, 0) && aud_cc(s_eng, ch, 98, lsb, 0) && data_entry(ch, value);
    int rc = ok(b);
    leave();
    return rc;
}

int audio_rpn(int ch, int lsb, int value)
{
    ENTER();
    bool b = aud_cc(s_eng, ch, 101, 0, 0) && aud_cc(s_eng, ch, 100, lsb, 0) && data_entry(ch, value);
    int rc = ok(b);
    leave();
    return rc;
}

int audio_map_cc(int ch, int cc, int param, float min, float max)
{
    ENTER();
    int rc = ok(aud_map_cc(s_eng, ch, cc, param, audio_param_value(param, min), audio_param_value(param, max)));
    leave();
    return rc;
}

int audio_pressure_target(int ch, int param, float amount)
{
    ENTER();
    int rc = ok(aud_pressure_target(s_eng, ch, param, q16(amount)));
    leave();
    return rc;
}

int audio_mpe(int first, int last)
{
    ENTER();
    int rc = ok(aud_mpe(s_eng, first, last));
    leave();
    return rc;
}

int audio_volume(float v)
{
    if (!s_avail) {
        return AUD_ERR_NODEV;
    }
    float a = v < 0 ? 0 : v > 1 ? 1 : v;
    s_app_vol = (uint32_t)lrintf(a * 65536.0f);
    update_gain();
    return AUD_OK;
}

int audio_song_load(const uint8_t *data, size_t len)
{
    ENTER();
    int rc = aud_song_load(s_eng, data, len);
    s_used = true;
    leave();
    return rc;
}

int audio_song_load_file(const char *path)
{
    if (!s_avail) {
        return AUD_ERR_NODEV;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return AUD_ERR_ARG;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 && n <= 1024 * 1024 ? malloc((size_t)n) : NULL;
    int rc = AUD_ERR_NOMEM;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        rc = audio_song_load(buf, (size_t)n);
    }
    free(buf);
    fclose(f);
    return rc;
}

int64_t audio_song_play(int song, const aud_song_opts_t *o)
{
    ENTER();
    int64_t rc = aud_song_play(s_eng, song, o);
    leave();
    return rc;
}

int64_t audio_song_pause(uint32_t quant)
{
    ENTER();
    int64_t rc = aud_song_pause(s_eng, quant);
    leave();
    return rc;
}

int64_t audio_song_stop(uint32_t quant)
{
    ENTER();
    int64_t rc = aud_song_stop(s_eng, quant);
    leave();
    return rc;
}

int64_t audio_transport(float bpm, int ppqn, int beats, uint32_t quant)
{
    if (!(bpm >= 20 && bpm <= 300) || ppqn < 0 || beats < 0) {
        return AUD_ERR_ARG;
    }
    ENTER();
    int64_t rc = aud_transport(s_eng, (uint32_t)lrintf(bpm * 65536.0f), (uint32_t)ppqn, (uint32_t)beats, quant);
    leave();
    return rc;
}

int audio_transport_stop(void)
{
    ENTER();
    int rc = ok(aud_transport_stop(s_eng));
    leave();
    return rc;
}

void audio_grid(aud_grid_t *g)
{
    memset(g, 0, sizeof(*g));
    if (enter() != AUD_OK) {
        return;
    }
    aud_grid(s_eng, g);
    leave();
}

uint32_t audio_clock(void)
{
    ENTER0();
    uint32_t t = aud_clock(s_eng);
    leave();
    return t;
}

uint32_t audio_heard(void)
{
    ENTER0();
    uint32_t t = aud_heard(s_eng, audio_port_latency());
    leave();
    return t;
}

int64_t audio_insert(int ch, const aud_item_t *items, int n, const aud_edit_opts_t *o)
{
    ENTER();
    int64_t rc = aud_edit(s_eng, AUD_ED_INSERT, ch, 0, 0, items, n, o);
    leave();
    return rc;
}

int64_t audio_update(int ch, uint32_t from, uint32_t to, const aud_item_t *items, int n, const aud_edit_opts_t *o)
{
    ENTER();
    int64_t rc = aud_edit(s_eng, AUD_ED_UPDATE, ch, from, to, items, n, o);
    leave();
    return rc;
}

int64_t audio_delete(int ch, uint32_t from, uint32_t to, const aud_edit_opts_t *o)
{
    ENTER();
    int64_t rc = aud_edit(s_eng, AUD_ED_DELETE, ch, from, to, NULL, 0, o);
    leave();
    return rc;
}

int64_t audio_stop(int ch, const aud_edit_opts_t *o)
{
    ENTER();
    int64_t rc = aud_edit(s_eng, AUD_ED_STOP, ch, 0, 0, NULL, 0, o);
    leave();
    return rc;
}

void audio_stats(audio_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rate = s_rate;
    out->channels = (uint32_t)s_chans;
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);     // stats never start the engine
    if (s_task && s_eng) {
        aud_stats_t st;
        aud_stats(s_eng, &st);
        out->dropped_events = st.dropped_events;
        out->voices_active = st.voices_active;
        out->pool_used = st.pool_used;
        out->pool_free = st.pool_free;
        out->stale = st.stale;
        out->ignored = st.ignored;
        out->ramps_dropped = st.ramps_dropped;
        out->tl_rejected = st.tl_rejected;
        out->tl_late = st.tl_late;
        out->tl_free = st.tl_free;
        out->clipped = st.clipped;
        out->running = true;
        out->dac_awake = !atomic_load(&s_asleep);
        out->cpu = atomic_load(&s_cpu) / 100.0f;
        out->peak_cpu = atomic_load(&s_cpu_peak) / 100.0f;
        out->underruns = audio_port_underruns();
        out->latency_frames = audio_port_latency();
    }
    xSemaphoreGive(s_lock);
}
