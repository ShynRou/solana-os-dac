#pragma once

// Engine internals shared by the core's files. Nothing outside core/ includes
// this.

#include <stdatomic.h>
#include "aud.h"

#define AUD_NONE16          0xFFFF
#define AUD_ONE             65536       // 1.0 in Q16
#define AUD_ENV_ONE         (1 << 24)   // envelope full scale
#define AUD_MIN_ATTACK_MS   2           // FR-23
#define AUD_MIN_RELEASE_MS  5
#define AUD_KILL_MS         2           // stolen and killed voices
#define AUD_VOICE_SCALE     22938       // 0.35: one voice's peak, Q16 (the limiter takes the rest)
#define AUD_F0_Q16          535809      // MIDI note 0, 8.1758 Hz, Q16
#define AUD_SERIAL_MIXER    0x80000000u // serials of notes the mixer starts (timelines)

// ---------------------------------------------------------------------------
// Immediate events (16 bytes)

enum {
    EV_NOTE_ON = 1,         // ch, a note, b vel, serial
    EV_NOTE_PARAM_B,        // bundle entry right after its note-on: serial, a param, value
    EV_NOTE_OFF,            // ch, a note
    EV_NOTE_OFF_H,          // serial
    EV_CC,                  // ch, a cc, b value
    EV_BEND,                // ch, value -8192..8191
    EV_PRESSURE,            // ch, b value
    EV_POLY,                // ch, a note, b value
    EV_PROGRAM,             // ch, a program
    EV_PARAM,               // ch, a param, value
    EV_NOTE_PARAM,          // serial, a param, value
    EV_NOTE_RAMP,           // serial, a param, value, ch|b<<8 = ms (low 16 bits); see below
    EV_NOTE_BEND,           // serial, value 1/256 semitone
    EV_NOTE_PRESSURE,       // serial, value Q16
    EV_ASSIGN,              // ch, value handle
    EV_PTARGET,             // ch, a param, value amount
    EV_MAP_CC,              // ch, a cc, value def index
    EV_MPE,                 // a first, b last
    EV_ALL_OFF,             // a kill
};

typedef struct {
    uint32_t time;          // sample clock
    uint8_t type, ch, a, b;
    uint32_t serial;
    int32_t value;
} aud_ev_t;

_Static_assert(sizeof(aud_ev_t) == 16, "event is 16 bytes");

// ---------------------------------------------------------------------------
// Commands: edits and transport (tick scheduled)

enum {
    CMD_EDIT,               // op, ch, from, to, block, n, opts
    CMD_TEMPO,              // value bpm_q16
    CMD_TRANSPORT_STOP,     // applied when popped; value: new ppqn | beats << 16 (0: keep)
    CMD_SONG_PLAY,          // song, opts
    CMD_SONG_PAUSE,
    CMD_SONG_STOP,
};

typedef struct {
    uint32_t apply;         // tick
    uint32_t from, to;
    uint32_t value;
    uint16_t n;
    uint8_t type, op, ch, block, song, flags;
} aud_cmd_t;

#define CMDF_TRIM           0x01
#define CMDF_KILL           0x02
#define CMDF_RESET          0x04
#define CMDF_LOOP           0x08
#define CMDF_FOLLOW         0x10

// ---------------------------------------------------------------------------
// Voices

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE, ENV_KILL };

typedef struct {
    uint8_t param;          // AUD_P_*; 0xFF free
    int32_t from, to;
    uint32_t total, done;   // samples
} aud_ramp_t;

