// Standard MIDI File loader (FR-20): format 0 or 1, metrical division, read
// into the pool as one list of events sorted by tick, with every note-on
// paired with its note-off so a song note becomes one timeline item.

#include <string.h>
#include "aud_int.h"

typedef struct {
    const uint8_t *p, *end;
} rd_t;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool vlq(rd_t *r, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (r->p >= r->end) {
            return false;
        }
        uint8_t b = *r->p++;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

#define NOTE_OFF_KIND   0xF0        // temporary: removed after pairing
#define DEAD_KIND       0xFF

// One pass over a track. ev NULL: only count. Returns events or <0.
static int parse_track(const uint8_t *p, size_t len, aud_song_ev_t *ev, uint32_t *last_tick, uint32_t *bar_num,
                       uint32_t *bar_den)
{
    rd_t r = { p, p + len };
    uint32_t tick = 0;
    uint8_t status = 0;
    int n = 0;
    while (r.p < r.end) {
        uint32_t d;
        if (!vlq(&r, &d)) {
            return AUD_ERR_FORMAT;
        }
        tick += d;
        if (r.p >= r.end) {
            return AUD_ERR_FORMAT;
        }
        uint8_t b = *r.p;
        if (b & 0x80) {
            status = b;
            r.p++;
        } else if (!status) {
            return AUD_ERR_FORMAT;          // running status with none set
        }
        if (status == 0xFF) {               // meta
            if (r.p >= r.end) {
                return AUD_ERR_FORMAT;
            }
            uint8_t type = *r.p++;
            uint32_t ml;
            if (!vlq(&r, &ml) || ml > (uint32_t)(r.end - r.p)) {
                return AUD_ERR_FORMAT;
            }
            if (type == 0x51 && ml == 3) {
                if (ev) {
                    ev[n] = (aud_song_ev_t){ .tick = tick, .kind = SE_TEMPO,
                                             .v = (uint32_t)r.p[0] << 16 | (uint32_t)r.p[1] << 8 | r.p[2] };
                }
                n++;
            } else if (type == 0x58 && ml >= 2 && bar_num && !*bar_num) {
                *bar_num = r.p[0];
                *bar_den = r.p[1] < 7 ? 1u << r.p[1] : 4;
            } else if (type == 0x2F) {
                r.p += ml;
                break;
            }
            r.p += ml;
            status = 0;                     // meta and sysex cancel running status
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            uint32_t sl;
            if (!vlq(&r, &sl) || sl > (uint32_t)(r.end - r.p)) {
                return AUD_ERR_FORMAT;
            }
            r.p += sl;
            status = 0;
            continue;
        }
        uint8_t hi = status & 0xF0, ch = status & 15;
        int need = hi == 0xC0 || hi == 0xD0 ? 1 : 2;
        if (r.end - r.p < need) {
            return AUD_ERR_FORMAT;
        }
        uint8_t d1 = r.p[0] & 127, d2 = need == 2 ? r.p[1] & 127 : 0;
        r.p += need;
        aud_song_ev_t e = { .tick = tick, .ch = ch, .a = d1, .b = d2 };
        switch (hi) {
        case 0x80: e.kind = NOTE_OFF_KIND; break;
        case 0x90: e.kind = d2 ? SE_NOTE : NOTE_OFF_KIND; break;
        case 0xA0: e.kind = SE_POLY; break;
        case 0xB0: e.kind = SE_CC; break;
        case 0xC0: e.kind = SE_PROGRAM; break;
        case 0xD0: e.kind = SE_PRESSURE; e.b = d1; break;
        case 0xE0: e.kind = SE_BEND; e.v = (uint32_t)((int32_t)(d2 << 7 | d1) - 8192); break;
        default: return AUD_ERR_FORMAT;
        }
        if (ev) {
            ev[n] = e;
        }
        n++;
    }
    if (last_tick && tick > *last_tick) {
        *last_tick = tick;
    }
    return n;
}

// Stable merge sort by tick (tracks were appended in order).
static void sort_ev(aud_song_ev_t *a, aud_song_ev_t *tmp, uint32_t n)
{
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = a[j].tick < a[i].tick ? a[j++] : a[i++];
            }
            while (i < mid) {
                tmp[k++] = a[i++];
            }
            while (j < hi) {
                tmp[k++] = a[j++];
            }
        }
        memcpy(a, tmp, n * sizeof(*a));
    }
}

