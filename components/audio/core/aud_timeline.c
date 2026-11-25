// The tick clock, per-channel timelines, quantised edits and the song feed
// (task spec FR-38 to FR-54; build documentation "Clock, timelines and
// quantised editing").
//
// Each channel keeps a singly linked list of future items, sorted by tick,
// in one pool of AUD_TL_NODES nodes. Only the mixer touches the lists. The
// producer stages an edit's items in a staging block and posts a command
// that the mixer applies, whole or not at all, when the clock reaches the
// command's tick.

#include <string.h>
#include "aud_int.h"

#define NODES   (e->cold->nodes)

// ---------------------------------------------------------------------------
// Node pool (mixer)

void aud_tl_init(aud_engine_t *e)
{
    for (int i = 0; i < AUD_TL_NODES; i++) {
        NODES[i].next = (uint16_t)(i + 1 < AUD_TL_NODES ? i + 1 : AUD_NONE16);
    }
    e->tl_free_head = 0;
    e->tl_free = AUD_TL_NODES;
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        e->chans[ch].head = AUD_NONE16;
    }
}

static uint16_t node_alloc(aud_engine_t *e)
{
    uint16_t n = e->tl_free_head;
    if (n != AUD_NONE16) {
        e->tl_free_head = NODES[n].next;
        e->tl_free--;
    }
    return n;
}

static void node_free(aud_engine_t *e, uint16_t n)
{
    NODES[n].next = e->tl_free_head;
    e->tl_free_head = n;
    e->tl_free++;
}

// Insert sorted by tick, after items with the same tick (edits keep order).
int aud_tl_insert(aud_engine_t *e, int ch, const aud_item_t *it)
{
    uint16_t n = node_alloc(e);
    if (n == AUD_NONE16) {
        return AUD_ERR_FULL;
    }
    NODES[n] = *it;
    uint16_t *link = &e->chans[ch].head;
    while (*link != AUD_NONE16 && (int32_t)(NODES[*link].tick - it->tick) <= 0) {
        link = &NODES[*link].next;
    }
    NODES[n].next = *link;
    *link = n;
    return AUD_OK;
}

// Remove a channel's future items starting in [from, to); song_only: only
// the sequencer's. Returns how many.
static int remove_range(aud_engine_t *e, int ch, uint32_t from, uint32_t to, bool song_only)
{
    int k = 0;
    uint16_t *link = &e->chans[ch].head;
    while (*link != AUD_NONE16) {
        aud_item_t *it = &NODES[*link];
        bool in = (int32_t)(it->tick - from) >= 0 && (int32_t)(it->tick - to) < 0;
        if (in && (!song_only || (it->flags & AUD_ITEM_SONG))) {
            uint16_t dead = *link;
            *link = it->next;
            node_free(e, dead);
            k++;
        } else {
            link = &it->next;
        }
    }
    return k;
}

static int count_range(aud_engine_t *e, int ch, uint32_t from, uint32_t to)
{
    int k = 0;
    for (uint16_t n = e->chans[ch].head; n != AUD_NONE16; n = NODES[n].next) {
        k += (int32_t)(NODES[n].tick - from) >= 0 && (int32_t)(NODES[n].tick - to) < 0;
    }
    return k;
}

void aud_tl_clear_channel(aud_engine_t *e, int ch, bool song_only)
{
    remove_range(e, ch, e->clock - 0x40000000u, e->clock + 0x3FFFFFFFu, song_only);
}

// ---------------------------------------------------------------------------
// Tempo

void aud_tl_set_tempo(aud_engine_t *e, uint32_t bpm_q16)
{
    if (bpm_q16 < 20 * AUD_ONE) {
        bpm_q16 = 20 * AUD_ONE;
    } else if (bpm_q16 > 300 * AUD_ONE) {
        bpm_q16 = 300 * AUD_ONE;
    }
    e->bpm_q16 = bpm_q16;
    // samples per tick = rate * 60 / (bpm * ppqn), Q16
    uint64_t num = (uint64_t)e->rate * 60u << 32;
    uint64_t den = (uint64_t)bpm_q16 * e->ppqn;
    e->spt = (uint32_t)(num / den);
    if (e->spt < AUD_ONE) {
        e->spt = AUD_ONE;                   // at least a sample per tick
    }
    if (e->tick_pos >= e->spt) {
        e->tick_pos = e->spt - 1;
    }
}

