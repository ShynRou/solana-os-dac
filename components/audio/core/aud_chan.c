// Channels: controllers (task spec "Channel controllers"), RPN and NRPN
// (FR-27), program changes, and applying immediate events.

#include <string.h>
#include "aud_int.h"
#include "aud_tables.h"

#define REL(v)      (((int32_t)(v) - 64) * AUD_ONE / 128)   // CC 64 neutral: -0.5 .. +0.5

void aud_chan_reset(aud_engine_t *e, int ch, bool all)
{
    aud_chan_t *c = &e->chans[ch];
    if (all) {
        memset(c, 0, sizeof(*c));
        c->head = AUD_NONE16;
        for (int i = 0; i < AUD_MAPS; i++) {
            c->maps[i].def = 0xFF;
        }
    }
    c->vol = 100;
    c->expr = 127;
    c->pan_cc = 64;
    c->mod = 0;
    c->sustain = c->sostenuto = c->soft = c->legato = c->porta_on = c->mono = 0;
    c->porta_src = 0xFF;
    c->last_note = 0xFF;
    c->rpn_msb = c->rpn_lsb = c->nrpn_msb = c->nrpn_lsb = 127;
    c->data_msb = c->data_lsb = 0;
    c->nrpn_active = c->lsb_seen = 0;
    c->variation = 0xFF;
    c->chan_pressure = 0;
    c->bend = 0;
    bool mpe = e->mpe_first <= e->mpe_last && ch >= e->mpe_first && ch <= e->mpe_last;
    c->bend_range = (uint16_t)((mpe ? 48 : 2) * 256);
    c->tune_coarse = c->tune_fine = 0;
    c->chan_set = 0;
    for (int p = 0; p < AUD_P_COUNT; p++) {
        c->chan_abs[p] = 0;
        c->chan_rel[p] = aud_param_mult_kind(p) ? AUD_ONE : 0;
    }
    c->wave_override = c->filter_override = c->loop_override = c->lfo_targets_override = 0xFF;
    c->key_track_override = -1;
    c->sample_fine = 0;
    if (all) {
        c->ptarget = AUD_P_VIB_DEPTH;       // FR-32 default target
        c->pamount = AUD_ONE / 2;
    }
}

void aud_chan_pedal_release(aud_engine_t *e, int ch)
{
    aud_chan_t *c = &e->chans[ch];
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->ch == ch && v->sustained && !c->sustain && !v->sostenuto) {
            aud_voice_release(e, v);
        }
    }
}

static void set_abs(aud_chan_t *c, int p, int32_t v)
{
    c->chan_abs[p] = aud_param_clamp(p, v);
    c->chan_set |= 1u << p;
}

static void data_entry(aud_engine_t *e, int ch)
{
    aud_chan_t *c = &e->chans[ch];
    int v14 = c->lsb_seen ? (c->data_msb << 7 | c->data_lsb) : c->data_msb;
    if (!c->nrpn_active) {
        if (c->rpn_msb != 0) {
            return;
        }
        switch (c->rpn_lsb) {
        case 0:     // pitch bend range: MSB semitones, LSB cents
            c->bend_range = (uint16_t)aud_clampi(c->data_msb * 256 + c->data_lsb * 256 / 100, 0, 96 * 256);
            break;
        case 1:     // fine tuning: 14 bits, centre 8192, +-100 cents
            c->tune_fine = ((c->data_msb << 7 | c->data_lsb) - 8192) * 256 / 8192;
            break;
        case 2:     // coarse tuning: MSB semitones, centre 64
            c->tune_coarse = (c->data_msb - 64) * 256;
            break;
        }
        return;
    }
    if (c->nrpn_msb != 1) {
        return;
    }
    // NRPN MSB 1 (task spec table); the value is the 14-bit data entry.
    switch (c->nrpn_lsb) {
    case 0: c->wave_override = (uint8_t)aud_clampi(v14, 0, AUD_WAVE_COUNT - 1); break;
    case 1: set_abs(c, AUD_P_PW, aud_clampi(v14, 5, 95) * AUD_ONE / 100); break;
    case 2: set_abs(c, AUD_P_FENV, aud_clampi(v14, 0, 100) * AUD_ONE / 100); break;
    case 3: c->key_track_override = aud_clampi(v14, 0, 100) * AUD_ONE / 100; break;
    case 4: set_abs(c, AUD_P_VIB_RATE, aud_clampi(v14, 10, 2000) * AUD_ONE / 100); break;   // 1/100 Hz
    case 5: c->lfo_targets_override = (uint8_t)(v14 & 7); break;
    case 6: set_abs(c, AUD_P_SUSTAIN, aud_clampi(v14, 0, 100) * AUD_ONE / 100); break;
    case 7: c->filter_override = v14 ? 1 : 0; break;
    case 8: c->loop_override = v14 ? 1 : 0; break;
    case 9: c->sample_fine = aud_clampi(v14 - 8192, -100, 100) * AUD_ONE; break;               // 8192 + cents
    }
}