typedef struct {
    uint8_t env;            // ENV_*
    uint8_t ch, note, vel;
    uint8_t sustained;      // note-off arrived while the pedal was down
    uint8_t sostenuto;      // held by CC 66
    uint8_t is_sample;
    uint8_t from_tl;        // started by a timeline item
    uint8_t song;           // ... written by the sequencer
    uint8_t pend;           // a stolen voice: start the pending note when the fade ends
    uint8_t nramps;
    uint8_t wave;
    uint32_t serial;
    uint32_t age;           // samples since the note started (vibrato delay)
    uint32_t order;         // start order: oldest is stolen first
    uint32_t src_tick;      // the timeline item's start tick
    uint32_t end_tick;      // timeline notes: when the engine releases it
    uint8_t end_armed;
    // Note layer (FR-24): absolute overrides, a mask of which are set.
    uint32_t note_set;
    int32_t note_abs[AUD_P_COUNT];
    int32_t bend_note;      // 1/256 semitone
    int32_t pressure;       // Q16
    aud_ramp_t ramps[AUD_RAMPS_VOICE];
    // The instrument, copied at note start.
    aud_patch_t patch;
    const aud_sample_t *smp;
    // Oscillator / sample position
    uint32_t phase;         // synth: 32-bit phase; sampler: frame index
    uint32_t frac;          // sampler: 16-bit fraction
    uint32_t noise;         // LFSR
    uint32_t noise_ph;
    uint32_t lfo_phase;
    int32_t glide;          // portamento offset, 1/256 semitone, moving to 0
    int32_t glide_step;     // per sample, Q8 of the offset unit
    // Envelope
    int32_t level;          // Q24
    int32_t rate;           // per sample, for the current stage
    // Filter
    int32_t lp, bp;
    // Gains, Q16, ramped across a segment; kgl/kgr at full envelope level
    // (frozen when the voice is killed)
    int32_t gl, gr, kgl, kgr;
    uint32_t step;          // the last oscillator or sample step (frozen when killed)
} aud_voice_t;

// ---------------------------------------------------------------------------
// Channels

typedef struct {
    uint8_t cc;
    uint8_t def;            // index into map_defs, 0xFF none
} aud_map_t;

typedef struct {
    int32_t min, max;
    uint8_t param;
} aud_map_def_t;

typedef struct {
    uint16_t handle;        // instrument, 0 none
    uint8_t vol, expr, pan_cc;
    uint8_t mod;
    uint8_t sustain, sostenuto, soft, legato, porta_on, mono;
    uint8_t porta_src;      // CC 84: next note glides from here (0xFF none)
    uint8_t last_note;      // portamento source (0xFF none)
    uint8_t bank_msb, bank_lsb;
    uint8_t rpn_msb, rpn_lsb, nrpn_msb, nrpn_lsb, data_msb, data_lsb;
    uint8_t nrpn_active;    // the last select was NRPN
    uint8_t variation;      // CC 70
    uint8_t chan_pressure;
    int16_t bend;           // -8192..8191
    uint16_t bend_range;    // 1/256 semitone (RPN 0)
    int32_t tune_coarse, tune_fine;  // RPN 2, RPN 1: 1/256 semitone
    uint8_t lsb_seen;       // data entry LSB arrived since the last select
    uint32_t chan_set;      // which chan_abs are set (named params, NRPN, map_cc)
    int32_t chan_abs[AUD_P_COUNT];
    int32_t chan_rel[AUD_P_COUNT];  // CC offsets (offset params) or Q16 multipliers (times, rate)
    uint8_t wave_override;  // NRPN 0 (0xFF none)
    uint8_t filter_override, loop_override;  // NRPN 7, 8 (0xFF none)
    uint8_t lfo_targets_override;            // NRPN 5 (0xFF none)
    int32_t key_track_override;              // NRPN 3 (-1 none)
    int32_t sample_fine;    // NRPN 9, cents Q16
    uint8_t ptarget;        // pressure target param
    int32_t pamount;        // Q16
    aud_map_t maps[AUD_MAPS];
    // Timeline: a sorted list of future items
    uint16_t head;
    uint8_t song_muted;     // stop on this channel removed it from the song
    uint8_t id_next;
    uint8_t ids[AUD_IDS];
    uint32_t id_serial[AUD_IDS];
} aud_chan_t;

// ---------------------------------------------------------------------------
// Songs

enum { SE_NOTE, SE_CC, SE_BEND, SE_PRESSURE, SE_POLY, SE_PROGRAM, SE_TEMPO };

typedef struct {
    uint32_t tick;          // file ticks
    uint8_t kind, ch, a, b;
    uint32_t v;             // note length (file ticks), bend, tempo (us per quarter)
} aud_song_ev_t;