// ---------------------------------------------------------------------------
// Note ids inside an edit (FR-44)

static void id_remember(aud_chan_t *c, uint8_t id, uint32_t serial)
{
    for (int i = 0; i < AUD_IDS; i++) {
        if (c->ids[i] == id) {
            c->id_serial[i] = serial;
            return;
        }
    }
    int i = c->id_next++ % AUD_IDS;
    c->ids[i] = id;
    c->id_serial[i] = serial;
}

static aud_voice_t *id_voice(aud_engine_t *e, aud_chan_t *c, uint8_t id)
{
    for (int i = 0; i < AUD_IDS; i++) {
        if (c->ids[i] == id) {
            aud_voice_t *v = aud_voice_find(e, c->id_serial[i]);
            if (!v) {
                atomic_fetch_add(&e->c_stale, 1);
            }
            return v;
        }
    }
    atomic_fetch_add(&e->c_stale, 1);
    return NULL;
}

// ---------------------------------------------------------------------------
// Playing an item

static void play(aud_engine_t *e, int ch, const aud_item_t *it)
{
    aud_chan_t *c = &e->chans[ch];
    aud_voice_t *v;
    switch (it->kind) {
    case AUD_IT_NOTE: {
        uint32_t s = AUD_SERIAL_MIXER | (++e->mixer_serial & ~AUD_SERIAL_MIXER);
        if (!aud_note_start(e, ch, it->a, it->b ? it->b : 1, s, &v) || !v) {
            break;
        }
        v->from_tl = 1;
        v->song = (it->flags & AUD_ITEM_SONG) != 0;
        v->src_tick = it->tick;
        v->end_tick = it->tick + (uint32_t)(it->v > 0 ? it->v : 1);
        v->end_armed = 1;
        if (it->id) {
            id_remember(c, it->id, s);
        }
        break;
    }
    case AUD_IT_CC:
        aud_chan_cc(e, ch, it->a & 127, it->b & 127);
        break;
    case AUD_IT_BEND:
        if (it->ref) {
            if ((v = id_voice(e, c, it->ref))) {
                v->bend_note = aud_clampi(it->v, -24 * 256, 24 * 256);
            }
        } else {
            c->bend = (int16_t)aud_clampi(it->v, -8192, 8191);
        }
        break;
    case AUD_IT_PRESSURE:
        if (it->ref) {
            if ((v = id_voice(e, c, it->ref))) {
                v->pressure = (it->b & 127) * AUD_ONE / 127;
            }
        } else if (it->a < 128) {
            for (int i = 0; i < AUD_VOICES; i++) {
                aud_voice_t *w = &e->voices[i];
                if (w->ch == ch && w->note == it->a && w->env != ENV_OFF) {
                    w->pressure = (it->b & 127) * AUD_ONE / 127;
                }
            }
        } else {
            c->chan_pressure = it->b & 127;
        }
        break;
    case AUD_IT_PROGRAM:
        aud_chan_program(e, ch, it->a & 127);
        break;
    case AUD_IT_PARAM:
        if (it->a >= AUD_P_COUNT) {
            break;
        }
        if (it->ref) {
            if ((v = id_voice(e, c, it->ref))) {
                aud_voice_param(e, v, it->a, it->v);
            }
        } else {
            c->chan_abs[it->a] = aud_param_clamp(it->a, it->v);
            c->chan_set |= 1u << it->a;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Edits (mixer side)

static void release_owned(aud_engine_t *e, int ch, uint32_t from, uint32_t to, bool trim)
{
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->ch != ch || !v->from_tl || v->env == ENV_OFF || v->pend) {
            continue;
        }
        bool inside = (int32_t)(v->src_tick - from) >= 0 && (int32_t)(v->src_tick - to) < 0;
        bool before = (int32_t)(v->src_tick - from) < 0;
        if (inside || (trim && before)) {
            aud_voice_release(e, v);        // FR-50: through its release envelope
        }
    }
}

static void free_block(aud_engine_t *e, const aud_cmd_t *c)
{
    if (c->block < AUD_STAGE_BLOCKS) {
        atomic_store_explicit(&e->cold->stage_busy[c->block], 0, memory_order_release);
    }
}

static void stop_channel(aud_engine_t *e, int ch, bool kill, bool reset)
{
    aud_tl_clear_channel(e, ch, false);
    if (e->seq.playing) {
        e->chans[ch].song_muted = 1;
    }
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
        } else {
            aud_voice_release(e, v);
        }
    }
    e->chans[ch].sustain = 0;
    if (reset) {
        aud_chan_reset(e, ch, false);
    }
}

