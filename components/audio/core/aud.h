#pragma once

// Portable audio engine core: MIDI-driven synth and sampler voices, the tick
// clock with per-channel timelines, and a Standard MIDI File sequencer, mixed
// to 16-bit PCM. No ESP-IDF, FreeRTOS or floating point in here: the badge
// (components/audio/audio.c), the browser emulator and the host tests
// (components/audio/host/) all compile these files unchanged.
//
// Threads. Exactly two sides touch an engine:
//   producer  the app's thread (the Lua task on the badge): aud_note_on and
//             every other call below marked [P]. Calls never block and never
//             wait for the mixer; they write into single-producer rings.
//   consumer  the mixer (aud_render, aud_reset), marked [C]. It owns voices,
//             channels, timelines and the sequencer, so it needs no locks.
// A producer call made while aud_render runs on another core is safe; two
// producers at once are not (audio.c serialises its callers).
//
// Time. Immediate events carry a delay in samples from the last rendered
// sample (aud_now); the mixer applies them sample-accurately by splitting
// its block at the event. Musical events use the tick clock: a 32-bit count
// of ticks derived from the sample count with a fractional carry, so it never
// drifts (task spec FR-38 to FR-54).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUD_CHANNELS        16
#define AUD_VOICES          16
#define AUD_CHUNK           64          // frames between parameter updates
#define AUD_BLOCK_MAX       256         // most frames aud_render renders per call
#define AUD_QUEUE           256         // immediate events in flight (FR-8)
#define AUD_PENDING         256         // events the mixer holds sorted by time
#define AUD_HANDLES         64          // patch and bank handles, 1..AUD_HANDLES-1
#define AUD_PATCH_SLOTS     64          // patch storage (aud_patch_set uses a fresh slot)
#define AUD_BANKS           16
#define AUD_BANK_SAMPLES    32
#define AUD_PROGRAMS        32          // program-change map entries
#define AUD_MAPS            8           // audio.map_cc bindings per channel
#define AUD_MAP_DEFS        64          // map_cc definitions per run
#define AUD_TL_NODES        2048        // timeline items, all channels (FR-45)
#define AUD_STAGE_BLOCKS    4
#define AUD_STAGE_ITEMS     256         // items per edit
#define AUD_CMDS            32          // edits and transport commands in flight
#define AUD_RAMPS_VOICE     4
#define AUD_RAMPS_TOTAL     32
#define AUD_SONGS           4
#define AUD_IDS             16          // local note ids remembered per channel
#define AUD_TEMPOS          8           // tempo changes waiting for their tick

// Errors (negative returns).
enum {
    AUD_OK = 0,
    AUD_ERR_ARG = -1,       // a bad argument
    AUD_ERR_FULL = -2,      // a queue, table or pool is full
    AUD_ERR_NOMEM = -3,     // the sample pool is full
    AUD_ERR_FORMAT = -4,    // a file the loader can't read
    AUD_ERR_STEREO = -5,    // a stereo WAV
    AUD_ERR_STATE = -6,     // not now (e.g. no song loaded)
    AUD_ERR_NODEV = -7,     // no DAC (audio.c)
};

// Parameters, shared by the patch, channel and note layers (FR-24). Units:
//   Q16 fixed point unless noted; "ms" ones are whole milliseconds.
typedef enum {
    AUD_P_VOLUME,           // gain 0..2 (patch default 1)
    AUD_P_PAN,              // -1 left .. 1 right
    AUD_P_CUTOFF,           // 0..1, exponential from ~8 Hz to the limit (fs/6)
    AUD_P_RES,              // 0..1
    AUD_P_ATTACK,           // ms
    AUD_P_DECAY,            // ms
    AUD_P_SUSTAIN,          // 0..1
    AUD_P_RELEASE,          // ms
    AUD_P_VIB_RATE,         // LFO rate, Hz 0.1..20 (Q16)
    AUD_P_VIB_DEPTH,        // semitones 0..12
    AUD_P_VIB_DELAY,        // ms before vibrato and tremolo reach full depth
    AUD_P_TREM,             // 0..1
    AUD_P_PW,               // pulse width 0.05..0.95
    AUD_P_DETUNE,           // cents -100..100 (Q16)
    AUD_P_FENV,             // filter envelope amount -1..1
    AUD_P_PORTA,            // portamento time, ms
    AUD_P_COUNT
} aud_param_t;

