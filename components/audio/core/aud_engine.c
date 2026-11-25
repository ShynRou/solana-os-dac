// Engine: memory, parameters, the producer's calls, the render loop and the
// master stage. See aud.h for the threading rules.

#include <string.h>
#include "aud_int.h"
#include "aud_tables.h"

// ---------------------------------------------------------------------------
// Parameters

typedef struct {
    const char *name;
    int32_t def, min, max;
    uint8_t mult;           // CC layer multiplies (times, rates)
    uint8_t ms;             // whole milliseconds (the API passes ms * 65536)
} param_def_t;

#define Q(x) ((int32_t)((x) * 65536.0 + ((x) >= 0 ? 0.5 : -0.5)))

static const param_def_t s_params[AUD_P_COUNT] = {
    [AUD_P_VOLUME] = {"volume", Q(1), 0, Q(2), 0, 0},
    [AUD_P_PAN] = {"pan", 0, Q(-1), Q(1), 0, 0},
    [AUD_P_CUTOFF] = {"cutoff", Q(1), 0, Q(1), 0, 0},
    [AUD_P_RES] = {"resonance", 0, 0, Q(1), 0, 0},
    [AUD_P_ATTACK] = {"attack", 5, 0, 60000, 1, 1},
    [AUD_P_DECAY] = {"decay", 80, 0, 60000, 1, 1},
    [AUD_P_SUSTAIN] = {"sustain", Q(0.8), 0, Q(1), 0, 0},
    [AUD_P_RELEASE] = {"release", 150, 0, 60000, 1, 1},
    [AUD_P_VIB_RATE] = {"vibrato_rate", Q(5), Q(0.1), Q(20), 1, 0},
    [AUD_P_VIB_DEPTH] = {"vibrato_depth", 0, 0, Q(12), 0, 0},
    [AUD_P_VIB_DELAY] = {"vibrato_delay", 0, 0, 10000, 1, 1},
    [AUD_P_TREM] = {"tremolo_depth", 0, 0, Q(1), 0, 0},
    [AUD_P_PW] = {"pulse_width", Q(0.5), Q(0.05), Q(0.95), 0, 0},
    [AUD_P_DETUNE] = {"detune", 0, Q(-100), Q(100), 0, 0},
    [AUD_P_FENV] = {"filter_env", 0, Q(-1), Q(1), 0, 0},
    [AUD_P_PORTA] = {"portamento", 0, 0, 10000, 0, 1},
};

const char *aud_param_name(int p)
{
    return p >= 0 && p < AUD_P_COUNT ? s_params[p].name : "";
}

int aud_param_find(const char *name)
{
    for (int i = 0; name && i < AUD_P_COUNT; i++) {
        if (!strcmp(name, s_params[i].name)) {
            return i;
        }
    }
    return -1;
}

int32_t aud_param_clamp(int p, int32_t v)
{
    return p >= 0 && p < AUD_P_COUNT ? aud_clampi(v, s_params[p].min, s_params[p].max) : v;
}

int32_t aud_param_from_q16(int p, int32_t q16)
{
    if (p < 0 || p >= AUD_P_COUNT) {
        return 0;
    }
    return aud_param_clamp(p, s_params[p].ms ? (q16 + 32768) >> 16 : q16);
}

int32_t aud_param_mult_kind(int p)
{
    return p >= 0 && p < AUD_P_COUNT && s_params[p].mult;
}

void aud_patch_default(aud_patch_t *p)
{
    memset(p, 0, sizeof(*p));
    p->wave = AUD_WAVE_SINE;
    p->lfo_targets = AUD_LFO_PITCH | AUD_LFO_AMP | AUD_LFO_CUTOFF;
    for (int i = 0; i < AUD_P_COUNT; i++) {
        p->p[i] = s_params[i].def;
    }
}

void aud_sample_default(aud_sample_t *s)
{
    memset(s, 0, sizeof(*s));
    s->root = 60;
    s->key_hi = 127;
    s->vel_hi = 127;
    s->volume = AUD_ONE;
    s->adsr[0] = 2;
    s->adsr[1] = 0;
    s->adsr[2] = AUD_ONE;
    s->adsr[3] = 50;
}

// ---------------------------------------------------------------------------
// Fixed-point helpers