static void run_edit(aud_engine_t *e, const aud_cmd_t *c)
{
    const aud_item_t *items = c->block < AUD_STAGE_BLOCKS ? e->cold->stage[c->block] : NULL;
    int ch = c->ch;
    uint32_t now = e->clock;
    switch (c->op) {
    case AUD_ED_INSERT:
    case AUD_ED_UPDATE: {
        int room = (int)e->tl_free;
        if (c->op == AUD_ED_UPDATE) {
            room += count_range(e, ch, c->from, c->to);
        }
        if (room < c->n) {
            atomic_fetch_add(&e->c_rejected, 1);    // nothing applies (FR-51)
            break;
        }
        uint32_t base = c->from;
        if (c->op == AUD_ED_UPDATE) {
            remove_range(e, ch, c->from, c->to, false);
            release_owned(e, ch, c->from, c->to, (c->flags & CMDF_TRIM) != 0);
        }
        for (int i = 0; i < c->n; i++) {
            aud_item_t it = items[i];
            it.tick = base + it.tick;
            it.flags &= (uint8_t)~AUD_ITEM_SONG;
            if ((int32_t)(it.tick - now) < 0) {
                atomic_fetch_add(&e->c_late, 1);    // already past: skipped
                continue;
            }
            aud_tl_insert(e, ch, &it);
        }
        break;
    }
    case AUD_ED_DELETE:
        remove_range(e, ch, c->from, c->to, false);
        release_owned(e, ch, c->from, c->to, (c->flags & CMDF_TRIM) != 0);
        break;
    case AUD_ED_STOP:
        if (ch == AUD_ALL) {
            for (int k = 0; k < AUD_CHANNELS; k++) {
                stop_channel(e, k, (c->flags & CMDF_KILL) != 0, (c->flags & CMDF_RESET) != 0);
            }
            aud_seq_halt(e, true);
            e->npending = 0;                // millisecond-delay events go too (FR-52)
        } else {
            stop_channel(e, ch, (c->flags & CMDF_KILL) != 0, (c->flags & CMDF_RESET) != 0);
        }
        break;
    }
    free_block(e, c);
}

static void transport_stop(aud_engine_t *e, const aud_cmd_t *c)
{
    for (int i = 0; i < e->ncmd; i++) {
        free_block(e, &e->cmdq[i]);
    }
    e->ncmd = 0;
    e->ntempos = 0;
    aud_seq_halt(e, true);
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        stop_channel(e, ch, false, false);
        e->chans[ch].song_muted = 0;
    }
    if (c->value & 0xFFFF) {
        e->ppqn = c->value & 0xFFFF;
    }
    if (c->value >> 16) {
        e->beats = c->value >> 16;
    }
    e->clock = 0;
    e->tick_pos = 0;
    aud_tl_set_tempo(e, c->from ? c->from : e->bpm_q16);
}

static void run_cmd(aud_engine_t *e, const aud_cmd_t *c)
{
    switch (c->type) {
    case CMD_EDIT: run_edit(e, c); break;
    case CMD_TEMPO: aud_tl_set_tempo(e, c->value); break;
    case CMD_SONG_PLAY: aud_seq_start(e, c); break;
    case CMD_SONG_PAUSE: aud_seq_halt(e, false); break;
    case CMD_SONG_STOP: aud_seq_halt(e, true); break;
    }
}

void aud_tl_take_cmds(aud_engine_t *e)
{
    uint32_t head = atomic_load_explicit(&e->cmds.head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&e->cmds.tail, memory_order_acquire);
    while (head != tail) {
        aud_cmd_t c = e->cmds.buf[head % AUD_CMDS];
        head++;
        if (c.type == CMD_TRANSPORT_STOP) {
            transport_stop(e, &c);
            continue;
        }
        if ((int32_t)(c.apply - e->clock) < 0) {
            atomic_fetch_add(&e->c_late, 1);
            c.apply = e->clock;             // late: at once
        }
        if (e->ncmd >= AUD_CMDS) {
            atomic_fetch_add(&e->c_rejected, 1);
            free_block(e, &c);
            continue;
        }
        int i = e->ncmd;                    // sorted by tick, stable (FR-50: in order)
        while (i > 0 && (int32_t)(c.apply - e->cmdq[i - 1].apply) < 0) {
            e->cmdq[i] = e->cmdq[i - 1];
            i--;
        }
        e->cmdq[i] = c;
        e->ncmd++;
    }
    atomic_store_explicit(&e->cmds.head, head, memory_order_release);
}