const char *aud_param_name(int p);              // "volume", ... (task spec FR-30)
int aud_param_find(const char *name);           // -1 if unknown
// Clamp a value in a parameter's units to its range.
int32_t aud_param_clamp(int p, int32_t v);
// The unit conversion every front end needs: API value (a float scaled by
// 65536, e.g. 0.5 -> 32768; times as ms * 65536) to the parameter's units.
int32_t aud_param_from_q16(int p, int32_t q16);

enum { AUD_WAVE_SINE, AUD_WAVE_PULSE, AUD_WAVE_SAW, AUD_WAVE_TRI, AUD_WAVE_NOISE, AUD_WAVE_COUNT };
enum { AUD_LFO_PITCH = 1, AUD_LFO_AMP = 2, AUD_LFO_CUTOFF = 4 };

// A synth patch: plain data (FR-14). Notes copy it when they start.
typedef struct {
    uint8_t wave;           // AUD_WAVE_*
    uint8_t filter;         // 1: the low-pass is on
    uint8_t lfo_targets;    // AUD_LFO_* bits
    uint8_t reserved;
    int32_t p[AUD_P_COUNT]; // base values
    int32_t key_track;      // filter key tracking 0..1 (Q16)
    int32_t lfo_cutoff;     // LFO to cutoff depth, 0..1 of the cutoff range (Q16)
} aud_patch_t;

void aud_patch_default(aud_patch_t *p);         // sine, no filter, 5/80/0.8/150 ms

// One sample of a bank (FR-16). data points into the engine's pool.
typedef struct {
    const int16_t *data;
    uint32_t len;           // frames
    uint32_t loop_start, loop_end;  // loop_end > loop_start: sustain loop
    uint32_t rate;          // the recording's sample rate
    uint8_t root, key_lo, key_hi, vel_lo, vel_hi;
    uint8_t layer;          // CC 70 picks the layer (0 by default)
    int16_t fine;           // cents
    int32_t volume;         // Q16
    int32_t adsr[4];        // attack ms, decay ms, sustain Q16, release ms
} aud_sample_t;

void aud_sample_default(aud_sample_t *s);       // root 60, all keys, 2/0/1/50 ms

// Timeline items (FR-44): one scheduled thing on a channel. A note is one
// item with its length; the engine makes the note-off.
enum {
    AUD_IT_NOTE,            // a = note, b = velocity, v = length in ticks
    AUD_IT_CC,              // a = controller, b = value
    AUD_IT_BEND,            // v = -8192..8191; with ref: the note's bend, 1/256 semitone
    AUD_IT_PRESSURE,        // b = value; with ref (or a != 0xFF): that note's pressure
    AUD_IT_PROGRAM,         // a = program
    AUD_IT_PARAM,           // a = aud_param_t, v = value; with ref: that note only
    AUD_IT_COUNT
};
#define AUD_ITEM_SONG       0x01        // flags: written by the sequencer

typedef struct {
    uint32_t tick;          // in an edit: offset from the edit's start
    uint16_t next;          // engine use
    uint8_t kind;           // AUD_IT_*
    uint8_t a, b;
    uint8_t id;             // a note's local id (0: none)
    uint8_t ref;            // the note id a change addresses (0: the channel)
    uint8_t flags;
    int32_t v;
} aud_item_t;               // 16 bytes

// Edits on a channel's timeline (FR-46 to FR-52).
enum { AUD_ED_INSERT, AUD_ED_UPDATE, AUD_ED_DELETE, AUD_ED_STOP };
#define AUD_ALL             0xFF        // channel: every channel (stop)
#define AUD_QUANT_NOW       0           // quant: the earliest allowed tick

typedef struct {
    uint32_t quant;         // grid in ticks, or AUD_QUANT_NOW
    uint32_t at;            // earliest tick wanted (0: none)
    bool trim, kill, reset;
} aud_edit_opts_t;

typedef struct {
    uint32_t ppqn, beat, bar;   // ticks
    uint32_t bpm_q16;
} aud_grid_t;