uint32_t aud_pitch_ratio_q16(int32_t p)
{
    int32_t oct = p >= 0 ? p / 3072 : -((-p + 3071) / 3072);
    int32_t rem = p - oct * 3072;
    uint64_t r = ((uint64_t)aud_semi[rem >> 8] * aud_frac[rem & 255]) >> 16;
    if (oct >= 0) {
        if (oct > 15) {
            return UINT32_MAX;
        }
        r <<= oct;
    } else {
        r = -oct > 31 ? 0 : r >> -oct;
    }
    return r > UINT32_MAX ? UINT32_MAX : (uint32_t)r;
}

// ---------------------------------------------------------------------------
// Memory and lifecycle

size_t aud_engine_size(void)
{
    return sizeof(aud_engine_t);
}

size_t aud_cold_size(void)
{
    return sizeof(aud_cold_t);
}

static void publish(aud_engine_t *e)
{
    atomic_store_explicit(&e->pub_samples, e->samples, memory_order_release);
    atomic_store_explicit(&e->pub_clock, e->clock, memory_order_release);
    atomic_store_explicit(&e->pub_spt, e->spt, memory_order_relaxed);
    atomic_store_explicit(&e->pub_bpm, e->bpm_q16, memory_order_relaxed);
    atomic_store_explicit(&e->pub_ppqn, e->ppqn, memory_order_relaxed);
    atomic_store_explicit(&e->pub_beats, e->beats, memory_order_relaxed);
    atomic_store_explicit(&e->pub_tl_free, e->tl_free, memory_order_relaxed);
    atomic_store_explicit(&e->pub_blocks, e->blocks, memory_order_release);
}

void aud_init(aud_engine_t *e, void *cold, void *pool, size_t pool_size, uint32_t rate, int channels)
{
    memset(e, 0, sizeof(*e));
    e->cold = cold;
    e->pool = pool;
    e->pool_size = pool_size;
    e->rate = rate < 8000 ? 8000 : rate > 48000 ? 48000 : rate;
    e->nch = channels == 1 ? 1 : 2;
    e->master = e->master_target = AUD_ONE;
    e->dither_rng = 0x12345678;
    atomic_store(&e->pub_block_frames, AUD_BLOCK_MAX);
    aud_reset(e);
}

void aud_reset(aud_engine_t *e)
{
    aud_cold_t *c = e->cold;
    memset(c, 0, sizeof(*c));
    e->pool_used = 0;
    e->serial = 0;
    e->nmap_defs = e->nbanks = e->nsongs = 0;
    memset(e->last_apply, 0, sizeof(e->last_apply));
    for (int i = 0; i < AUD_HANDLES; i++) {
        atomic_store(&e->handles[i], 0);
        e->handle_used[i] = 0;
    }
    atomic_store(&e->ring.head, 0);
    atomic_store(&e->ring.tail, 0);
    atomic_store(&e->cmds.head, 0);
    atomic_store(&e->cmds.tail, 0);
    e->npending = e->ncmd = e->ntempos = 0;
    memset(e->voices, 0, sizeof(e->voices));
    for (int i = 0; i < AUD_VOICES; i++) {
        for (int r = 0; r < AUD_RAMPS_VOICE; r++) {
            e->voices[i].ramps[r].param = 0xFF;
        }
    }
    e->voice_order = 0;
    e->mixer_serial = 0;
    e->ramps_total = 0;
    e->mpe_first = 1;
    e->mpe_last = 0;
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        aud_chan_reset(e, ch, true);
    }
    e->clock = 0;
    e->tick_pos = 0;
    e->ppqn = 480;
    e->beats = 4;
    aud_tl_init(e);
    aud_tl_set_tempo(e, 120 * AUD_ONE);
    e->seq.song = -1;
    e->seq.playing = e->seq.paused = 0;
    e->lim_gain = AUD_ONE;
    e->idle = 0;
    atomic_store(&e->c_dropped, 0);
    atomic_store(&e->c_stale, 0);
    atomic_store(&e->c_ignored, 0);
    atomic_store(&e->c_ramps, 0);
    atomic_store(&e->c_rejected, 0);
    atomic_store(&e->c_late, 0);
    atomic_store(&e->c_voices, 0);
    atomic_store(&e->c_clipped, 0);
    publish(e);
}

void aud_set_master(aud_engine_t *e, uint32_t gain_q16)
{
    e->master_target = gain_q16 > 4 * AUD_ONE ? 4 * AUD_ONE : gain_q16;
}

void aud_set_trace(aud_engine_t *e, aud_trace_fn fn, void *ud)
{
    e->trace = fn;
    e->trace_ud = ud;
}

void aud_set_dither(aud_engine_t *e, bool on)
{
    e->dither = on;
}

uint32_t aud_idle_frames(const aud_engine_t *e)
{
    return e->idle;
}