int aud_song_load(aud_engine_t *e, const uint8_t *data, size_t len)
{
    if (e->nsongs >= AUD_SONGS) {
        return AUD_ERR_FULL;
    }
    if (len < 14 || memcmp(data, "MThd", 4) || be32(data + 4) < 6) {
        return AUD_ERR_FORMAT;
    }
    uint32_t hl = be32(data + 4);
    unsigned format = data[8] << 8 | data[9], ntrk = data[10] << 8 | data[11], div = data[12] << 8 | data[13];
    if (format > 1 || !ntrk || !div || (div & 0x8000) || hl > len - 8) {
        return AUD_ERR_FORMAT;          // SMPTE time and format 2 are not supported
    }
    // Pass 1: count.
    uint32_t total = 0, last = 0, bar_num = 0, bar_den = 4;
    const uint8_t *p = data + 8 + hl, *end = data + len;
    for (unsigned t = 0; t < ntrk; t++) {
        if (end - p < 8) {
            return AUD_ERR_FORMAT;
        }
        uint32_t tl = be32(p + 4);
        if (tl > (uint32_t)(end - p - 8)) {
            return AUD_ERR_FORMAT;
        }
        if (!memcmp(p, "MTrk", 4)) {
            int n = parse_track(p + 8, tl, NULL, &last, &bar_num, &bar_den);
            if (n < 0) {
                return n;
            }
            total += (uint32_t)n;
        }
        p += 8 + tl;
    }
    size_t mark = e->pool_used;
    aud_song_ev_t *ev = aud_pool_alloc(e, (total ? total : 1) * sizeof(aud_song_ev_t));
    aud_song_ev_t *tmp = aud_pool_alloc(e, (total ? total : 1) * sizeof(aud_song_ev_t));
    if (!ev || !tmp) {
        e->pool_used = mark;
        return AUD_ERR_NOMEM;
    }
    // Pass 2: fill, then merge the tracks by tick.
    uint32_t k = 0;
    p = data + 8 + hl;
    for (unsigned t = 0; t < ntrk; t++) {
        uint32_t tl = be32(p + 4);
        if (!memcmp(p, "MTrk", 4)) {
            k += (uint32_t)parse_track(p + 8, tl, ev + k, NULL, NULL, NULL);
        }
        p += 8 + tl;
    }
    sort_ev(ev, tmp, total);
    // Pair notes: the open note-on per channel and key, kept in tmp's space.
    uint32_t *open = (uint32_t *)tmp;
    if ((size_t)total * sizeof(aud_song_ev_t) < 16 * 128 * sizeof(uint32_t)) {
        e->pool_used = (size_t)((uint8_t *)tmp - e->pool);
        open = aud_pool_alloc(e, 16 * 128 * sizeof(uint32_t));
        if (!open) {
            e->pool_used = mark;
            return AUD_ERR_NOMEM;
        }
    }
    for (int i = 0; i < 16 * 128; i++) {
        open[i] = UINT32_MAX;
    }
    uint32_t end_tick = last;
    for (uint32_t i = 0; i < total; i++) {
        aud_song_ev_t *x = &ev[i];
        if (x->kind != SE_NOTE && x->kind != NOTE_OFF_KIND) {
            continue;
        }
        uint32_t *o = &open[x->ch * 128 + x->a];
        if (*o != UINT32_MAX) {
            ev[*o].v = x->tick - ev[*o].tick;   // a repeat note-on also ends the last one
            *o = UINT32_MAX;
        }
        if (x->kind == SE_NOTE) {
            *o = i;
        } else {
            x->kind = DEAD_KIND;
        }
    }
    for (int i = 0; i < 16 * 128; i++) {
        if (open[i] != UINT32_MAX) {
            ev[open[i]].v = end_tick > ev[open[i]].tick ? end_tick - ev[open[i]].tick : div;
        }
    }
    uint32_t m = 0, first_tempo = 500000;
    bool tempo_seen = false;
    for (uint32_t i = 0; i < total; i++) {
        if (ev[i].kind == DEAD_KIND) {
            continue;
        }
        if (ev[i].kind == SE_TEMPO && !tempo_seen && ev[i].tick == 0) {
            first_tempo = ev[i].v;
            tempo_seen = true;
        }
        ev[m++] = ev[i];
    }
    // Free the scratch; keep the events.
    e->pool_used = (size_t)((uint8_t *)ev - e->pool) + (m ? m : 1) * sizeof(aud_song_ev_t);
    uint32_t bar = div * 4 * (bar_num ? bar_num : 4) / (bar_den ? bar_den : 4);
    if (!bar) {
        bar = div * 4;
    }
    int id = e->nsongs;
    aud_song_t *s = &e->cold->songs[id];
    s->ev = ev;
    s->n = m;
    s->division = div;
    s->length = (end_tick + bar - 1) / bar * bar;
    s->first_tempo = first_tempo;
    e->nsongs++;
    return id;
}

uint32_t aud_song_ticks(const aud_engine_t *e, int song)
{
    return song >= 0 && song < e->nsongs ? e->cold->songs[song].length : 0;
}