typedef struct {
    uint32_t dropped_events;    // immediate events lost to a full queue
    uint32_t stale;             // calls on a note handle that has ended
    uint32_t ignored;           // CC 91/93/95 and other accepted-but-unused input
    uint32_t ramps_dropped;
    uint32_t tl_rejected;       // edits refused (timeline or staging full)
    uint32_t tl_late;           // items or edits that arrived after their tick
    uint32_t tl_free;           // timeline items free
    uint32_t voices_active;
    uint32_t pool_used, pool_free;
    uint32_t rate, channels;
    uint32_t clipped;           // samples the limiter could not hold under full scale
} aud_stats_t;

typedef struct aud_engine aud_engine_t;

// Memory: the engine is three blocks the caller allocates.
//   aud_engine_size()  hot state (voices, channels, rings): internal RAM
//   aud_cold_size()    timelines, staging, banks, patches, songs: PSRAM is fine
//   pool               sample and song data: PSRAM
size_t aud_engine_size(void);
size_t aud_cold_size(void);
// rate: 8000..48000; channels: 1 or 2. Clears everything.
void aud_init(aud_engine_t *e, void *cold, void *pool, size_t pool_size, uint32_t rate, int channels);
// [C] Silence and forget everything the app made (patches, banks, songs,
// timelines, the pool); keeps the rate and memory.
void aud_reset(aud_engine_t *e);
// [C] Render frames (<= AUD_BLOCK_MAX) of interleaved PCM16 (channels per
// frame as set at init).
void aud_render(aud_engine_t *e, int16_t *out, int frames);
// [C] Note trace for tests and the debug console: called by the mixer when
// a note starts sounding, starts its release (or 2 ms kill fade), and ends.
enum { AUD_TRACE_START, AUD_TRACE_RELEASE, AUD_TRACE_END };
typedef void (*aud_trace_fn)(void *ud, int what, int ch, int note, uint32_t serial, uint32_t sample, uint32_t tick);
void aud_set_trace(aud_engine_t *e, aud_trace_fn fn, void *ud);
// [C] Master gain (Q16, smoothed), and whether TPDF dither is on.
void aud_set_master(aud_engine_t *e, uint32_t gain_q16);
void aud_set_dither(aud_engine_t *e, bool on);
// [C] Frames since the last sounding voice or pending event (0 while busy).
uint32_t aud_idle_frames(const aud_engine_t *e);

uint32_t aud_rate(const aud_engine_t *e);
int aud_channels(const aud_engine_t *e);
void aud_stats(const aud_engine_t *e, aud_stats_t *out);    // [P] or [C]

// ---------------------------------------------------------------------------
// Producer side [P]

// Samples rendered so far (wraps after ~24 h at 48 kHz).
uint32_t aud_now(const aud_engine_t *e);
uint32_t aud_ms_to_samples(const aud_engine_t *e, uint32_t ms);

// Instruments. Handles are 1..AUD_HANDLES-1; <0 is an error.
int aud_patch_new(aud_engine_t *e, const aud_patch_t *p);
int aud_patch_set(aud_engine_t *e, int handle, const aud_patch_t *p);  // later notes only
int aud_bank_new(aud_engine_t *e);
int aud_bank_add(aud_engine_t *e, int handle, const aud_sample_t *s);
int aud_program_set(aud_engine_t *e, int bank, int program, int handle);  // a .mid's program changes
bool aud_handle_is_bank(const aud_engine_t *e, int handle);

// Pool: sample and song memory, freed only by aud_reset.
void *aud_pool_alloc(aud_engine_t *e, size_t n);
void aud_pool_used(const aud_engine_t *e, size_t *used, size_t *free_);