uint32_t aud_rate(const aud_engine_t *e)
{
    return e->rate;
}

int aud_channels(const aud_engine_t *e)
{
    return e->nch;
}

void aud_stats(const aud_engine_t *e, aud_stats_t *o)
{
    memset(o, 0, sizeof(*o));
    o->dropped_events = atomic_load(&e->c_dropped);
    o->stale = atomic_load(&e->c_stale);
    o->ignored = atomic_load(&e->c_ignored);
    o->ramps_dropped = atomic_load(&e->c_ramps);
    o->tl_rejected = atomic_load(&e->c_rejected);
    o->tl_late = atomic_load(&e->c_late);
    o->tl_free = atomic_load(&e->pub_tl_free);
    o->voices_active = atomic_load(&e->c_voices);
    o->clipped = atomic_load(&e->c_clipped);
    o->pool_used = (uint32_t)e->pool_used;
    o->pool_free = (uint32_t)(e->pool_size - e->pool_used);
    o->rate = e->rate;
    o->channels = (uint32_t)e->nch;
}

// ---------------------------------------------------------------------------
// Producer: pool and instruments

void *aud_pool_alloc(aud_engine_t *e, size_t n)
{
    size_t at = (e->pool_used + 3) & ~(size_t)3;
    if (!e->pool || n > e->pool_size || at > e->pool_size - n) {
        return NULL;
    }
    e->pool_used = at + n;
    return e->pool + at;
}

void aud_pool_used(const aud_engine_t *e, size_t *used, size_t *free_)
{
    if (used) {
        *used = e->pool_used;
    }
    if (free_) {
        *free_ = e->pool_size - e->pool_used;
    }
}

static int handle_alloc(aud_engine_t *e)
{
    for (int h = 1; h < AUD_HANDLES; h++) {
        if (!e->handle_used[h]) {
            e->handle_used[h] = 1;
            return h;
        }
    }
    return AUD_ERR_FULL;
}

enum { SLOT_FREE, SLOT_LIVE, SLOT_RETIRED };

static int patch_slot(aud_engine_t *e)
{
    aud_cold_t *c = e->cold;
    uint32_t blocks = atomic_load_explicit(&e->pub_blocks, memory_order_acquire);
    for (int i = 0; i < AUD_PATCH_SLOTS; i++) {
        // A replaced patch may still be being copied by a note start in the
        // block that was rendering when it was replaced: two blocks later
        // nothing reads it.
        if (c->patch_used[i] == SLOT_FREE ||
                (c->patch_used[i] == SLOT_RETIRED && blocks - c->patch_retired[i] >= 2)) {
            c->patch_used[i] = SLOT_LIVE;
            return i;
        }
    }
    return AUD_ERR_FULL;
}

static void patch_sanitize(aud_patch_t *p)
{
    if (p->wave >= AUD_WAVE_COUNT) {
        p->wave = AUD_WAVE_SINE;
    }
    p->filter = p->filter ? 1 : 0;
    p->lfo_targets &= 7;
    for (int i = 0; i < AUD_P_COUNT; i++) {
        p->p[i] = aud_param_clamp(i, p->p[i]);
    }
    p->key_track = aud_clampi(p->key_track, 0, AUD_ONE);
    p->lfo_cutoff = aud_clampi(p->lfo_cutoff, 0, AUD_ONE);
}

int aud_patch_new(aud_engine_t *e, const aud_patch_t *p)
{
    int h = handle_alloc(e);
    if (h < 0) {
        return h;
    }
    int s = patch_slot(e);
    if (s < 0) {
        e->handle_used[h] = 0;
        return s;
    }
    e->cold->patches[s] = *p;
    patch_sanitize(&e->cold->patches[s]);
    atomic_store_explicit(&e->handles[h], (uint16_t)(H_PATCH | s), memory_order_release);
    return h;
}

int aud_patch_set(aud_engine_t *e, int h, const aud_patch_t *p)
{
    if (h <= 0 || h >= AUD_HANDLES) {
        return AUD_ERR_ARG;
    }
    uint16_t old = atomic_load(&e->handles[h]);
    if (!(old & H_PATCH)) {
        return AUD_ERR_ARG;
    }
    int s = patch_slot(e);
    if (s < 0) {
        return s;
    }
    e->cold->patches[s] = *p;
    patch_sanitize(&e->cold->patches[s]);
    atomic_store_explicit(&e->handles[h], (uint16_t)(H_PATCH | s), memory_order_release);
    int o = old & 0xFF;
    e->cold->patch_retired[o] = atomic_load(&e->pub_blocks);
    e->cold->patch_used[o] = SLOT_RETIRED;
    return h;
}