static void voices_on_channel(aud_engine_t *e, int ch, bool kill, bool ignore_pedals)
{
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->ch != ch || v->env == ENV_OFF) {
            continue;
        }
        if (v->pend) {
            v->pend = 0;
            v->serial = 0;
        }
        if (kill) {
            aud_voice_kill(e, v);
        } else if (ignore_pedals) {
            aud_voice_release(e, v);
        } else {
            aud_voice_off(e, v);
        }
    }
}

void aud_chan_cc(aud_engine_t *e, int ch, int cc, int v)
{
    aud_chan_t *c = &e->chans[ch];
    switch (cc) {
    case 0: c->bank_msb = (uint8_t)v; break;
    case 32: c->bank_lsb = (uint8_t)v; break;
    case 1: c->mod = (uint8_t)v; break;
    case 5: set_abs(c, AUD_P_PORTA, v * v * 2000 / 16129); break;
    case 6: c->data_msb = (uint8_t)v; data_entry(e, ch); break;
    case 38: c->data_lsb = (uint8_t)v; c->lsb_seen = 1; data_entry(e, ch); break;
    case 96: case 97: {                     // data increment / decrement
        int d = c->lsb_seen ? (c->data_msb << 7 | c->data_lsb) : c->data_msb;
        d = aud_clampi(d + (cc == 96 ? 1 : -1), 0, 16383);
        if (c->lsb_seen) {
            c->data_msb = (uint8_t)(d >> 7), c->data_lsb = (uint8_t)(d & 127);
        } else {
            c->data_msb = (uint8_t)aud_clampi(d, 0, 127);
        }
        data_entry(e, ch);
        break;
    }
    case 98: c->nrpn_lsb = (uint8_t)v; c->nrpn_active = 1; c->lsb_seen = 0; break;
    case 99: c->nrpn_msb = (uint8_t)v; c->nrpn_active = 1; c->lsb_seen = 0; break;
    case 100: c->rpn_lsb = (uint8_t)v; c->nrpn_active = 0; c->lsb_seen = 0; break;
    case 101: c->rpn_msb = (uint8_t)v; c->nrpn_active = 0; c->lsb_seen = 0; break;
    case 7: c->vol = (uint8_t)v; break;
    case 10: c->pan_cc = (uint8_t)v; set_abs(c, AUD_P_PAN, (v - 64) * AUD_ONE / 63); break;
    case 11: c->expr = (uint8_t)v; break;
    case 64:
        c->sustain = v >= 64;
        if (!c->sustain) {
            aud_chan_pedal_release(e, ch);
        }
        break;
    case 65: c->porta_on = v >= 64; break;
    case 66:
        if (v >= 64 && !c->sostenuto) {
            for (int i = 0; i < AUD_VOICES; i++) {
                aud_voice_t *w = &e->voices[i];
                if (w->ch == ch && w->env >= ENV_ATTACK && w->env <= ENV_SUSTAIN && !w->sustained) {
                    w->sostenuto = 1;
                }
            }
        } else if (v < 64 && c->sostenuto) {
            for (int i = 0; i < AUD_VOICES; i++) {
                e->voices[i].sostenuto = e->voices[i].ch == ch ? 0 : e->voices[i].sostenuto;
            }
            aud_chan_pedal_release(e, ch);
        }
        c->sostenuto = v >= 64;
        break;
    case 67: c->soft = v >= 64; break;
    case 68: c->legato = v >= 64; break;
    case 70: c->variation = (uint8_t)v; break;
    case 71: c->chan_rel[AUD_P_RES] = REL(v); break;
    case 72: c->chan_rel[AUD_P_RELEASE] = (int32_t)aud_ccmul[v]; break;
    case 73: c->chan_rel[AUD_P_ATTACK] = (int32_t)aud_ccmul[v]; break;
    case 74: c->chan_rel[AUD_P_CUTOFF] = REL(v); break;
    case 75: c->chan_rel[AUD_P_DECAY] = (int32_t)aud_ccmul[v]; break;
    case 76: c->chan_rel[AUD_P_VIB_RATE] = (int32_t)aud_ccmul[v]; break;
    case 77: c->chan_rel[AUD_P_VIB_DEPTH] = (v - 64) * AUD_ONE / 64; break;    // +-1 semitone
    case 78: c->chan_rel[AUD_P_VIB_DELAY] = (int32_t)aud_ccmul[v]; break;
    case 84: c->porta_src = (uint8_t)v; break;
    case 92: c->chan_rel[AUD_P_TREM] = v * AUD_ONE / 127; break;
    case 94: c->chan_rel[AUD_P_DETUNE] = (v - 64) * 100 * AUD_ONE / 64; break;
    case 91: case 93: case 95:          // reverb, chorus, phaser: no effects bus
    case 124: case 125:                 // omni
        atomic_fetch_add(&e->c_ignored, 1);
        break;
    case 120: voices_on_channel(e, ch, true, true); break;
    case 121:
        aud_chan_reset(e, ch, false);
        aud_chan_pedal_release(e, ch);
        break;
    case 123:
        c->sustain = 0;
        voices_on_channel(e, ch, false, true);
        break;
    case 126: c->mono = 1; break;
    case 127: c->mono = 0; break;
    default: {
        for (int i = 0; i < AUD_MAPS; i++) {
            if (c->maps[i].def != 0xFF && c->maps[i].cc == cc) {
                const aud_map_def_t *d = &e->cold->map_defs[c->maps[i].def];
                set_abs(c, d->param, d->min + (int32_t)(((int64_t)(d->max - d->min) * v) / 127));
                return;
            }
        }
        atomic_fetch_add(&e->c_ignored, 1);
    }
    }
}