// Immediate events. delay: samples from aud_now(). Returns false when the
// queue was full (the event is dropped and counted).
bool aud_assign(aud_engine_t *e, int ch, int handle);
// Note on: returns the note's handle (a serial, never 0), or 0 when dropped.
// bundle: up to 8 parameter values applied from the first sample (FR-29).
typedef struct { uint8_t param; int32_t value; } aud_pv_t;
uint32_t aud_note_on(aud_engine_t *e, int ch, int note, int vel, uint32_t delay, const aud_pv_t *bundle, int n);
bool aud_note_off(aud_engine_t *e, int ch, int note, uint32_t delay);
bool aud_note_off_h(aud_engine_t *e, uint32_t handle, uint32_t delay);
bool aud_cc(aud_engine_t *e, int ch, int cc, int value, uint32_t delay);
bool aud_bend(aud_engine_t *e, int ch, int value14, uint32_t delay);         // -8192..8191
bool aud_pressure(aud_engine_t *e, int ch, int value, uint32_t delay);      // channel pressure 0..127
bool aud_poly_pressure(aud_engine_t *e, int ch, int note, int value, uint32_t delay);
bool aud_program(aud_engine_t *e, int ch, int program, uint32_t delay);
bool aud_param(aud_engine_t *e, int ch, int param, int32_t value, uint32_t delay);
bool aud_note_param(aud_engine_t *e, uint32_t handle, int param, int32_t value, uint32_t delay);
bool aud_note_ramp(aud_engine_t *e, uint32_t handle, int param, int32_t value, uint32_t ms, uint32_t delay);
bool aud_note_bend(aud_engine_t *e, uint32_t handle, int32_t semis_256, uint32_t delay);   // 1/256 semitone
bool aud_note_pressure(aud_engine_t *e, uint32_t handle, int32_t q16, uint32_t delay);
bool aud_pressure_target(aud_engine_t *e, int ch, int param, int32_t amount_q16);
bool aud_map_cc(aud_engine_t *e, int ch, int cc, int param, int32_t min, int32_t max);
bool aud_mpe(aud_engine_t *e, int first, int last);         // first > last: off
bool aud_midi(aud_engine_t *e, uint8_t status, uint8_t d1, uint8_t d2, uint32_t delay);
// All voices: release (kill = 2 ms fade), sustain and sostenuto cleared.
bool aud_all_off(aud_engine_t *e, bool kill);

// Clock and grid.
uint32_t aud_clock(const aud_engine_t *e);                  // tick being rendered
uint32_t aud_heard(const aud_engine_t *e, uint32_t latency_frames);
void aud_grid(const aud_engine_t *e, aud_grid_t *g);
// Tempo (bpm 20..300) applies at the next quant boundary; a ppqn (24..960) or
// beats (1..16) change restarts the transport (as aud_transport_stop first).
// Returns the tick the tempo applies at, or <0.
int64_t aud_transport(aud_engine_t *e, uint32_t bpm_q16, uint32_t ppqn, uint32_t beats, uint32_t quant);
// Clears every timeline, staged edit and song position and restarts the
// clock at 0 (applied at the start of the next block).
bool aud_transport_stop(aud_engine_t *e);
// The tick an edit with these options applies at (FR-49).
uint32_t aud_apply_tick(const aud_engine_t *e, const aud_edit_opts_t *o);
// Edits. ch 0..15 (AUD_ALL for stop). Returns the apply tick, or <0.
// insert: items[i].tick is an offset from the apply tick.
// update: replaces items starting in [from, to); offsets are from `from`.
int64_t aud_edit(aud_engine_t *e, int op, int ch, uint32_t from, uint32_t to,
                 const aud_item_t *items, int n, const aud_edit_opts_t *o);

// Songs: parse a Standard MIDI File (format 0 or 1) into the pool.
int aud_song_load(aud_engine_t *e, const uint8_t *data, size_t len);   // song id 0..AUD_SONGS-1
typedef struct {
    bool loop;
    bool follow_tempo;      // the file's tempo map drives the transport
    uint32_t tempo_scale_q16;   // with follow_tempo; 65536 = as written
    uint32_t quant;         // start boundary (ticks; 0 now)
} aud_song_opts_t;
int64_t aud_song_play(aud_engine_t *e, int song, const aud_song_opts_t *o);   // resumes after pause
int64_t aud_song_pause(aud_engine_t *e, uint32_t quant);
int64_t aud_song_stop(aud_engine_t *e, uint32_t quant);    // and back to the start
uint32_t aud_song_ticks(const aud_engine_t *e, int song);   // length in file ticks

// WAV loading (FR-15, FR-18): PCM 8/16-bit or IMA ADPCM, mono, decoded into
// the pool as PCM16. read() returns bytes read (0 at the end). The sample's
// data, len, rate and, if the file has a "smpl" chunk, loop are filled; the
// rest is left as the caller set it.
typedef size_t (*aud_read_fn)(void *ud, void *buf, size_t n);
int aud_wav_load(aud_engine_t *e, aud_read_fn read, void *ud, aud_sample_t *s);
// Decode from memory into a caller's buffer (host tests): frames written or <0.
int aud_wav_decode(const uint8_t *data, size_t len, int16_t *out, size_t max_frames, uint32_t *rate);