int aud_bank_new(aud_engine_t *e)
{
    if (e->nbanks >= AUD_BANKS) {
        return AUD_ERR_FULL;
    }
    int h = handle_alloc(e);
    if (h < 0) {
        return h;
    }
    int b = e->nbanks++;
    atomic_store(&e->cold->bank_count[b], 0);
    atomic_store_explicit(&e->handles[h], (uint16_t)(H_BANK | b), memory_order_release);
    return h;
}

bool aud_handle_is_bank(const aud_engine_t *e, int h)
{
    return h > 0 && h < AUD_HANDLES && (atomic_load(&((aud_engine_t *)e)->handles[h]) & H_BANK);
}

int aud_bank_add(aud_engine_t *e, int h, const aud_sample_t *s)
{
    if (!aud_handle_is_bank(e, h) || !s->data || !s->len) {
        return AUD_ERR_ARG;
    }
    int b = atomic_load(&e->handles[h]) & 0xFF;
    aud_bank_t *bank = &e->cold->banks[b];
    uint8_t n = atomic_load(&e->cold->bank_count[b]);
    if (n >= AUD_BANK_SAMPLES) {
        return AUD_ERR_FULL;
    }
    aud_sample_t *d = &bank->s[n];
    *d = *s;
    if (d->loop_end > d->len) {
        d->loop_end = d->len;
    }
    if (d->loop_start >= d->loop_end) {
        d->loop_start = d->loop_end = 0;
    }
    if (d->key_lo > d->key_hi || d->key_hi > 127) {
        d->key_lo = 0, d->key_hi = 127;
    }
    if (d->vel_lo > d->vel_hi || d->vel_hi > 127) {
        d->vel_lo = 0, d->vel_hi = 127;
    }
    if (!d->rate) {
        d->rate = e->rate;
    }
    d->root &= 127;
    d->fine = (int16_t)aud_clampi(d->fine, -100, 100);
    d->volume = aud_clampi(d->volume, 0, 2 * AUD_ONE);
    for (int i = 0; i < 4; i++) {
        d->adsr[i] = aud_clampi(d->adsr[i], 0, i == 2 ? AUD_ONE : 60000);
    }
    bank->count = n + 1;
    atomic_store_explicit(&e->cold->bank_count[b], (uint8_t)(n + 1), memory_order_release);
    return n;
}

int aud_program_set(aud_engine_t *e, int bank, int program, int handle)
{
    aud_cold_t *c = e->cold;
    if (program < 0 || program > 127 || bank < 0 || bank > 127 || handle <= 0 || handle >= AUD_HANDLES) {
        return AUD_ERR_ARG;
    }
    uint8_t n = atomic_load(&c->nprograms);
    for (int i = 0; i < n; i++) {
        if (c->programs[i].bank == bank && c->programs[i].program == program) {
            c->programs[i].handle = (uint16_t)handle;   // a 16-bit store: the mixer sees old or new
            return AUD_OK;
        }
    }
    if (n >= AUD_PROGRAMS) {
        return AUD_ERR_FULL;
    }
    c->programs[n].bank = (uint8_t)bank;
    c->programs[n].program = (uint8_t)program;
    c->programs[n].handle = (uint16_t)handle;
    atomic_store_explicit(&c->nprograms, (uint8_t)(n + 1), memory_order_release);
    return AUD_OK;
}

// ---------------------------------------------------------------------------
// Producer: events

uint32_t aud_now(const aud_engine_t *e)
{
    return atomic_load_explicit(&((aud_engine_t *)e)->pub_samples, memory_order_acquire);
}

uint32_t aud_ms_to_samples(const aud_engine_t *e, uint32_t ms)
{
    return aud_ms_samples(e->rate, ms > 600000 ? 600000 : ms);
}

static uint32_t ring_room(aud_engine_t *e)
{
    uint32_t tail = atomic_load_explicit(&e->ring.tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&e->ring.head, memory_order_acquire);
    return AUD_QUEUE - (tail - head);
}

// Post n events at once, or none (the call never blocks; FR-8).
static bool post_n(aud_engine_t *e, const aud_ev_t *ev, int n)
{
    if (ring_room(e) < (uint32_t)n) {
        atomic_fetch_add(&e->c_dropped, (uint32_t)n);
        return false;
    }
    uint32_t tail = atomic_load_explicit(&e->ring.tail, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        e->ring.buf[(tail + i) % AUD_QUEUE] = ev[i];
    }
    atomic_store_explicit(&e->ring.tail, tail + n, memory_order_release);
    return true;
}

