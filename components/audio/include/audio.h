#pragma once

// Solana OS audio: the engine as the OS and apps see it (task spec "App
// API"). An optional module: everything here is safe to call on a badge
// without the DAC (audio_available() is false and the rest does nothing),
// and with CONFIG_SOLANA_AUDIO off it all compiles to the same no-ops.
//
// These are the C names the spec lists for badge.h. Lua apps get them as the
// `audio` module (main/lua/lua_audio.c). Handles are small integers; errors
// are negative AUD_ERR_* codes. Times: delays in ms from the call; musical
// times in ticks of the transport clock.
//
// Threads: app calls come from one task at a time (the Lua task, or the OS
// task for Settings); they never block on the mixer. audio_release() may
// come from the OS task while the app's task is still winding down.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aud.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// OS side

void audio_boot(void);                      // once at boot, after the I2C bus is up: probe the DAC
bool audio_available(void);                 // a DAC answered at boot
void audio_release(void);                   // the app exited: silence, free the pool, stop the task
bool audio_running(void);                   // the mixer task exists

// Settings (persisted; FR-4): volume 0..AUDIO_VOLUME_MAX, mute. They apply
// on top of the app's volume, at once.
#define AUDIO_VOLUME_MAX    10
int audio_setting_volume(void);
void audio_setting_set_volume(int v);
bool audio_setting_muted(void);
void audio_setting_set_muted(bool m);
// The Settings test tone: 440 Hz at a low level; false stops it and the engine.
int audio_test_tone(bool on);

// ---------------------------------------------------------------------------
// App side

// Optional (default 32000 Hz stereo). Call it before making anything: once
// the app has a synth, bank or song, another format returns AUD_ERR_STATE.
// rate 22050, 32000 or 48000; channels 1 or 2.
int audio_init(int rate, int channels);
// Silence and forget everything the app made (synths, banks, songs,
// timelines, the pool) and stop the engine, as when the app exits; the next
// call starts afresh, so audio_init can then pick another format.
int audio_reset(void);

typedef struct {
    int wave;                   // AUD_WAVE_*
    float pw;                   // pulse width 0.05..0.95
    float attack, decay, sustain, release;   // ms, ms, 0..1, ms
    bool filter;
    float cutoff, res, fenv, key_track;      // 0..1 (fenv -1..1)
    float lfo_rate;             // Hz
    float vibrato;              // semitones
    float tremolo;              // 0..1
    float lfo_cutoff;           // 0..1
    float lfo_delay;            // ms
    int lfo_targets;            // AUD_LFO_* (0: all three)
    float volume, pan, detune, portamento;  // 0..2, -1..1, cents, ms
} audio_synth_t;

void audio_synth_default(audio_synth_t *s);
int audio_synth_new(const audio_synth_t *s);
int audio_synth_set(int handle, const audio_synth_t *s);    // notes started afterwards

typedef struct {
    int root, key_lo, key_hi, vel_lo, vel_hi, layer;
    float fine;                 // cents
    float volume;
    float attack, decay, sustain, release;
    int loop_start, loop_end;   // frames; end <= start: the file's loop, or none
} audio_sample_opts_t;

void audio_sample_default(audio_sample_opts_t *o);
int audio_bank_new(void);
// Load a WAV (PCM8/16 or IMA ADPCM, mono) into the pool and add it to the
// bank. read() is the file; the loader reads it itself (FR-18).
int audio_bank_add(int bank, aud_read_fn read, void *ud, const audio_sample_opts_t *o);
int audio_bank_add_file(int bank, const char *path, const audio_sample_opts_t *o);
int audio_program(int bank_msb, int program, int handle);   // what a .mid's program change selects

int audio_assign(int ch, int handle);
uint32_t audio_note_on(int ch, int note, int vel, uint32_t delay_ms, const aud_pv_t *bundle, int n);
int audio_note_off(int ch, int note, uint32_t delay_ms);
int audio_note_off_h(uint32_t handle, uint32_t delay_ms);
int audio_cc(int ch, int cc, int value, uint32_t delay_ms);
int audio_bend(int ch, float amount, uint32_t delay_ms);    // -1..1 of the bend range
int audio_pressure(int ch, float amount, uint32_t delay_ms);
int audio_midi(uint8_t status, uint8_t d1, uint8_t d2, uint32_t delay_ms);
int audio_all_off(bool kill);

// Parameters by AUD_P_* id (aud_param_find("cutoff")); values in API units:
// 0..1 levels, ms for times, Hz for vibrato_rate, cents for detune.
int audio_param(int ch, int param, float value, uint32_t delay_ms);
int audio_note_param(uint32_t handle, int param, float value, uint32_t delay_ms);
int audio_note_ramp(uint32_t handle, int param, float value, uint32_t ms);
int audio_note_bend(uint32_t handle, float semitones, uint32_t delay_ms);
int audio_note_pressure(uint32_t handle, float amount, uint32_t delay_ms);
int audio_nrpn(int ch, int lsb, int value);                 // NRPN MSB 1 (task spec table)
int audio_rpn(int ch, int lsb, int value);
int audio_map_cc(int ch, int cc, int param, float min, float max);
int audio_pressure_target(int ch, int param, float amount);
int audio_mpe(int first, int last);                         // first > last: off
int32_t audio_param_value(int param, float value);          // API units -> engine units

int audio_volume(float v);                                  // the app's volume 0..1

// Songs
int audio_song_load(const uint8_t *data, size_t len);
int audio_song_load_file(const char *path);
int64_t audio_song_play(int song, const aud_song_opts_t *o);
int64_t audio_song_pause(uint32_t quant);
int64_t audio_song_stop(uint32_t quant);

// Clock, grid and quantised editing. Items as aud_item_t, values in engine
// units (audio_param_value for AUD_IT_PARAM).
int64_t audio_transport(float bpm, int ppqn, int beats, uint32_t quant);
int audio_transport_stop(void);
void audio_grid(aud_grid_t *g);
uint32_t audio_clock(void);
uint32_t audio_heard(void);
int64_t audio_insert(int ch, const aud_item_t *items, int n, const aud_edit_opts_t *o);
int64_t audio_update(int ch, uint32_t from, uint32_t to, const aud_item_t *items, int n, const aud_edit_opts_t *o);
int64_t audio_delete(int ch, uint32_t from, uint32_t to, const aud_edit_opts_t *o);
int64_t audio_stop(int ch, const aud_edit_opts_t *o);      // ch AUD_ALL: everything

typedef struct {
    float cpu, peak_cpu;        // % of the core spent rendering
    uint32_t underruns, dropped_events, voices_active, pool_used, pool_free, rate, channels;
    uint32_t stale, ignored, ramps_dropped, tl_rejected, tl_late, tl_free, clipped;
    uint32_t latency_frames;
    bool running, dac_awake;
} audio_stats_t;
void audio_stats(audio_stats_t *out);

#ifdef __cplusplus
}
#endif