// ---------------------------------------------------------------------------
// Everything due by `tick`, in order: edits, tempo, note ends, items.

void aud_tl_process(aud_engine_t *e, uint32_t tick)
{
    int k = 0;
    while (k < e->ncmd && (int32_t)(e->cmdq[k].apply - tick) <= 0) {
        run_cmd(e, &e->cmdq[k]);
        k++;
    }
    if (k) {
        memmove(e->cmdq, e->cmdq + k, (size_t)(e->ncmd - k) * sizeof(aud_cmd_t));
        e->ncmd -= k;
    }
    k = 0;
    while (k < e->ntempos && (int32_t)(e->tempos[k].tick - tick) <= 0) {
        aud_tl_set_tempo(e, e->tempos[k].bpm_q16);
        k++;
    }
    if (k) {
        memmove(e->tempos, e->tempos + k, (size_t)(e->ntempos - k) * sizeof(e->tempos[0]));
        e->ntempos -= k;
    }
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->end_armed && v->env != ENV_OFF && (int32_t)(v->end_tick - tick) <= 0) {
            v->end_armed = 0;
            aud_voice_off(e, v);
        }
    }
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        aud_chan_t *c = &e->chans[ch];
        while (c->head != AUD_NONE16 && (int32_t)(NODES[c->head].tick - tick) <= 0) {
            uint16_t n = c->head;
            aud_item_t it = NODES[n];
            c->head = it.next;
            node_free(e, n);
            play(e, ch, &it);
        }
    }
}

uint32_t aud_tl_next(aud_engine_t *e)
{
    uint32_t best = UINT32_MAX;
    int32_t bd = INT32_MAX;
#define CONSIDER(t) do { int32_t d_ = (int32_t)((t) - e->clock); if (d_ > 0 && d_ < bd) { bd = d_; best = (t); } } while (0)
    if (e->ncmd) {
        CONSIDER(e->cmdq[0].apply);
    }
    if (e->ntempos) {
        CONSIDER(e->tempos[0].tick);
    }
    for (int i = 0; i < AUD_VOICES; i++) {
        if (e->voices[i].end_armed && e->voices[i].env != ENV_OFF) {
            CONSIDER(e->voices[i].end_tick);
        }
    }
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        if (e->chans[ch].head != AUD_NONE16) {
            CONSIDER(NODES[e->chans[ch].head].tick);
        }
    }
#undef CONSIDER
    return best;
}

// ---------------------------------------------------------------------------
// Sequencer (FR-20, FR-21, FR-53): writes song events into the channel
// timelines one bar ahead of the clock.

static int64_t song_tick(const aud_engine_t *e, const aud_song_t *s, uint32_t ft)
{
    return e->seq.origin + (int64_t)((uint64_t)ft * e->ppqn / s->division);
}