static bool post(aud_engine_t *e, uint8_t type, int ch, int a, int b, uint32_t serial, int32_t value, uint32_t delay)
{
    aud_ev_t ev = {
        .time = aud_now(e) + delay, .type = type, .ch = (uint8_t)ch, .a = (uint8_t)a, .b = (uint8_t)b,
        .serial = serial, .value = value,
    };
    return post_n(e, &ev, 1);
}

#define CH_OK(ch)   ((ch) >= 0 && (ch) < AUD_CHANNELS)
#define B7(v)       ((v) < 0 ? 0 : (v) > 127 ? 127 : (v))

bool aud_assign(aud_engine_t *e, int ch, int handle)
{
    if (!CH_OK(ch) || handle < 0 || handle >= AUD_HANDLES) {
        return false;
    }
    return post(e, EV_ASSIGN, ch, 0, 0, 0, handle, 0);
}

uint32_t aud_note_on(aud_engine_t *e, int ch, int note, int vel, uint32_t delay, const aud_pv_t *bundle, int n)
{
    if (!CH_OK(ch) || note < 0 || note > 127 || n < 0 || n > 8) {
        return 0;
    }
    if (vel <= 0) {
        aud_note_off(e, ch, note, delay);
        return 0;
    }
    uint32_t s = (e->serial + 1) & ~AUD_SERIAL_MIXER;
    if (!s) {
        s = 1;
    }
    aud_ev_t ev[9];
    uint32_t t = aud_now(e) + delay;
    ev[0] = (aud_ev_t){ .time = t, .type = EV_NOTE_ON, .ch = (uint8_t)ch, .a = (uint8_t)note,
                        .b = (uint8_t)B7(vel), .serial = s };
    int k = 1;
    for (int i = 0; i < n; i++) {
        if (bundle[i].param < AUD_P_COUNT) {
            ev[k++] = (aud_ev_t){ .time = t, .type = EV_NOTE_PARAM_B, .ch = (uint8_t)ch, .a = bundle[i].param,
                                  .serial = s, .value = aud_param_clamp(bundle[i].param, bundle[i].value) };
        }
    }
    if (!post_n(e, ev, k)) {
        return 0;
    }
    e->serial = s;
    return s;
}

bool aud_note_off(aud_engine_t *e, int ch, int note, uint32_t delay)
{
    return CH_OK(ch) && note >= 0 && note < 128 && post(e, EV_NOTE_OFF, ch, note, 0, 0, 0, delay);
}

bool aud_note_off_h(aud_engine_t *e, uint32_t h, uint32_t delay)
{
    return h && post(e, EV_NOTE_OFF_H, 0, 0, 0, h, 0, delay);
}

bool aud_cc(aud_engine_t *e, int ch, int cc, int value, uint32_t delay)
{
    return CH_OK(ch) && cc >= 0 && cc < 128 && post(e, EV_CC, ch, cc, B7(value), 0, 0, delay);
}

bool aud_bend(aud_engine_t *e, int ch, int v, uint32_t delay)
{
    return CH_OK(ch) && post(e, EV_BEND, ch, 0, 0, 0, aud_clampi(v, -8192, 8191), delay);
}

bool aud_pressure(aud_engine_t *e, int ch, int value, uint32_t delay)
{
    return CH_OK(ch) && post(e, EV_PRESSURE, ch, 0, B7(value), 0, 0, delay);
}

bool aud_poly_pressure(aud_engine_t *e, int ch, int note, int value, uint32_t delay)
{
    return CH_OK(ch) && note >= 0 && note < 128 && post(e, EV_POLY, ch, note, B7(value), 0, 0, delay);
}

bool aud_program(aud_engine_t *e, int ch, int program, uint32_t delay)
{
    return CH_OK(ch) && program >= 0 && program < 128 && post(e, EV_PROGRAM, ch, program, 0, 0, 0, delay);
}

bool aud_param(aud_engine_t *e, int ch, int p, int32_t value, uint32_t delay)
{
    return CH_OK(ch) && p >= 0 && p < AUD_P_COUNT && post(e, EV_PARAM, ch, p, 0, 0, aud_param_clamp(p, value), delay);
}

bool aud_note_param(aud_engine_t *e, uint32_t h, int p, int32_t value, uint32_t delay)
{
    return h && p >= 0 && p < AUD_P_COUNT && post(e, EV_NOTE_PARAM, 0, p, 0, h, aud_param_clamp(p, value), delay);
}