typedef struct {
    aud_song_ev_t *ev;
    uint32_t n;
    uint32_t division;      // file ticks per quarter
    uint32_t length;        // file ticks, whole bars
    uint32_t first_tempo;   // us per quarter
} aud_song_t;

typedef struct {
    int8_t song;            // playing song, -1 none
    uint8_t playing, loop, follow, paused;
    uint32_t scale_q16;
    uint32_t pos;           // next event to feed
    int64_t origin;         // transport tick of file tick 0 (this pass)
    uint32_t pause_ft;      // file tick where it paused
} aud_seq_t;

// ---------------------------------------------------------------------------
// Rings (single producer, single consumer)

typedef struct {
    _Atomic uint32_t head, tail;    // producer writes tail, consumer head
    aud_ev_t buf[AUD_QUEUE];
} aud_ring_t;

typedef struct {
    _Atomic uint32_t head, tail;
    aud_cmd_t buf[AUD_CMDS];
} aud_cmd_ring_t;

// ---------------------------------------------------------------------------
// Cold state (PSRAM is fine): read at note start and once a block.

typedef struct {
    uint8_t count;
    aud_sample_t s[AUD_BANK_SAMPLES];
} aud_bank_t;

typedef struct {
    aud_item_t nodes[AUD_TL_NODES];
    aud_item_t stage[AUD_STAGE_BLOCKS][AUD_STAGE_ITEMS];
    _Atomic uint8_t stage_busy[AUD_STAGE_BLOCKS];
    uint16_t stage_n[AUD_STAGE_BLOCKS];
    aud_patch_t patches[AUD_PATCH_SLOTS];
    uint8_t patch_used[AUD_PATCH_SLOTS];        // producer's allocation
    uint32_t patch_retired[AUD_PATCH_SLOTS];    // block count when replaced; reusable 2 blocks later
    aud_bank_t banks[AUD_BANKS];
    _Atomic uint8_t bank_count[AUD_BANKS];
    aud_song_t songs[AUD_SONGS];
    aud_map_def_t map_defs[AUD_MAP_DEFS];
    struct { uint8_t bank, program; uint16_t handle; } programs[AUD_PROGRAMS];
    _Atomic uint8_t nprograms;
} aud_cold_t;

// Handle map entries: 0 none, PATCH | slot, BANK | bank index.
#define H_PATCH             0x100
#define H_BANK              0x200

// ---------------------------------------------------------------------------
// The engine (hot state)

struct aud_engine {
    aud_cold_t *cold;
    uint8_t *pool;
    size_t pool_size;
    size_t pool_used;               // producer
    uint32_t rate;
    int nch;

    // Producer state
    uint32_t serial;                // last note serial handed out
    uint8_t nmap_defs;
    uint8_t nbanks, nsongs;
    uint32_t last_apply[AUD_CHANNELS];  // keeps edits on a channel in the order they were made
    _Atomic uint16_t handles[AUD_HANDLES];
    uint8_t handle_used[AUD_HANDLES];

    // Rings
    aud_ring_t ring;
    aud_cmd_ring_t cmds;

    // Mixer: sorted events and commands
    aud_ev_t pending[AUD_PENDING];
    int npending;
    aud_cmd_t cmdq[AUD_CMDS];
    int ncmd;
    struct { uint32_t tick, bpm_q16; } tempos[AUD_TEMPOS];
    int ntempos;

    aud_voice_t voices[AUD_VOICES];
    aud_chan_t chans[AUD_CHANNELS];
    uint32_t voice_order;
    uint32_t mixer_serial;
    int ramps_total;
    uint8_t mpe_first, mpe_last;    // first > last: off

    // Clock (mixer)
    uint32_t samples;               // sample clock
    uint32_t clock;                 // tick
    uint32_t tick_pos;              // Q16 samples into the tick
    uint32_t spt;                   // samples per tick, Q16
    uint32_t bpm_q16, ppqn, beats;
    uint32_t tick_done;             // ticks up to here are processed
    bool tick_fresh;                // tick_done is unset (start of a run)
    uint16_t tl_free_head;
    uint32_t tl_free;
    uint32_t blocks;                // blocks rendered (patch slot reuse)