void aud_seq_feed(aud_engine_t *e)
{
    aud_seq_t *q = &e->seq;
    if (!q->playing || q->song < 0) {
        return;
    }
    const aud_song_t *s = &e->cold->songs[q->song];
    int64_t horizon = (int64_t)e->clock + e->ppqn * e->beats;
    for (int guard = 0; guard < 4096; guard++) {
        if (q->pos >= s->n) {
            if (!q->loop || !s->length) {
                q->playing = 0;             // fed to the end; the last notes play out
                return;
            }
            q->origin += (int64_t)((uint64_t)s->length * e->ppqn / s->division);
            q->pos = 0;
        }
        const aud_song_ev_t *ev = &s->ev[q->pos];
        int64_t t = song_tick(e, s, ev->tick);
        if (t > horizon) {
            return;
        }
        if (t < (int64_t)e->clock) {
            t = e->clock;                   // fed late (pool was full): play now
        }
        if (ev->kind == SE_TEMPO) {
            if (q->follow && e->ntempos < AUD_TEMPOS && ev->v) {
                uint64_t bpm = (60000000ull << 16) / ev->v;
                bpm = (bpm * q->scale_q16) >> 16;
                int i = e->ntempos++;
                e->tempos[i].tick = (uint32_t)t;
                e->tempos[i].bpm_q16 = (uint32_t)bpm;
            }
            q->pos++;
            continue;
        }
        if (e->chans[ev->ch].song_muted) {
            q->pos++;
            continue;
        }
        aud_item_t it = { .tick = (uint32_t)t, .flags = AUD_ITEM_SONG, .a = ev->a, .b = ev->b };
        switch (ev->kind) {
        case SE_NOTE:
            it.kind = AUD_IT_NOTE;
            it.v = (int32_t)((uint64_t)ev->v * e->ppqn / s->division);
            if (it.v < 1) {
                it.v = 1;
            }
            break;
        case SE_CC: it.kind = AUD_IT_CC; break;
        case SE_BEND: it.kind = AUD_IT_BEND; it.v = (int32_t)ev->v; break;
        case SE_PRESSURE: it.kind = AUD_IT_PRESSURE; it.a = 0xFF; break;
        case SE_POLY: it.kind = AUD_IT_PRESSURE; break;
        case SE_PROGRAM: it.kind = AUD_IT_PROGRAM; break;
        default: q->pos++; continue;
        }
        if (aud_tl_insert(e, ev->ch, &it) != AUD_OK) {
            atomic_fetch_add(&e->c_late, 1);
            return;                         // the pool is full: try again next block
        }
        q->pos++;
    }
}

static uint32_t first_at_or_after(const aud_song_t *s, uint32_t ft)
{
    uint32_t i = 0;
    while (i < s->n && s->ev[i].tick < ft) {
        i++;
    }
    return i;
}

void aud_seq_start(aud_engine_t *e, const aud_cmd_t *c)
{
    aud_seq_t *q = &e->seq;
    if (c->song >= AUD_SONGS || !e->cold->songs[c->song].ev) {
        return;
    }
    const aud_song_t *s = &e->cold->songs[c->song];
    if (q->playing) {
        aud_seq_halt(e, true);
    }
    bool resume = q->paused && q->song == c->song;
    q->song = (int8_t)c->song;
    q->loop = (c->flags & CMDF_LOOP) != 0;
    q->follow = (c->flags & CMDF_FOLLOW) != 0;
    q->scale_q16 = c->value ? c->value : AUD_ONE;
    uint32_t ft = resume ? q->pause_ft : 0;
    q->origin = (int64_t)c->apply - (int64_t)((uint64_t)ft * e->ppqn / s->division);
    q->pos = first_at_or_after(s, ft);
    q->playing = 1;
    q->paused = 0;
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        e->chans[ch].song_muted = 0;
    }
    if (q->follow && s->first_tempo) {
        aud_tl_set_tempo(e, (uint32_t)((((60000000ull << 16) / s->first_tempo) * q->scale_q16) >> 16));
    }
    aud_seq_feed(e);                        // the first items may be due on this very tick
}

void aud_seq_halt(aud_engine_t *e, bool rewind)
{
    aud_seq_t *q = &e->seq;
    if (q->song < 0) {
        return;
    }
    const aud_song_t *s = &e->cold->songs[q->song];
    if (q->playing && !rewind && s->division) {
        int64_t ft = ((int64_t)e->clock - q->origin) * s->division / e->ppqn;
        if (ft < 0) {
            ft = 0;
        }
        if (s->length) {
            ft %= s->length;
        }
        q->pause_ft = (uint32_t)ft;
        q->paused = 1;
    } else if (rewind) {
        q->paused = 0;
        q->pause_ft = 0;
    }
    q->playing = 0;
    q->pos = 0;
    for (int ch = 0; ch < AUD_CHANNELS; ch++) {
        aud_tl_clear_channel(e, ch, true);
    }
    for (int i = 0; i < AUD_VOICES; i++) {
        aud_voice_t *v = &e->voices[i];
        if (v->song && v->env != ENV_OFF && !v->pend) {
            aud_voice_release(e, v);
        }
    }
    // Tempo changes the song queued are dropped with it.
    e->ntempos = 0;
}

// ---------------------------------------------------------------------------
// Producer side