bool aud_note_ramp(aud_engine_t *e, uint32_t h, int p, int32_t value, uint32_t ms, uint32_t delay)
{
    if (!h || p < 0 || p >= AUD_P_COUNT) {
        return false;
    }
    ms = ms > 65535 ? 65535 : ms;
    return post(e, EV_NOTE_RAMP, (int)(ms & 0xFF), p, (int)(ms >> 8), h, aud_param_clamp(p, value), delay);
}

bool aud_note_bend(aud_engine_t *e, uint32_t h, int32_t semis_256, uint32_t delay)
{
    return h && post(e, EV_NOTE_BEND, 0, 0, 0, h, aud_clampi(semis_256, -24 * 256, 24 * 256), delay);
}

bool aud_note_pressure(aud_engine_t *e, uint32_t h, int32_t q16, uint32_t delay)
{
    return h && post(e, EV_NOTE_PRESSURE, 0, 0, 0, h, aud_clampi(q16, 0, AUD_ONE), delay);
}

bool aud_pressure_target(aud_engine_t *e, int ch, int p, int32_t amount)
{
    return CH_OK(ch) && p >= 0 && p < AUD_P_COUNT && post(e, EV_PTARGET, ch, p, 0, 0, amount, 0);
}

bool aud_map_cc(aud_engine_t *e, int ch, int cc, int p, int32_t min, int32_t max)
{
    if (!CH_OK(ch) || cc < 0 || cc > 127 || p < 0 || p >= AUD_P_COUNT || e->nmap_defs >= AUD_MAP_DEFS) {
        return false;
    }
    int d = e->nmap_defs;
    e->cold->map_defs[d] = (aud_map_def_t){ .min = aud_param_clamp(p, min), .max = aud_param_clamp(p, max),
                                            .param = (uint8_t)p };
    if (!post(e, EV_MAP_CC, ch, cc, 0, 0, d, 0)) {
        return false;
    }
    e->nmap_defs++;
    return true;
}

bool aud_mpe(aud_engine_t *e, int first, int last)
{
    return post(e, EV_MPE, 0, aud_clampi(first, 0, 15), aud_clampi(last, 0, 15), 0, 0, 0);
}

bool aud_all_off(aud_engine_t *e, bool kill)
{
    return post(e, EV_ALL_OFF, 0, kill, 0, 0, 0, 0);
}

