// Voices: allocation (FR-9), the layered parameters (FR-24), envelopes,
// oscillators, the filter and the sampler, rendered a segment at a time.
//
// A segment is at most AUD_CHUNK (64) frames and ends early at an event or a
// tick with work, so events land on their sample. Parameters are worked out
// once per segment and gains ramp linearly across it, so nothing clicks
// (FR-26).

#include <string.h>
#include "aud_int.h"
#include "aud_tables.h"

static inline int32_t sine_at(uint32_t ph)
{
    uint32_t i = ph >> 24, f = (ph >> 8) & 0xFFFF;
    int32_t a = aud_sine[i], b = aud_sine[i + 1];
    return a + (((b - a) * (int32_t)f) >> 16);
}

// ---------------------------------------------------------------------------
// Parameters: patch, then channel, then note (FR-24, FR-25)

int32_t aud_eff(const aud_engine_t *e, const aud_voice_t *v, int p)
{
    const aud_chan_t *c = &e->chans[v->ch];
    uint32_t bit = 1u << p;
    int32_t val = (v->note_set & bit) ? v->note_abs[p] : (c->chan_set & bit) ? c->chan_abs[p] : v->patch.p[p];
    bool mult = aud_param_mult_kind(p);
    if (mult) {
        val = (int32_t)(((int64_t)val * c->chan_rel[p]) >> 16);
    } else {
        val += c->chan_rel[p];
    }
    if (c->ptarget == p && c->pamount) {
        int32_t pr = v->pressure;
        int32_t cp = c->chan_pressure * 516;            // 127 -> ~1.0
        if (cp > pr) {
            pr = cp;
        }
        int32_t amt = aud_mul16(c->pamount, pr);
        val = mult ? (int32_t)(((int64_t)val * (AUD_ONE + amt)) >> 16) : val + amt;
    }
    switch (p) {
    case AUD_P_VIB_DEPTH:
        val += c->mod * (AUD_ONE / 2) / 127;            // the mod wheel adds up to half a semitone (FR-13)
        break;
    case AUD_P_VOLUME:
        if (c->soft) {
            val >>= 1;                                  // soft pedal: about -6 dB
        }
        break;
    case AUD_P_CUTOFF:
        if (c->soft) {
            val -= AUD_ONE / 20;
        }
        break;
    }
    return aud_param_clamp(p, val);
}

// ---------------------------------------------------------------------------
// Envelope

static int32_t env_rate(const aud_engine_t *e, int32_t ms, int32_t min_ms)
{
    if (ms < min_ms) {
        ms = min_ms;
    }
    uint32_t n = aud_ms_samples(e->rate, (uint32_t)ms);
    return AUD_ENV_ONE / (int32_t)(n ? n : 1);
}

static void enter_decay(aud_engine_t *e, aud_voice_t *v)
{
    v->env = ENV_DECAY;
    v->rate = env_rate(e, aud_eff(e, v, AUD_P_DECAY), 1);
}