static bool post_cmd(aud_engine_t *e, const aud_cmd_t *c)
{
    uint32_t tail = atomic_load_explicit(&e->cmds.tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&e->cmds.head, memory_order_acquire);
    if (tail - head >= AUD_CMDS) {
        return false;
    }
    e->cmds.buf[tail % AUD_CMDS] = *c;
    atomic_store_explicit(&e->cmds.tail, tail + 1, memory_order_release);
    return true;
}

uint32_t aud_apply_tick(const aud_engine_t *e, const aud_edit_opts_t *o)
{
    aud_engine_t *m = (aud_engine_t *)e;
    uint32_t clock = atomic_load_explicit(&m->pub_clock, memory_order_acquire);
    uint32_t spt = atomic_load(&m->pub_spt), frames = AUD_BLOCK_MAX;
    // The lock point: one (full) block past the tick being rendered (FR-49).
    uint32_t lock = clock + (spt ? (uint32_t)(((uint64_t)frames << 16) / spt) : 0) + 1;
    uint32_t t = lock;
    if (o && o->at && (int32_t)(o->at - lock) > 0) {
        t = o->at;
    }
    uint32_t q = o ? o->quant : 0;
    if (q > 1) {
        t = (uint32_t)(((uint64_t)t + q - 1) / q * q);
    }
    return t;
}

int64_t aud_transport(aud_engine_t *e, uint32_t bpm_q16, uint32_t ppqn, uint32_t beats, uint32_t quant)
{
    if (bpm_q16 < 20 * AUD_ONE || bpm_q16 > 300 * AUD_ONE || (ppqn && (ppqn < 24 || ppqn > 960)) ||
            (beats && (beats < 1 || beats > 16))) {
        return AUD_ERR_ARG;
    }
    uint32_t cur_p = atomic_load(&e->pub_ppqn), cur_b = atomic_load(&e->pub_beats);
    if ((ppqn && ppqn != cur_p) || (beats && beats != cur_b)) {
        aud_cmd_t c = { .type = CMD_TRANSPORT_STOP, .from = bpm_q16,
                        .value = (ppqn ? ppqn : cur_p) | (beats ? beats : cur_b) << 16 };
        // The ring drains at the next block: publish the new grid now so
        // the caller's next aud_grid() agrees.
        if (!post_cmd(e, &c)) {
            return AUD_ERR_FULL;
        }
        atomic_store(&e->pub_ppqn, ppqn ? ppqn : cur_p);
        atomic_store(&e->pub_beats, beats ? beats : cur_b);
        atomic_store(&e->pub_clock, 0);
        memset(e->last_apply, 0, sizeof(e->last_apply));
        return 0;
    }
    aud_edit_opts_t o = { .quant = quant };
    aud_cmd_t c = { .type = CMD_TEMPO, .apply = aud_apply_tick(e, &o), .value = bpm_q16, .block = 0xFF };
    return post_cmd(e, &c) ? (int64_t)c.apply : AUD_ERR_FULL;
}

bool aud_transport_stop(aud_engine_t *e)
{
    aud_cmd_t c = { .type = CMD_TRANSPORT_STOP, .block = 0xFF };
    if (!post_cmd(e, &c)) {
        return false;
    }
    atomic_store(&e->pub_clock, 0);
    memset(e->last_apply, 0, sizeof(e->last_apply));
    return true;
}