bool aud_midi(aud_engine_t *e, uint8_t status, uint8_t d1, uint8_t d2, uint32_t delay)
{
    int ch = status & 15;
    d1 &= 127;
    d2 &= 127;
    switch (status & 0xF0) {
    case 0x80: return aud_note_off(e, ch, d1, delay);
    case 0x90: return d2 ? aud_note_on(e, ch, d1, d2, delay, NULL, 0) != 0 : aud_note_off(e, ch, d1, delay);
    case 0xA0: return aud_poly_pressure(e, ch, d1, d2, delay);
    case 0xB0: return aud_cc(e, ch, d1, d2, delay);
    case 0xC0: return aud_program(e, ch, d1, delay);
    case 0xD0: return aud_pressure(e, ch, d1, delay);
    case 0xE0: return aud_bend(e, ch, (d2 << 7 | d1) - 8192, delay);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Mixer: pending events

static bool is_param_ev(uint8_t t)
{
    return t == EV_CC || t == EV_BEND || t == EV_PRESSURE || t == EV_POLY || t == EV_PARAM ||
           t == EV_NOTE_PARAM || t == EV_NOTE_BEND || t == EV_NOTE_PRESSURE;
}

// Same target: a later value replaces it (FR-34).
static bool same_target(const aud_ev_t *a, const aud_ev_t *b)
{
    if (a->type != b->type) {
        return false;
    }
    switch (a->type) {
    case EV_CC:
        // Controllers with side effects (data entry, pedals, selects) must all arrive.
        return a->ch == b->ch && a->a == b->a && (a->a == 1 || a->a == 7 || a->a == 10 || a->a == 11 ||
               (a->a >= 70 && a->a <= 78) || a->a == 74 || a->a == 92 || a->a == 94);
    case EV_BEND: case EV_PRESSURE:
        return a->ch == b->ch;
    case EV_POLY:
        return a->ch == b->ch && a->a == b->a;
    case EV_PARAM:
        return a->ch == b->ch && a->a == b->a;
    case EV_NOTE_PARAM:
        return a->serial == b->serial && a->a == b->a;
    case EV_NOTE_BEND: case EV_NOTE_PRESSURE:
        return a->serial == b->serial;
    }
    return false;
}

static void pending_add(aud_engine_t *e, const aud_ev_t *ev)
{
    // Coalesce: a parameter event for the same target inside the same chunk.
    if (is_param_ev(ev->type)) {
        for (int i = e->npending - 1; i >= 0; i--) {
            aud_ev_t *p = &e->pending[i];
            if (same_target(p, ev) && (p->time / AUD_CHUNK) == (ev->time / AUD_CHUNK)) {
                p->value = ev->value;
                p->b = ev->b;
                return;
            }
        }
    }
    if (e->npending >= AUD_PENDING) {
        atomic_fetch_add(&e->c_dropped, 1);
        return;
    }
    // Sorted by time, stable: after every event at the same or an earlier time.
    int i = e->npending;
    while (i > 0 && aud_time_before(ev->time, e->pending[i - 1].time)) {
        e->pending[i] = e->pending[i - 1];
        i--;
    }
    e->pending[i] = *ev;
    e->npending++;
}

static void take_events(aud_engine_t *e)
{
    uint32_t head = atomic_load_explicit(&e->ring.head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&e->ring.tail, memory_order_acquire);
    while (head != tail) {
        aud_ev_t ev = e->ring.buf[head % AUD_QUEUE];
        head++;
        pending_add(e, &ev);
    }
    atomic_store_explicit(&e->ring.head, head, memory_order_release);
}

static int pending_pop_due(aud_engine_t *e)
{
    int n = 0;
    while (n < e->npending && !aud_time_before(e->samples, e->pending[n].time)) {
        aud_apply_event(e, &e->pending[n]);
        n++;
        if (e->npending == 0) {             // stop{all} dropped them while applying
            return 0;
        }
    }
    if (n) {
        memmove(e->pending, e->pending + n, (size_t)(e->npending - n) * sizeof(aud_ev_t));
        e->npending -= n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Mixer: master stage

#define LIM_KNEE    24576
#define LIM_MAX     32767

static inline int32_t soft_clip(int32_t y, uint32_t *clipped)
{
    int32_t a = y < 0 ? -y : y;
    if (a <= LIM_KNEE) {
        return y;
    }
    // Quadratic knee: slope 1 at the knee, 0 at LIM_MAX (2 * (MAX - KNEE) above it).
    int32_t u = a - LIM_KNEE, r = LIM_MAX - LIM_KNEE;
    int32_t o;
    if (u >= 2 * r) {
        o = LIM_MAX;
        (*clipped)++;
    } else {
        o = LIM_KNEE + u - (int32_t)(((int64_t)u * u) / (4 * r));
    }
    return y < 0 ? -o : o;
}

static void master(aud_engine_t *e, int16_t *out, int frames)
{
    uint32_t clipped = 0;
    // Release: back to unity over ~100 ms.
    uint32_t rel = (uint32_t)((uint64_t)AUD_ONE * AUD_CHUNK * 10 / e->rate);
    for (int off = 0; off < frames; off += AUD_CHUNK) {
        int n = frames - off < AUD_CHUNK ? frames - off : AUD_CHUNK;
        int32_t *l = e->acc_l + off, *r = e->acc_r + off;
        // Master gain moves to its target across the chunk.
        int32_t g0 = (int32_t)e->master, g1 = (int32_t)e->master_target;
        // Peak after the master gain, for the limiter.
        int32_t peak = 0;
        for (int i = 0; i < n; i++) {
            int32_t a = l[i] < 0 ? -l[i] : l[i];
            if (a > peak) {
                peak = a;
            }
            if (e->nch == 2) {
                a = r[i] < 0 ? -r[i] : r[i];
                if (a > peak) {
                    peak = a;
                }
            }
        }
        int64_t pk = ((int64_t)peak * (g0 > g1 ? g0 : g1)) >> 16;
        uint32_t lg = e->lim_gain + rel;
        if (lg > AUD_ONE) {
            lg = AUD_ONE;
        }
        if (pk > LIM_MAX) {
            uint32_t need = (uint32_t)(((int64_t)LIM_MAX << 16) / pk);
            if (need < lg) {
                lg = need;          // instant attack
            }
        }
        int32_t lim0 = (int32_t)e->lim_gain, lim1 = (int32_t)lg;
        e->lim_gain = lg;
        for (int i = 0; i < n; i++) {
            int32_t g = g0 + (int32_t)(((int64_t)(g1 - g0) * (i + 1)) / n);
            int32_t lm = lim0 > lim1 ? lim1 : lim0 + (int32_t)(((int64_t)(lim1 - lim0) * (i + 1)) / n);
            int32_t gg = (int32_t)(((int64_t)g * lm) >> 16);     // Q16
            for (int c = 0; c < e->nch; c++) {
                int32_t x = c ? r[i] : l[i];
                int32_t y = (int32_t)(((int64_t)x * gg) >> 8);   // Q8
                if (e->dither) {
                    uint32_t s = e->dither_rng;
                    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
                    e->dither_rng = s;
                    y += (int32_t)(s & 0xFF) - (int32_t)((s >> 8) & 0xFF);  // TPDF, +-1 LSB
                }
                y = soft_clip(y >> 8, &clipped);
                out[(off + i) * e->nch + c] = (int16_t)y;
            }
        }
        e->master = (uint32_t)g1;
    }
    if (clipped) {
        atomic_fetch_add(&e->c_clipped, clipped);
    }
}

// ---------------------------------------------------------------------------
// Mixer: render

static uint32_t samples_to_tick(const aud_engine_t *e, uint32_t tick)
{
    uint64_t need = (uint64_t)(tick - e->clock) * e->spt;
    if (need <= e->tick_pos) {
        return 0;
    }
    need -= e->tick_pos;
    return (uint32_t)((need + 65535) >> 16);
}

static void advance(aud_engine_t *e, int n)
{
    e->samples += (uint32_t)n;
    e->tick_pos += (uint32_t)n << 16;
    while (e->tick_pos >= e->spt) {
        e->tick_pos -= e->spt;
        e->clock++;
    }
}

void aud_render(aud_engine_t *e, int16_t *out, int frames)
{
    if (frames > AUD_BLOCK_MAX) {
        frames = AUD_BLOCK_MAX;
    }
    atomic_store_explicit(&e->pub_block_frames, (uint32_t)frames, memory_order_relaxed);
    take_events(e);
    aud_tl_take_cmds(e);
    aud_seq_feed(e);
    memset(e->acc_l, 0, sizeof(int32_t) * frames);
    if (e->nch == 2) {
        memset(e->acc_r, 0, sizeof(int32_t) * frames);
    }
    int pos = 0;
    while (pos < frames) {
        aud_tl_process(e, e->clock);
        pending_pop_due(e);
        int n = frames - pos < AUD_CHUNK ? frames - pos : AUD_CHUNK;
        if (e->npending) {
            uint32_t d = e->pending[0].time - e->samples;
            if (d < (uint32_t)n) {
                n = (int)d;
            }
        }
        uint32_t nt = aud_tl_next(e);
        if (nt != UINT32_MAX) {
            uint32_t d = samples_to_tick(e, nt);
            if (d < (uint32_t)n) {
                n = (int)d;
            }
        }
        if (n <= 0) {
            n = 1;          // due work was handled; never stall
        }
        e->render_pos = pos;
        for (int i = 0; i < AUD_VOICES; i++) {
            if (e->voices[i].env != ENV_OFF) {
                aud_voice_render(e, &e->voices[i], n);
            }
        }
        advance(e, n);
        pos += n;
    }
    master(e, out, frames);
    uint32_t active = 0;
    for (int i = 0; i < AUD_VOICES; i++) {
        active += e->voices[i].env != ENV_OFF;
    }
    atomic_store_explicit(&e->c_voices, active, memory_order_relaxed);
    bool busy = active || e->npending || e->ncmd || e->seq.playing || e->tl_free < AUD_TL_NODES;
    e->idle = busy ? 0 : e->idle + (uint32_t)frames;
    e->blocks++;
    publish(e);
}

// ---------------------------------------------------------------------------
// Producer: clock and grid

uint32_t aud_clock(const aud_engine_t *e)
{
    return atomic_load_explicit(&((aud_engine_t *)e)->pub_clock, memory_order_acquire);
}

uint32_t aud_heard(const aud_engine_t *e, uint32_t latency)
{
    aud_engine_t *m = (aud_engine_t *)e;
    uint32_t clock = atomic_load(&m->pub_clock), spt = atomic_load(&m->pub_spt);
    uint32_t back = spt ? (uint32_t)(((uint64_t)latency << 16) / spt) : 0;
    return back > clock ? 0 : clock - back;
}

void aud_grid(const aud_engine_t *e, aud_grid_t *g)
{
    aud_engine_t *m = (aud_engine_t *)e;
    g->ppqn = atomic_load(&m->pub_ppqn);
    g->beat = g->ppqn;
    g->bar = g->ppqn * atomic_load(&m->pub_beats);
    g->bpm_q16 = atomic_load(&m->pub_bpm);
}