static void env_advance(aud_engine_t *e, aud_voice_t *v, int n)
{
    while (n > 0) {
        switch (v->env) {
        case ENV_ATTACK: {
            int32_t need = (AUD_ENV_ONE - v->level + v->rate - 1) / v->rate;
            if (need <= n) {
                v->level = AUD_ENV_ONE;
                n -= need;
                enter_decay(e, v);
            } else {
                v->level += v->rate * n;
                n = 0;
            }
            break;
        }
        case ENV_DECAY: {
            int32_t sus = aud_eff(e, v, AUD_P_SUSTAIN) << 8;
            if (v->level <= sus) {
                v->env = ENV_SUSTAIN;
                break;
            }
            int32_t need = (v->level - sus + v->rate - 1) / v->rate;
            if (need <= n) {
                v->level = sus;
                v->env = ENV_SUSTAIN;
                n = 0;
            } else {
                v->level -= v->rate * n;
                n = 0;
            }
            break;
        }
        case ENV_SUSTAIN: {
            // Follows sustain changes at the decay rate, so they never click.
            int32_t sus = aud_eff(e, v, AUD_P_SUSTAIN) << 8;
            int32_t d = v->rate * n;
            v->level = v->level > sus ? (v->level - d < sus ? sus : v->level - d)
                                      : (v->level + d > sus ? sus : v->level + d);
            if (sus == 0 && v->level == 0) {
                v->env = ENV_OFF;
            }
            n = 0;
            break;
        }
        case ENV_RELEASE:
        case ENV_KILL:
            if (v->level <= v->rate * n) {
                v->level = 0;
                v->env = ENV_OFF;
            } else {
                v->level -= v->rate * n;
            }
            n = 0;
            break;
        default:
            n = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Ramps (FR-33): move one note-layer value over a time, in the mixer.

void aud_voice_clear_ramps(aud_engine_t *e, aud_voice_t *v)
{
    for (int i = 0; i < AUD_RAMPS_VOICE; i++) {
        if (v->ramps[i].param != 0xFF) {
            v->ramps[i].param = 0xFF;
            e->ramps_total--;
        }
    }
    v->nramps = 0;
}

static void ramps_advance(aud_engine_t *e, aud_voice_t *v, int n)
{
    for (int i = 0; i < AUD_RAMPS_VOICE && v->nramps; i++) {
        aud_ramp_t *r = &v->ramps[i];
        if (r->param == 0xFF) {
            continue;
        }
        r->done += (uint32_t)n;
        if (r->done >= r->total) {
            v->note_abs[r->param] = r->to;
            r->param = 0xFF;
            v->nramps--;
            e->ramps_total--;
        } else {
            v->note_abs[r->param] = r->from + (int32_t)(((int64_t)(r->to - r->from) * r->done) / r->total);
        }
    }
}

static int32_t base_value(const aud_engine_t *e, const aud_voice_t *v, int p)
{
    const aud_chan_t *c = &e->chans[v->ch];
    return (v->note_set & (1u << p)) ? v->note_abs[p] : (c->chan_set & (1u << p)) ? c->chan_abs[p] : v->patch.p[p];
}

static void cancel_ramp(aud_engine_t *e, aud_voice_t *v, int p)
{
    for (int i = 0; i < AUD_RAMPS_VOICE; i++) {
        if (v->ramps[i].param == p) {
            v->ramps[i].param = 0xFF;
            v->nramps--;
            e->ramps_total--;
        }
    }
}

void aud_voice_param(aud_engine_t *e, aud_voice_t *v, int p, int32_t value)
{
    cancel_ramp(e, v, p);
    v->note_abs[p] = aud_param_clamp(p, value);
    v->note_set |= 1u << p;
}

void aud_voice_ramp(aud_engine_t *e, aud_voice_t *v, int p, int32_t value, uint32_t ms)
{
    cancel_ramp(e, v, p);
    uint32_t total = aud_ms_samples(e->rate, ms);
    if (!total) {
        aud_voice_param(e, v, p, value);
        return;
    }
    if (v->nramps >= AUD_RAMPS_VOICE || e->ramps_total >= AUD_RAMPS_TOTAL) {
        atomic_fetch_add(&e->c_ramps, 1);
        return;
    }
    for (int i = 0; i < AUD_RAMPS_VOICE; i++) {
        aud_ramp_t *r = &v->ramps[i];
        if (r->param == 0xFF) {
            r->from = base_value(e, v, p);
            r->to = aud_param_clamp(p, value);
            r->total = total;
            r->done = 0;
            r->param = (uint8_t)p;
            v->nramps++;
            e->ramps_total++;
            v->note_abs[p] = r->from;
            v->note_set |= 1u << p;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Starting and stopping notes

aud_voice_t *aud_voice_find(aud_engine_t *e, uint32_t serial)
{
    if (!serial) {
        return NULL;
    }
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->serial == serial && (v->env != ENV_OFF || v->pend)) {
            return v;
        }
    }
    return NULL;
}

static const aud_sample_t *pick_sample(const aud_engine_t *e, int bank, int note, int vel, int layer)
{
    const aud_cold_t *c = e->cold;
    int n = atomic_load_explicit(&((aud_cold_t *)c)->bank_count[bank], memory_order_acquire);
    const aud_sample_t *any = NULL;
    for (int i = 0; i < n; i++) {
        const aud_sample_t *s = &c->banks[bank].s[i];
        if (note < s->key_lo || note > s->key_hi || vel < s->vel_lo || vel > s->vel_hi) {
            continue;
        }
        if (layer < 0 ? s->layer == 0 : s->layer == layer) {
            return s;
        }
        if (!any) {
            any = s;
        }
    }
    return any;
}

// Copy the channel's instrument into the voice and start its envelope from
// the voice's current level (0 for a fresh voice: no click either way).
static bool voice_begin(aud_engine_t *e, aud_voice_t *v)
{
    aud_chan_t *c = &e->chans[v->ch];
    uint16_t m = c->handle ? atomic_load_explicit(&e->handles[c->handle], memory_order_acquire) : 0;
    if (m & H_PATCH) {
        v->patch = e->cold->patches[m & 0xFF];
        v->smp = NULL;
        v->is_sample = 0;
        v->wave = v->patch.wave;
        if (c->variation != 0xFF) {
            v->wave = (uint8_t)(c->variation * AUD_WAVE_COUNT / 128);
        }
        if (c->wave_override != 0xFF) {
            v->wave = c->wave_override;
        }
    } else if (m & H_BANK) {
        const aud_sample_t *s = pick_sample(e, m & 0xFF, v->note, v->vel, c->variation == 0xFF ? -1 : c->variation >> 4);
        if (!s) {
            return false;
        }
        aud_patch_default(&v->patch);
        v->patch.p[AUD_P_VOLUME] = s->volume;
        v->patch.p[AUD_P_ATTACK] = s->adsr[0];
        v->patch.p[AUD_P_DECAY] = s->adsr[1];
        v->patch.p[AUD_P_SUSTAIN] = s->adsr[2];
        v->patch.p[AUD_P_RELEASE] = s->adsr[3];
        v->smp = s;
        v->is_sample = 1;
    } else {
        return false;
    }
    if (c->filter_override != 0xFF) {
        v->patch.filter = c->filter_override;
    }
    if (c->lfo_targets_override != 0xFF) {
        v->patch.lfo_targets = c->lfo_targets_override;
    }
    if (c->key_track_override >= 0) {
        v->patch.key_track = c->key_track_override;
    }
    v->phase = v->frac = 0;
    v->noise = 0xACE1u ^ v->note;
    v->noise_ph = 0;
    v->lp = v->bp = 0;
    v->age = 0;
    v->env = ENV_ATTACK;
    v->rate = env_rate(e, aud_eff(e, v, AUD_P_ATTACK), AUD_MIN_ATTACK_MS);
    v->order = ++e->voice_order;
    // Portamento (CC 5, 65, 84)
    v->glide = 0;
    int32_t porta = aud_eff(e, v, AUD_P_PORTA);
    uint8_t src = c->porta_src != 0xFF ? c->porta_src : c->last_note;
    if ((c->porta_on || c->porta_src != 0xFF) && porta > 0 && src != 0xFF && src != v->note) {
        v->glide = ((int32_t)src - v->note) * 256;
        uint32_t ns = aud_ms_samples(e->rate, (uint32_t)porta);
        int32_t g = v->glide < 0 ? -v->glide : v->glide;
        v->glide_step = (int32_t)(((int64_t)g << 8) / (ns ? ns : 1));
        if (!v->glide_step) {
            v->glide_step = 1;
        }
    }
    c->porta_src = 0xFF;
    c->last_note = v->note;
    AUD_TRACE(e, AUD_TRACE_START, v);
    return true;
}

static void set_identity(aud_voice_t *v, int ch, int note, int vel, uint32_t serial)
{
    v->ch = (uint8_t)ch;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->serial = serial;
    v->sustained = v->sostenuto = 0;
    v->from_tl = v->song = v->end_armed = 0;
    v->note_set = 0;
    v->bend_note = 0;
    v->pressure = 0;
}

uint32_t aud_note_start(aud_engine_t *e, int ch, int note, int vel, uint32_t serial, aud_voice_t **out)
{
    aud_chan_t *c = &e->chans[ch];
    if (out) {
        *out = NULL;
    }
    if (!c->handle) {
        return 0;                           // unassigned channels are silent (FR-19)
    }
    bool mpe = e->mpe_first <= e->mpe_last && ch >= e->mpe_first && ch <= e->mpe_last;
    aud_voice_t *v = NULL;
    // Mono (CC 126), legato (CC 68) and MPE channels keep one sounding voice.
    if (c->mono || c->legato || mpe) {
        for (int i = 0; i < AUD_VOICES; i++) {
            aud_voice_t *w = &e->voices[i];
            if (w->ch == ch && w->env >= ENV_ATTACK && w->env <= ENV_SUSTAIN && !w->pend) {
                v = w;
                break;
            }
        }
        if (v && c->legato && !v->is_sample) {
            // Legato: new pitch, same envelope.
            int32_t from = v->note;
            aud_voice_clear_ramps(e, v);
            set_identity(v, ch, note, vel, serial);
            if (c->porta_on && aud_eff(e, v, AUD_P_PORTA) > 0) {
                v->glide = (from - note) * 256;
                uint32_t ns = aud_ms_samples(e->rate, (uint32_t)aud_eff(e, v, AUD_P_PORTA));
                int32_t g = v->glide < 0 ? -v->glide : v->glide;
                v->glide_step = (int32_t)(((int64_t)g << 8) / (ns ? ns : 1)) + 1;
            }
            c->last_note = (uint8_t)note;
            if (out) {
                *out = v;
            }
            return serial;
        }
    }
    // Re-triggering the same note reuses its voice.
    if (!v) {
        for (int i = 0; i < AUD_VOICES; i++) {
            aud_voice_t *w = &e->voices[i];
            if (w->ch == ch && w->note == note && w->env != ENV_OFF && w->env != ENV_KILL && !w->pend) {
                v = w;
                break;
            }
        }
    }
    if (v) {
        aud_voice_clear_ramps(e, v);
        int32_t level = v->level;
        set_identity(v, ch, note, vel, serial);
        if (!voice_begin(e, v)) {
            aud_voice_release(e, v);
            return 0;
        }
        v->level = level;                   // attack from where it is
    } else {
        for (int i = 0; i < AUD_VOICES; i++) {
            if (e->voices[i].env == ENV_OFF && !e->voices[i].pend) {
                v = &e->voices[i];
                break;
            }
        }
        if (!v) {
            // Steal: the quietest releasing voice, else the oldest.
            for (int i = 0; i < AUD_VOICES; i++) {
                aud_voice_t *w = &e->voices[i];
                if (w->env == ENV_RELEASE && !w->pend && (!v || w->level < v->level)) {
                    v = w;
                }
            }
            if (!v) {
                for (int i = 0; i < AUD_VOICES; i++) {
                    aud_voice_t *w = &e->voices[i];
                    if (!w->pend && (!v || (int32_t)(w->order - v->order) < 0)) {
                        v = w;
                    }
                }
            }
            if (!v) {
                v = &e->voices[0];
            }
            // Fade the old note for 2 ms, then start this one (no click).
            aud_voice_clear_ramps(e, v);
            if (v->env != ENV_KILL) {
                aud_voice_kill(e, v);
            }
            set_identity(v, ch, note, vel, serial);
            v->pend = 1;
            if (out) {
                *out = v;
            }
            return serial;
        }
        aud_voice_clear_ramps(e, v);
        set_identity(v, ch, note, vel, serial);
        v->level = 0;
        v->gl = v->gr = 0;
        if (!voice_begin(e, v)) {
            v->env = ENV_OFF;
            v->serial = 0;
            return 0;
        }
    }
    if (out) {
        *out = v;
    }
    return serial;
}

void aud_voice_release(aud_engine_t *e, aud_voice_t *v)
{
    if (v->env == ENV_OFF || v->env == ENV_RELEASE || v->env == ENV_KILL) {
        return;
    }
    v->env = ENV_RELEASE;
    v->rate = env_rate(e, aud_eff(e, v, AUD_P_RELEASE), AUD_MIN_RELEASE_MS);
    v->sustained = v->sostenuto = 0;
    v->end_armed = 0;
    AUD_TRACE(e, AUD_TRACE_RELEASE, v);
}

void aud_voice_kill(aud_engine_t *e, aud_voice_t *v)
{
    if (v->env == ENV_OFF) {
        return;
    }
    AUD_TRACE(e, AUD_TRACE_RELEASE, v);
    v->env = ENV_KILL;
    v->rate = env_rate(e, AUD_KILL_MS, AUD_KILL_MS);
    v->sustained = v->sostenuto = 0;
    v->end_armed = 0;
}

// Note-off for one voice, through the pedals.
void aud_voice_off(aud_engine_t *e, aud_voice_t *v)
{
    aud_chan_t *c = &e->chans[v->ch];
    if (v->env < ENV_ATTACK || v->env > ENV_SUSTAIN) {
        return;
    }
    if (c->sustain || v->sostenuto) {
        v->sustained = 1;
        v->end_armed = 0;
        return;
    }
    aud_voice_release(e, v);
}

void aud_note_stop(aud_engine_t *e, int ch, int note)
{
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->ch == ch && v->note == note) {
            if (v->pend) {
                v->pend = 0;                // never started: drop it with the fade
                v->serial = 0;
                continue;
            }
            aud_voice_off(e, v);
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering

static void osc(aud_voice_t *v, int16_t *t, int n, uint32_t step, uint32_t pw)
{
    uint32_t ph = v->phase;
    switch (v->wave) {
    case AUD_WAVE_SINE:
        for (int i = 0; i < n; i++, ph += step) {
            t[i] = (int16_t)sine_at(ph);
        }
        break;
    case AUD_WAVE_PULSE:
        for (int i = 0; i < n; i++, ph += step) {
            t[i] = ph < pw ? 32767 : -32767;
        }
        break;
    case AUD_WAVE_SAW:
        for (int i = 0; i < n; i++, ph += step) {
            t[i] = (int16_t)(ph >> 16);
        }
        break;
    case AUD_WAVE_TRI:
        for (int i = 0; i < n; i++, ph += step) {
            int32_t u = (int32_t)(ph >> 16);
            t[i] = (int16_t)(u < 32768 ? 2 * u - 32768 : 98303 - 2 * u);
        }
        break;
    default: {                          // noise: a 16-bit LFSR clocked at 16x the note frequency
        uint32_t lf = v->noise, np = v->noise_ph, ns = step << 4;
        for (int i = 0; i < n; i++) {
            uint32_t old = np;
            np += ns;
            if (np < old || ns >= 0x80000000u) {
                lf = (lf >> 1) ^ (-(lf & 1u) & 0xB400u);
            }
            t[i] = (int16_t)lf;
        }
        v->noise = lf;
        v->noise_ph = np;
        ph += step * (uint32_t)n;
        break;
    }
    }
    v->phase = ph;
}

// Returns false when a one-shot ran out (the voice ends).
static bool sampler(aud_voice_t *v, int16_t *t, int n, uint32_t step, bool loop)
{
    const aud_sample_t *s = v->smp;
    uint32_t idx = v->phase, fr = v->frac;
    uint32_t end = loop ? s->loop_end : s->len;
    uint32_t span = s->loop_end - s->loop_start;
    for (int i = 0; i < n; i++) {
        if (idx >= end) {
            if (!loop) {
                memset(t + i, 0, (size_t)(n - i) * sizeof(int16_t));
                v->phase = idx;
                return false;
            }
            while (idx >= end) {
                idx -= span;
            }
        }
        int32_t a = s->data[idx];
        uint32_t j = idx + 1;
        int32_t b = j < end ? s->data[j] : loop ? s->data[s->loop_start] : 0;
        t[i] = (int16_t)(a + (((b - a) * (int32_t)fr) >> 16));
        fr += step;
        idx += fr >> 16;
        fr &= 0xFFFF;
    }
    v->phase = idx;
    v->frac = fr;
    return true;
}

static uint32_t synth_step(const aud_engine_t *e, int32_t pitch)
{
    uint64_t f = ((uint64_t)AUD_F0_Q16 * aud_pitch_ratio_q16(pitch)) >> 16;     // Hz, Q16
    uint64_t st = (f << 16) / e->rate;
    return st > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)st;
}

void aud_voice_render(aud_engine_t *e, aud_voice_t *v, int n)
{
    aud_chan_t *c = &e->chans[v->ch];
    int16_t *t = e->tmp;
    int32_t *L = e->acc_l + e->render_pos, *R = e->acc_r + e->render_pos;
    int32_t gl0 = v->gl, gr0 = v->gr, gl1, gr1;
    bool alive = true;

    if (v->env == ENV_KILL) {
        env_advance(e, v, n);
        int32_t lv = v->level >> 8;
        gl1 = aud_mul16(v->kgl, lv);
        gr1 = aud_mul16(v->kgr, lv);
        if (v->is_sample) {
            alive = sampler(v, t, n, v->step, v->smp->loop_end > v->smp->loop_start);
        } else {
            osc(v, t, n, v->step, (uint32_t)aud_eff(e, v, AUD_P_PW) << 16);
        }
    } else {
        ramps_advance(e, v, n);
        // LFO
        int32_t rate = aud_eff(e, v, AUD_P_VIB_RATE);
        uint32_t lstep = (uint32_t)(((uint64_t)(uint32_t)rate << 16) / e->rate);
        v->lfo_phase += lstep * (uint32_t)n;
        int32_t lfo = sine_at(v->lfo_phase);                    // Q15
        int32_t fade = AUD_ONE;
        int32_t delay = aud_eff(e, v, AUD_P_VIB_DELAY);
        if (delay > 0) {
            uint32_t ds = aud_ms_samples(e->rate, (uint32_t)delay);
            fade = v->age >= ds ? AUD_ONE : (int32_t)(((uint64_t)v->age << 16) / (ds ? ds : 1));
        }
        uint8_t tg = v->patch.lfo_targets;
        // Pitch, in 1/256 semitone
        int32_t pitch = v->note * 256 + v->bend_note + c->tune_coarse + c->tune_fine + v->glide;
        pitch += (int32_t)(((int64_t)c->bend * c->bend_range) / 8192);
        if (e->mpe_first <= e->mpe_last && v->ch >= e->mpe_first && v->ch <= e->mpe_last && e->mpe_first > 0) {
            const aud_chan_t *mc = &e->chans[e->mpe_first - 1];         // the zone's master channel
            pitch += (int32_t)(((int64_t)mc->bend * mc->bend_range) / 8192);
        }
        pitch += (int32_t)(((int64_t)aud_eff(e, v, AUD_P_DETUNE) * 256 / 100) >> 16);
        if (tg & AUD_LFO_PITCH) {
            int32_t depth = aud_mul16(aud_eff(e, v, AUD_P_VIB_DEPTH), fade);   // semitones Q16
            pitch += (int32_t)(((int64_t)depth * lfo) >> 23);                  // * 256 / 32768 / 65536
        }
        if (v->glide) {
            int32_t d = (int32_t)(((int64_t)v->glide_step * n) >> 8);
            if (!d) {
                d = 1;
            }
            v->glide = v->glide > 0 ? (v->glide > d ? v->glide - d : 0) : (-v->glide > d ? v->glide + d : 0);
        }
        uint32_t step;
        bool loop = false;
        if (v->is_sample) {
            const aud_sample_t *s = v->smp;
            int32_t p = pitch - s->root * 256 + s->fine * 256 / 100 +
                        (int32_t)(((int64_t)c->sample_fine * 256 / 100) >> 16);
            uint64_t st = ((uint64_t)aud_pitch_ratio_q16(p) * s->rate) / e->rate;
            step = st > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)st;
            loop = s->loop_end > s->loop_start && c->loop_override != 0;
        } else {
            step = synth_step(e, pitch);
        }
        v->step = step;
        // Envelope
        env_advance(e, v, n);
        int32_t lv = v->level >> 8;                             // Q16
        // Gain: velocity, volume, CC 7, CC 11, tremolo
        int32_t g = (int32_t)((uint32_t)v->vel * v->vel * 65536u / 16129u);
        g = aud_mul16(g, aud_eff(e, v, AUD_P_VOLUME));
        g = aud_mul16(g, (int32_t)((uint32_t)c->vol * c->vol * 65536u / 16129u));
        g = aud_mul16(g, (int32_t)((uint32_t)c->expr * c->expr * 65536u / 16129u));
        if (tg & AUD_LFO_AMP) {
            int32_t trem = aud_mul16(aud_eff(e, v, AUD_P_TREM), fade);
            int32_t dip = aud_mul16(trem, (lfo + 32768) * 2) / 2;   // 0..trem
            g = aud_mul16(g, AUD_ONE - dip);
        }
        g = aud_mul16(g, AUD_VOICE_SCALE);
        if (e->nch == 2) {
            int32_t pan = aud_eff(e, v, AUD_P_PAN);
            int idx = 64 + (int)(((int64_t)pan * 64) >> 16);
            idx = idx < 0 ? 0 : idx > 128 ? 128 : idx;
            v->kgl = (g * aud_pan_l[idx]) >> 15;
            v->kgr = (g * aud_pan_r[idx]) >> 15;
        } else {
            v->kgl = v->kgr = (g * 23170) >> 15;
        }
        gl1 = aud_mul16(v->kgl, lv);
        gr1 = aud_mul16(v->kgr, lv);
        // Oscillator or sample
        if (v->is_sample) {
            alive = sampler(v, t, n, step, loop);
        } else {
            osc(v, t, n, step, (uint32_t)aud_eff(e, v, AUD_P_PW) << 16);
        }
        // Filter: Chamberlin state-variable low-pass, fixed point
        if (v->patch.filter) {
            int32_t cut = aud_eff(e, v, AUD_P_CUTOFF);
            cut += aud_mul16(aud_eff(e, v, AUD_P_FENV), lv);
            cut += (int32_t)(((int64_t)v->patch.key_track * ((int32_t)v->note - 60) * 65536 / 135) >> 16);
            if (tg & AUD_LFO_CUTOFF) {
                cut += (int32_t)(((int64_t)aud_mul16(v->patch.lfo_cutoff, fade) * lfo) >> 15);
            }
            cut = aud_clampi(cut, 0, AUD_ONE);
            uint32_t fst = synth_step(e, (int32_t)(((int64_t)cut * 135 * 256) >> 16));
            if (fst > 0xFFFFFFFFu / 6) {
                fst = 0xFFFFFFFFu / 6;                      // stable up to fs / 6
            }
            int32_t f = 2 * sine_at(fst >> 1);
            if (f > 32000) {
                f = 32000;
            }
            int32_t q = 23170 - aud_mul16(aud_eff(e, v, AUD_P_RES), 21532);  // Q14: 1.41 .. 0.1
            int32_t lp = v->lp, bp = v->bp;
            for (int i = 0; i < n; i++) {
                lp += (f * bp) >> 15;
                lp = aud_clampi(lp, -65535, 65535);
                int32_t hp = aud_clampi(t[i] - lp - ((q * bp) >> 14), -65535, 65535);
                bp = aud_clampi(bp + ((f * hp) >> 15), -65535, 65535);
                t[i] = (int16_t)aud_clampi(lp, -32767, 32767);
            }
            v->lp = lp;
            v->bp = bp;
        }
    }
    // Mix with gains ramped across the segment
    int32_t dl = (gl1 - gl0) / n, dr = (gr1 - gr0) / n, gl = gl0, gr = gr0;
    if (e->nch == 2) {
        for (int i = 0; i < n; i++) {
            gl += dl;
            gr += dr;
            L[i] += (t[i] * gl) >> 16;
            R[i] += (t[i] * gr) >> 16;
        }
    } else {
        for (int i = 0; i < n; i++) {
            gl += dl;
            L[i] += (t[i] * gl) >> 16;
        }
    }
    v->gl = gl1;
    v->gr = gr1;
    v->age += (uint32_t)n;
    if (!alive) {
        v->env = ENV_OFF;
    }
    if (v->env == ENV_OFF) {
        aud_voice_clear_ramps(e, v);
        v->gl = v->gr = 0;
        if (!v->pend) {
            AUD_TRACE(e, AUD_TRACE_END, v);
        }
        if (v->pend) {
            // A stolen voice's fade is over: start the note that took it.
            v->pend = 0;
            v->level = 0;
            if (!voice_begin(e, v)) {
                v->env = ENV_OFF;
                v->serial = 0;
            }
        } else {
            v->serial = 0;
        }
    }
}