int64_t aud_edit(aud_engine_t *e, int op, int ch, uint32_t from, uint32_t to,
                 const aud_item_t *items, int n, const aud_edit_opts_t *o)
{
    if (op < AUD_ED_INSERT || op > AUD_ED_STOP || n < 0 || n > AUD_STAGE_ITEMS ||
            !((ch >= 0 && ch < AUD_CHANNELS) || (op == AUD_ED_STOP && ch == AUD_ALL))) {
        return AUD_ERR_ARG;
    }
    bool staged = op == AUD_ED_INSERT || op == AUD_ED_UPDATE;
    if (!staged) {
        n = 0;
    }
    aud_cold_t *cold = e->cold;
    aud_cmd_t c = { .type = CMD_EDIT, .op = (uint8_t)op, .ch = (uint8_t)ch, .from = from, .to = to,
                    .n = (uint16_t)n, .block = 0xFF };
    // The tick the edit takes effect at: the quant boundary (FR-49). An
    // insert touches nothing already there, so the mixer merges it at once
    // (it only needs to beat its first item); the others run at the
    // boundary. Either way an edit never runs before an earlier edit on the
    // same channel (FR-50: in the order they were made).
    uint32_t at = aud_apply_tick(e, o);
    uint32_t run = op == AUD_ED_INSERT ? aud_apply_tick(e, &(aud_edit_opts_t){ .quant = AUD_QUANT_NOW }) : at;
    int first = ch == AUD_ALL ? 0 : ch, last = ch == AUD_ALL ? AUD_CHANNELS - 1 : ch;
    for (int k = first; k <= last; k++) {
        if ((int32_t)(e->last_apply[k] - run) > 0) {
            run = e->last_apply[k];
        }
    }
    c.apply = run;
    if (op == AUD_ED_INSERT) {
        c.from = at;                        // the items' base
    }
    if (o) {
        c.flags = (uint8_t)((o->trim ? CMDF_TRIM : 0) | (o->kill ? CMDF_KILL : 0) | (o->reset ? CMDF_RESET : 0));
    }
    if (staged && n) {
        // Room: the free items the mixer reported, less what other edits in
        // flight will take. An update also frees its range, which only the
        // mixer can count: it may still refuse (counted in tl_rejected).
        uint32_t inflight = 0;
        int b = -1;
        for (int i = 0; i < AUD_STAGE_BLOCKS; i++) {
            if (atomic_load_explicit(&cold->stage_busy[i], memory_order_acquire)) {
                inflight += cold->stage_n[i];
            } else if (b < 0) {
                b = i;
            }
        }
        uint32_t free_ = atomic_load(&e->pub_tl_free);
        if (b < 0 || (op == AUD_ED_INSERT && (uint32_t)n + inflight > free_)) {
            atomic_fetch_add(&e->c_rejected, 1);
            return AUD_ERR_FULL;
        }
        for (int i = 0; i < n; i++) {
            aud_item_t it = items[i];
            if (it.kind >= AUD_IT_COUNT) {
                return AUD_ERR_ARG;
            }
            it.next = AUD_NONE16;
            cold->stage[b][i] = it;
        }
        cold->stage_n[b] = (uint16_t)n;
        atomic_store_explicit(&cold->stage_busy[b], 1, memory_order_release);
        c.block = (uint8_t)b;
    }
    if (!post_cmd(e, &c)) {
        if (c.block != 0xFF) {
            atomic_store(&cold->stage_busy[c.block], 0);
        }
        atomic_fetch_add(&e->c_rejected, 1);
        return AUD_ERR_FULL;
    }
    for (int k = first; k <= last; k++) {
        e->last_apply[k] = run;
    }
    return op == AUD_ED_INSERT ? at : run;
}

int64_t aud_song_play(aud_engine_t *e, int song, const aud_song_opts_t *o)
{
    if (song < 0 || song >= e->nsongs) {
        return AUD_ERR_ARG;
    }
    aud_edit_opts_t eo = { .quant = o ? o->quant : 0 };
    aud_cmd_t c = { .type = CMD_SONG_PLAY, .song = (uint8_t)song, .apply = aud_apply_tick(e, &eo), .block = 0xFF };
    if (o) {
        c.flags = (uint8_t)((o->loop ? CMDF_LOOP : 0) | (o->follow_tempo ? CMDF_FOLLOW : 0));
        c.value = o->tempo_scale_q16;
    }
    return post_cmd(e, &c) ? (int64_t)c.apply : AUD_ERR_FULL;
}

int64_t aud_song_pause(aud_engine_t *e, uint32_t quant)
{
    aud_edit_opts_t eo = { .quant = quant };
    aud_cmd_t c = { .type = CMD_SONG_PAUSE, .apply = aud_apply_tick(e, &eo), .block = 0xFF };
    return post_cmd(e, &c) ? (int64_t)c.apply : AUD_ERR_FULL;
}

int64_t aud_song_stop(aud_engine_t *e, uint32_t quant)
{
    aud_edit_opts_t eo = { .quant = quant };
    aud_cmd_t c = { .type = CMD_SONG_STOP, .apply = aud_apply_tick(e, &eo), .block = 0xFF };
    return post_cmd(e, &c) ? (int64_t)c.apply : AUD_ERR_FULL;
}