void aud_chan_program(aud_engine_t *e, int ch, int program)
{
    aud_chan_t *c = &e->chans[ch];
    aud_cold_t *cold = e->cold;
    int n = atomic_load_explicit(&cold->nprograms, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        if (cold->programs[i].bank == c->bank_msb && cold->programs[i].program == program) {
            c->handle = cold->programs[i].handle;
            return;
        }
    }
    atomic_fetch_add(&e->c_ignored, 1);     // not registered with audio.program: ignored
}

static aud_voice_t *find_or_stale(aud_engine_t *e, uint32_t serial)
{
    aud_voice_t *v = aud_voice_find(e, serial);
    if (!v) {
        atomic_fetch_add(&e->c_stale, 1);   // FR-28: a late call can't touch another note
    }
    return v;
}

void aud_apply_event(aud_engine_t *e, const aud_ev_t *ev)
{
    aud_chan_t *c = &e->chans[ev->ch & 15];
    aud_voice_t *v;
    switch (ev->type) {
    case EV_NOTE_ON:
        aud_note_start(e, ev->ch, ev->a, ev->b, ev->serial, NULL);
        break;
    case EV_NOTE_PARAM_B:
        if ((v = aud_voice_find(e, ev->serial))) {
            aud_voice_param(e, v, ev->a, ev->value);
        }
        break;
    case EV_NOTE_PARAM:
        if ((v = find_or_stale(e, ev->serial))) {
            aud_voice_param(e, v, ev->a, ev->value);
        }
        break;
    case EV_NOTE_OFF:
        aud_note_stop(e, ev->ch, ev->a);
        break;
    case EV_NOTE_OFF_H:
        if ((v = find_or_stale(e, ev->serial))) {
            if (v->pend) {
                v->pend = 0;
                v->serial = 0;
            } else {
                aud_voice_off(e, v);
            }
        }
        break;
    case EV_CC:
        aud_chan_cc(e, ev->ch, ev->a, ev->b);
        break;
    case EV_BEND:
        c->bend = (int16_t)ev->value;
        break;
    case EV_PRESSURE:
        c->chan_pressure = ev->b;
        break;
    case EV_POLY:
        for (int i = 0; i < AUD_VOICES; i++) {
            aud_voice_t *w = &e->voices[i];
            if (w->ch == ev->ch && w->note == ev->a && w->env != ENV_OFF) {
                w->pressure = ev->b * AUD_ONE / 127;
            }
        }
        break;
    case EV_PROGRAM:
        aud_chan_program(e, ev->ch, ev->a);
        break;
    case EV_PARAM:
        set_abs(c, ev->a, ev->value);
        break;
    case EV_NOTE_RAMP:
        if ((v = find_or_stale(e, ev->serial))) {
            aud_voice_ramp(e, v, ev->a, ev->value, (uint32_t)ev->ch | (uint32_t)ev->b << 8);
        }
        break;
    case EV_NOTE_BEND:
        if ((v = find_or_stale(e, ev->serial))) {
            v->bend_note = ev->value;
        }
        break;
    case EV_NOTE_PRESSURE:
        if ((v = find_or_stale(e, ev->serial))) {
            v->pressure = ev->value;
        }
        break;
    case EV_ASSIGN:
        c->handle = (uint16_t)ev->value;
        break;
    case EV_PTARGET:
        c->ptarget = ev->a;
        c->pamount = ev->value;
        break;
    case EV_MAP_CC: {
        int slot = -1;
        for (int i = 0; i < AUD_MAPS; i++) {
            if (c->maps[i].def != 0xFF && c->maps[i].cc == ev->a) {
                slot = i;
                break;
            }
            if (slot < 0 && c->maps[i].def == 0xFF) {
                slot = i;
            }
        }
        if (slot >= 0) {
            c->maps[slot].cc = ev->a;
            c->maps[slot].def = (uint8_t)ev->value;
        }
        break;
    }
    case EV_MPE:
        e->mpe_first = ev->a;
        e->mpe_last = ev->b;
        for (int ch = 0; ch < AUD_CHANNELS; ch++) {
            bool in = ev->a <= ev->b && ch >= ev->a && ch <= ev->b;
            e->chans[ch].bend_range = (uint16_t)((in ? 48 : 2) * 256);
        }
        break;
    case EV_ALL_OFF:
        for (int ch = 0; ch < AUD_CHANNELS; ch++) {
            e->chans[ch].sustain = 0;
            e->chans[ch].sostenuto = 0;
            voices_on_channel(e, ch, ev->a != 0, true);
        }
        break;
    }
}