    // Published to the producer
    _Atomic uint32_t pub_samples, pub_clock, pub_spt, pub_bpm, pub_ppqn, pub_beats, pub_tl_free,
                     pub_blocks, pub_block_frames;
    _Atomic uint32_t c_dropped, c_stale, c_ignored, c_ramps, c_rejected, c_late, c_voices, c_clipped;

    aud_seq_t seq;

    // Master
    uint32_t master, master_target; // Q16
    uint32_t lim_gain;              // Q16
    bool dither;
    uint32_t dither_rng;
    uint32_t idle;                  // frames since anything sounded or was pending

    aud_trace_fn trace;
    void *trace_ud;
    int render_pos;                 // where the current segment starts in acc_*
    int32_t acc_l[AUD_BLOCK_MAX], acc_r[AUD_BLOCK_MAX];
    int16_t tmp[AUD_CHUNK];
};

// ---------------------------------------------------------------------------
// Between the core's files

// aud_engine.c
uint32_t aud_pitch_ratio_q16(int32_t p);    // 2^(p / 3072), p in 1/256 semitone
int32_t aud_tri_lfo(uint32_t phase);
static inline int32_t aud_mul16(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}
static inline int32_t aud_clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static inline bool aud_time_before(uint32_t a, uint32_t b)     // sample time a < b, wrap-safe
{
    return (int32_t)(a - b) < 0;
}
static inline uint32_t aud_ms_samples(uint32_t rate, uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * rate / 1000);
}

#define AUD_TRACE(e, what, v) do { if ((e)->trace) (e)->trace((e)->trace_ud, (what), (v)->ch, (v)->note, \
    (v)->serial, (e)->samples, (e)->clock); } while (0)

// aud_voice.c
void aud_voice_render(aud_engine_t *e, aud_voice_t *v, int n);
void aud_voice_release(aud_engine_t *e, aud_voice_t *v);
void aud_voice_kill(aud_engine_t *e, aud_voice_t *v);
void aud_voice_off(aud_engine_t *e, aud_voice_t *v);   // a note-off through the pedals
uint32_t aud_note_start(aud_engine_t *e, int ch, int note, int vel, uint32_t serial, aud_voice_t **out);
void aud_note_stop(aud_engine_t *e, int ch, int note);
aud_voice_t *aud_voice_find(aud_engine_t *e, uint32_t serial);
void aud_voice_param(aud_engine_t *e, aud_voice_t *v, int p, int32_t value);
void aud_voice_ramp(aud_engine_t *e, aud_voice_t *v, int p, int32_t value, uint32_t ms);
void aud_voice_clear_ramps(aud_engine_t *e, aud_voice_t *v);
int32_t aud_eff(const aud_engine_t *e, const aud_voice_t *v, int p);

// aud_chan.c
void aud_chan_reset(aud_engine_t *e, int ch, bool all);
void aud_chan_cc(aud_engine_t *e, int ch, int cc, int value);
void aud_chan_program(aud_engine_t *e, int ch, int program);
void aud_chan_pedal_release(aud_engine_t *e, int ch);
void aud_apply_event(aud_engine_t *e, const aud_ev_t *ev);
int32_t aud_param_mult_kind(int p);         // 1: CC multiplies this param

// aud_timeline.c
void aud_tl_init(aud_engine_t *e);
void aud_tl_process(aud_engine_t *e, uint32_t tick);        // everything due at tick
uint32_t aud_tl_next(aud_engine_t *e);                      // next tick with work (UINT32_MAX none)
void aud_tl_take_cmds(aud_engine_t *e);
void aud_tl_set_tempo(aud_engine_t *e, uint32_t bpm_q16);
void aud_tl_clear_channel(aud_engine_t *e, int ch, bool song_only);
void aud_seq_feed(aud_engine_t *e);
void aud_seq_start(aud_engine_t *e, const aud_cmd_t *c);
void aud_seq_halt(aud_engine_t *e, bool rewind);
int aud_tl_insert(aud_engine_t *e, int ch, const aud_item_t *it);
