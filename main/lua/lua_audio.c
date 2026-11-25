// The `audio` module (permission "audio"): the Lua face of components/audio.
// See docs/LUA_API.md "audio" and the component's README.
//
// Every call checks the permission, then hands the work to audio.c, which
// never blocks on the mixer. Loading a sample or a song reads the file here,
// in the callback, so it costs LH_FS_OPEN_STEPS plus one step per
// LH_FS_BYTES_PER_STEP bytes, like fs.read. Note handles are integers from
// 2^30 (the engine's serial in the low 30 bits; Lua integers are 32-bit
// here), so note_off(handle [, delay]) and note_off(channel, note [, delay])
// can't be confused. Once the app is closing (host_exit), calls do nothing:
// the OS has already released the engine and a late call must not restart it.

#include <stdlib.h>
#include <string.h>
#include "lua_priv.h"
#include "audio.h"

#define SONG_META   "audio.song"
#define STATS_META  "audio.stats"
#define NOTE_BIT    0x40000000u
#define NOTE_MASK   0x3FFFFFFFu
#define ITEM_STEPS  4               // per timeline item parsed

static const char *err_text(int rc)
{
    switch (rc) {
    case AUD_ERR_ARG: return "bad argument";
    case AUD_ERR_FULL: return "full";
    case AUD_ERR_NOMEM: return "out of audio memory";
    case AUD_ERR_FORMAT: return "not a file the audio loader reads";
    case AUD_ERR_STEREO: return "stereo WAV: convert it to mono";
    case AUD_ERR_STATE: return "not now";
    case AUD_ERR_NODEV: return "no DAC";
    default: return "error";
    }
}

// nil, message: the shape of every recoverable failure.
static int fail(lua_State *L, int rc, const char *what)
{
    lua_pushnil(L);
    if (what) {
        lua_pushfstring(L, "%s: %s", what, err_text(rc));
    } else {
        lua_pushstring(L, err_text(rc));
    }
    return 2;
}

// The permission, and whether the app is still running.
static bool live(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_AUDIO, "audio");
    return !lh_get(L)->quit;
}

#define LIVE()  do { if (!live(L)) return 0; } while (0)

static int push_rc(lua_State *L, int rc)
{
    if (rc < 0) {
        return fail(L, rc, NULL);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int push_tick(lua_State *L, int64_t rc)
{
    if (rc < 0) {
        return fail(L, (int)rc, NULL);
    }
    lua_pushinteger(L, (lua_Integer)rc);
    return 1;
}

static int check_ch(lua_State *L, int idx)
{
    lua_Integer ch = luaL_checkinteger(L, idx);
    luaL_argcheck(L, ch >= 0 && ch < AUD_CHANNELS, idx, "channel 0..15");
    return (int)ch;
}

static uint32_t opt_ms(lua_State *L, int idx)
{
    lua_Number v = luaL_optnumber(L, idx, 0);
    return v <= 0 ? 0 : v >= 3600000 ? 3600000 : (uint32_t)v;
}

static int check_param(lua_State *L, int idx)
{
    const char *name = luaL_checkstring(L, idx);
    int p = aud_param_find(name);
    if (p < 0) {
        luaL_argerror(L, idx, lua_pushfstring(L, "unknown parameter \"%s\"", name));
    }
    return p;
}

static uint32_t check_note(lua_State *L, int idx)
{
    lua_Integer h = luaL_checkinteger(L, idx);
    luaL_argcheck(L, h >= (lua_Integer)NOTE_BIT && h <= (lua_Integer)(NOTE_BIT | NOTE_MASK), idx,
                  "a note handle from audio.note_on");
    return (uint32_t)h & NOTE_MASK;
}

// Number fields of a table at idx (absolute).
static float fnum(lua_State *L, int idx, const char *k, float def)
{
    lua_getfield(L, idx, k);
    float v = lua_isnil(L, -1) ? def : (float)luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static float inum(lua_State *L, int idx, int i, float def)
{
    lua_geti(L, idx, i);
    float v = lua_isnil(L, -1) ? def : (float)luaL_checknumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static bool fbool(lua_State *L, int idx, const char *k, bool def)
{
    lua_getfield(L, idx, k);
    bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

// A sub-table field pushed (true) or nothing pushed (false).
static bool fsub(lua_State *L, int idx, const char *k)
{
    if (lua_getfield(L, idx, k) == LUA_TTABLE) {
        return true;
    }
    lua_pop(L, 1);
    return false;
}

// Quantise option: ticks, or "now", "beat", "bar".
static uint32_t opt_quant(lua_State *L, int idx, const char *k)
{
    uint32_t q = 0;
    lua_getfield(L, idx, k);
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *s = lua_tostring(L, -1);
        aud_grid_t g;
        audio_grid(&g);
        q = !strcmp(s, "bar") ? g.bar : !strcmp(s, "beat") ? g.beat : 0;
        if (strcmp(s, "bar") && strcmp(s, "beat") && strcmp(s, "now")) {
            luaL_error(L, "audio: quant is a tick count, \"now\", \"beat\" or \"bar\"");
        }
    } else if (!lua_isnil(L, -1)) {
        lua_Integer v = luaL_checkinteger(L, -1);
        q = v < 0 ? 0 : (uint32_t)v;
    }
    lua_pop(L, 1);
    return q;
}

// ---------------------------------------------------------------------------
// Set-up

static int l_available(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_AUDIO, "audio");
    lua_pushboolean(L, audio_available());
    return 1;
}

static int l_init(lua_State *L)
{
    LIVE();
    int rate = 32000, ch = 2;
    if (lua_istable(L, 1)) {
        rate = (int)fnum(L, 1, "rate", 32000);
        ch = (int)fnum(L, 1, "channels", 2);
    }
    return push_rc(L, audio_init(rate, ch));
}

// audio.reset(): everything the app made is gone and the engine stops; the
// next call starts it again (audio.init can then change the format).
static int l_reset(lua_State *L)
{
    LIVE();
    return push_rc(L, audio_reset());
}

// The last four are shorthands for wave = "noise" with that noise.
static const char *const s_waves[] = { "sine", "pulse", "saw", "triangle", "noise",
                                       "pink", "brown", "metallic", "digital", NULL };
static const char *const s_noises[] = { "white", "pink", "brown", "metallic", "digital", NULL };

static void read_synth(lua_State *L, int t, audio_synth_t *s)
{
    audio_synth_default(s);
    lua_getfield(L, t, "wave");
    if (!lua_isnil(L, -1)) {
        int w = luaL_checkoption(L, -1, NULL, s_waves);
        s->wave = w < AUD_WAVE_COUNT ? w : AUD_WAVE_NOISE;
        if (w >= AUD_WAVE_COUNT) {
            s->noise = w - AUD_WAVE_COUNT + AUD_NOISE_PINK;
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "noise");
    if (!lua_isnil(L, -1)) {
        s->noise = luaL_checkoption(L, -1, NULL, s_noises);
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "voices");
    if (!lua_isnil(L, -1)) {
        lua_Integer n = luaL_checkinteger(L, -1);
        luaL_argcheck(L, n >= 0 && n <= AUD_VOICES, 1, "voices is 0 (no limit) to 16");
        s->voices = (int)n;
    }
    lua_pop(L, 1);
    if (fbool(L, t, "mono", false)) {
        s->voices = 1;
    }
    s->legato = fbool(L, t, "legato", false);
    s->pw = fnum(L, t, "pw", s->pw);
    s->volume = fnum(L, t, "volume", s->volume);
    s->pan = fnum(L, t, "pan", 0);
    s->detune = fnum(L, t, "detune", 0);
    s->portamento = fnum(L, t, "portamento", 0);
    if (fsub(L, t, "adsr")) {
        int a = lua_gettop(L);
        s->attack = inum(L, a, 1, s->attack);
        s->decay = inum(L, a, 2, s->decay);
        s->sustain = inum(L, a, 3, s->sustain);
        s->release = inum(L, a, 4, s->release);
        lua_pop(L, 1);
    }
    if (fsub(L, t, "filter")) {
        int f = lua_gettop(L);
        s->filter = fbool(L, f, "on", true);
        s->cutoff = fnum(L, f, "cutoff", 1);
        s->res = fnum(L, f, "res", 0);
        s->fenv = fnum(L, f, "env", 0);
        s->key_track = fnum(L, f, "key_track", 0);
        lua_pop(L, 1);
    }
    if (fsub(L, t, "lfo")) {
        int f = lua_gettop(L);
        s->lfo_rate = fnum(L, f, "rate", s->lfo_rate);
        s->vibrato = fnum(L, f, "vibrato", 0);
        s->tremolo = fnum(L, f, "tremolo", 0);
        s->lfo_cutoff = fnum(L, f, "cutoff", 0);
        s->lfo_delay = fnum(L, f, "delay", 0);
        lua_pop(L, 1);
    }
}

// audio.synth{wave=, noise=, voices=, mono=, legato=, pw=, adsr={a,d,s,r}, filter={cutoff=, res=, env=,
// key_track=}, lfo={rate=, vibrato=, tremolo=, cutoff=, delay=}, volume=,
// pan=, detune=, portamento=} -> handle
static int l_synth(lua_State *L)
{
    LIVE();
    luaL_checktype(L, 1, LUA_TTABLE);
    audio_synth_t s;
    read_synth(L, 1, &s);
    int h = audio_synth_new(&s);
    if (h < 0) {
        return fail(L, h, "audio.synth");
    }
    lua_pushinteger(L, h);
    return 1;
}

// audio.synth_set(handle, {...}): notes started afterwards.
static int l_synth_set(lua_State *L)
{
    LIVE();
    int h = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    audio_synth_t s;
    read_synth(L, 2, &s);
    return push_rc(L, audio_synth_set(h, &s));
}

static size_t file_read(void *ud, void *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ud);
}

// One bank entry at t: {file=, root=, keys={lo,hi}, vel={lo,hi}, layer=,
// fine=, volume=, loop={start,end}, adsr={a,d,s,r}}.
static int add_sample(lua_State *L, int bank, int t)
{
    lh_t *s = lh_get(L);
    char rel[LUA_PKG_PATH_MAX + 1];
    lua_getfield(L, t, "file");
    lh_fs_check_path(L, lua_gettop(L), rel);
    lua_pop(L, 1);
    audio_sample_opts_t o;
    audio_sample_default(&o);
    o.root = (int)fnum(L, t, "root", (float)o.root);
    o.layer = (int)fnum(L, t, "layer", 0);
    o.fine = fnum(L, t, "fine", 0);
    o.volume = fnum(L, t, "volume", 1);
    if (fsub(L, t, "keys")) {
        o.key_lo = (int)inum(L, lua_gettop(L), 1, (float)o.key_lo);
        o.key_hi = (int)inum(L, lua_gettop(L), 2, (float)o.key_hi);
        lua_pop(L, 1);
    }
    if (fsub(L, t, "vel")) {
        o.vel_lo = (int)inum(L, lua_gettop(L), 1, (float)o.vel_lo);
        o.vel_hi = (int)inum(L, lua_gettop(L), 2, (float)o.vel_hi);
        lua_pop(L, 1);
    }
    if (fsub(L, t, "loop")) {
        o.loop_start = (int)inum(L, lua_gettop(L), 1, 0);
        o.loop_end = (int)inum(L, lua_gettop(L), 2, 0);
        lua_pop(L, 1);
    }
    if (fsub(L, t, "adsr")) {
        int a = lua_gettop(L);
        o.attack = inum(L, a, 1, o.attack);
        o.decay = inum(L, a, 2, o.decay);
        o.sustain = inum(L, a, 3, o.sustain);
        o.release = inum(L, a, 4, o.release);
        lua_pop(L, 1);
    }
    long size = 0;
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    FILE *f = lh_fs_fopen(s, rel, &size);
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: not found", rel);
        return -1;
    }
    lh_charge_steps(L, (unsigned)(size / LH_FS_BYTES_PER_STEP));
    int rc = audio_bank_add(bank, file_read, f, &o);
    fclose(f);
    if (rc < 0) {
        fail(L, rc, rel);
        return -1;
    }
    return 0;
}

// audio.bank{entry, ...} -> handle, or nil and why.
static int l_bank(lua_State *L)
{
    LIVE();
    luaL_checktype(L, 1, LUA_TTABLE);
    int h = audio_bank_new();
    if (h < 0) {
        return fail(L, h, "audio.bank");
    }
    lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, 1, i);
        luaL_argcheck(L, lua_istable(L, -1), 1, "a list of {file=...} tables");
        if (add_sample(L, h, lua_gettop(L)) < 0) {
            return 2;
        }
        lua_pop(L, 1);
    }
    lua_pushinteger(L, h);
    return 1;
}

// audio.bank_add(bank, entry) -> true, or nil and why.
static int l_bank_add(lua_State *L)
{
    LIVE();
    int h = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (add_sample(L, h, 2) < 0) {
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_program(lua_State *L)
{
    LIVE();
    return push_rc(L, audio_program((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                                    (int)luaL_checkinteger(L, 3)));
}

static int l_assign(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    return push_rc(L, audio_assign(ch, (int)luaL_optinteger(L, 2, 0)));
}

static int l_volume(lua_State *L)
{
    LIVE();
    return push_rc(L, audio_volume((float)luaL_checknumber(L, 1)));
}

// ---------------------------------------------------------------------------
// Events

// audio.note_on(ch, note, vel [, delay_ms [, {param=value, ...}]]) -> handle
static int l_note_on(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    int note = (int)luaL_checkinteger(L, 2);
    int vel = (int)luaL_optinteger(L, 3, 100);
    uint32_t d = opt_ms(L, 4);
    aud_pv_t pv[8];
    int n = 0;
    if (lua_istable(L, 5)) {
        lua_pushnil(L);
        while (lua_next(L, 5)) {
            if (n == 8) {
                luaL_argerror(L, 5, "at most 8 parameters");
            }
            int p = aud_param_find(lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "");
            if (p < 0) {
                luaL_argerror(L, 5, "parameter names as keys");
            }
            pv[n].param = (uint8_t)p;
            pv[n].value = audio_param_value(p, (float)luaL_checknumber(L, -1));
            n++;
            lua_pop(L, 1);
        }
    }
    uint32_t h = audio_note_on(ch, note, vel, d, pv, n);
    if (!h) {
        return fail(L, AUD_ERR_FULL, NULL);
    }
    lua_pushinteger(L, (lua_Integer)(NOTE_BIT | (h & NOTE_MASK)));
    return 1;
}

// audio.chord(ch, {notes} [, vel [, delay [, strum]]]) -> {handles}: the notes
// at once, or each strum ms after the one before. A note that could not start
// has no handle (its slot is false).
static int l_chord(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int vel = (int)luaL_optinteger(L, 3, 100);
    uint32_t d = opt_ms(L, 4);
    uint32_t strum = opt_ms(L, 5);
    int count = (int)luaL_len(L, 2);
    luaL_argcheck(L, count >= 1 && count <= AUD_VOICES, 2, "1 to 16 notes");
    int notes[AUD_VOICES];
    uint32_t h[AUD_VOICES];
    for (int i = 0; i < count; i++) {
        lua_geti(L, 2, i + 1);
        notes[i] = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    if (audio_chord(ch, notes, count, vel, d, strum, h) == 0) {
        return fail(L, AUD_ERR_FULL, NULL);
    }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        if (h[i]) {
            lua_pushinteger(L, (lua_Integer)(NOTE_BIT | (h[i] & NOTE_MASK)));
        } else {
            lua_pushboolean(L, 0);
        }
        lua_seti(L, -2, i + 1);
    }
    return 1;
}

// audio.note_off(handle [, delay]), audio.note_off({handles} [, delay]) (a
// chord) or audio.note_off(ch, note [, delay])
static int l_note_off(lua_State *L)
{
    LIVE();
    if (lua_istable(L, 1)) {
        uint32_t d = opt_ms(L, 2);
        int count = (int)luaL_len(L, 1);
        for (int i = 1; i <= count; i++) {
            if (lua_geti(L, 1, i) != LUA_TBOOLEAN) {
                lua_Integer h = luaL_checkinteger(L, -1);
                luaL_argcheck(L, h >= (lua_Integer)NOTE_BIT && h <= (lua_Integer)(NOTE_BIT | NOTE_MASK), 1,
                              "note handles from audio.chord or audio.note_on");
                audio_note_off_h((uint32_t)h & NOTE_MASK, d);
            }
            lua_pop(L, 1);
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_Integer a = luaL_checkinteger(L, 1);
    if (a >= (lua_Integer)NOTE_BIT) {
        return push_rc(L, audio_note_off_h(check_note(L, 1), opt_ms(L, 2)));
    }
    return push_rc(L, audio_note_off(check_ch(L, 1), (int)luaL_checkinteger(L, 2), opt_ms(L, 3)));
}

static int l_cc(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    return push_rc(L, audio_cc(ch, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), opt_ms(L, 4)));
}

static int l_bend(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    return push_rc(L, audio_bend(ch, (float)luaL_checknumber(L, 2), opt_ms(L, 3)));
}

static int l_pressure(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    return push_rc(L, audio_pressure(ch, (float)luaL_checknumber(L, 2), opt_ms(L, 3)));
}

static int l_midi(lua_State *L)
{
    LIVE();
    return push_rc(L, audio_midi((uint8_t)luaL_checkinteger(L, 1), (uint8_t)luaL_optinteger(L, 2, 0),
                                 (uint8_t)luaL_optinteger(L, 3, 0), opt_ms(L, 4)));
}

static int l_all_off(lua_State *L)
{
    LIVE();
    return push_rc(L, audio_all_off(lua_toboolean(L, 1)));
}

static int l_param(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    int p = check_param(L, 2);
    return push_rc(L, audio_param(ch, p, (float)luaL_checknumber(L, 3), opt_ms(L, 4)));
}

static int l_note_param(lua_State *L)
{
    LIVE();
    uint32_t h = check_note(L, 1);
    int p = check_param(L, 2);
    return push_rc(L, audio_note_param(h, p, (float)luaL_checknumber(L, 3), opt_ms(L, 4)));
}

static int l_note_ramp(lua_State *L)
{
    LIVE();
    uint32_t h = check_note(L, 1);
    int p = check_param(L, 2);
    return push_rc(L, audio_note_ramp(h, p, (float)luaL_checknumber(L, 3), opt_ms(L, 4)));
}

static int l_note_bend(lua_State *L)
{
    LIVE();
    uint32_t h = check_note(L, 1);
    return push_rc(L, audio_note_bend(h, (float)luaL_checknumber(L, 2), opt_ms(L, 3)));
}

static int l_note_pressure(lua_State *L)
{
    LIVE();
    uint32_t h = check_note(L, 1);
    return push_rc(L, audio_note_pressure(h, (float)luaL_checknumber(L, 2), opt_ms(L, 3)));
}

static int l_nrpn(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    return push_rc(L, audio_nrpn(ch, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3)));
}

static int l_rpn(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    return push_rc(L, audio_rpn(ch, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3)));
}

static int l_map_cc(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    int cc = (int)luaL_checkinteger(L, 2);
    int p = check_param(L, 3);
    return push_rc(L, audio_map_cc(ch, cc, p, (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)));
}

static int l_pressure_target(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    int p = check_param(L, 2);
    return push_rc(L, audio_pressure_target(ch, p, (float)luaL_optnumber(L, 3, 1)));
}

// audio.mpe{first=1, last=15}, or audio.mpe(false)
static int l_mpe(lua_State *L)
{
    LIVE();
    if (!lua_istable(L, 1)) {
        return push_rc(L, audio_mpe(1, 0));
    }
    return push_rc(L, audio_mpe((int)fnum(L, 1, "first", 1), (int)fnum(L, 1, "last", 15)));
}

// ---------------------------------------------------------------------------
// Songs

typedef struct {
    int id;
} song_t;

// audio.song(path) -> song, or nil and why
static int l_song(lua_State *L)
{
    LIVE();
    lh_t *s = lh_get(L);
    char rel[LUA_PKG_PATH_MAX + 1];
    lh_fs_check_path(L, 1, rel);
    long size = 0;
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    FILE *f = lh_fs_fopen(s, rel, &size);
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: not found", rel);
        return 2;
    }
    lh_charge_steps(L, (unsigned)(size / LH_FS_BYTES_PER_STEP));
    uint8_t *buf = size > 0 && size <= LUA_PKG_FILE_MAX ? malloc((size_t)size) : NULL;
    int rc = AUD_ERR_NOMEM;
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
        rc = audio_song_load(buf, (size_t)size);
    }
    free(buf);
    fclose(f);
    if (rc < 0) {
        return fail(L, rc, rel);
    }
    song_t *sg = lua_newuserdatauv(L, sizeof(song_t), 0);
    sg->id = rc;
    luaL_setmetatable(L, SONG_META);
    return 1;
}

// song:play{loop=, quant=, follow_song_tempo=, tempo=} -> start tick
static int m_play(lua_State *L)
{
    song_t *sg = luaL_checkudata(L, 1, SONG_META);
    LIVE();
    aud_song_opts_t o = { .loop = false, .follow_tempo = true, .tempo_scale_q16 = 65536 };
    if (lua_istable(L, 2)) {
        o.loop = fbool(L, 2, "loop", false);
        o.follow_tempo = fbool(L, 2, "follow_song_tempo", true);
        float t = fnum(L, 2, "tempo", 1);
        o.tempo_scale_q16 = (uint32_t)(t < 0.25f ? 16384 : t > 4 ? 262144 : t * 65536);
        o.quant = opt_quant(L, 2, "quant");
    }
    return push_tick(L, audio_song_play(sg->id, &o));
}

static int m_pause(lua_State *L)
{
    luaL_checkudata(L, 1, SONG_META);
    LIVE();
    return push_tick(L, audio_song_pause(lua_istable(L, 2) ? opt_quant(L, 2, "quant") : 0));
}

static int m_stop(lua_State *L)
{
    luaL_checkudata(L, 1, SONG_META);
    LIVE();
    return push_tick(L, audio_song_stop(lua_istable(L, 2) ? opt_quant(L, 2, "quant") : 0));
}

// ---------------------------------------------------------------------------
// Clock, grid and timelines

// audio.transport{bpm=, ppqn=, beats=, quant=} -> the tick the tempo applies at
static int l_transport(lua_State *L)
{
    LIVE();
    luaL_checktype(L, 1, LUA_TTABLE);
    aud_grid_t g;
    audio_grid(&g);
    float bpm = fnum(L, 1, "bpm", g.bpm_q16 ? g.bpm_q16 / 65536.0f : 120);
    int ppqn = (int)fnum(L, 1, "ppqn", 0);
    int beats = (int)fnum(L, 1, "beats", 0);
    return push_tick(L, audio_transport(bpm, ppqn, beats, opt_quant(L, 1, "quant")));
}

static int l_transport_stop(lua_State *L)
{
    LIVE();
    return push_rc(L, audio_transport_stop());
}

static int l_grid(lua_State *L)
{
    LIVE();
    aud_grid_t g;
    audio_grid(&g);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, g.ppqn);
    lua_setfield(L, -2, "ppqn");
    lua_pushinteger(L, g.beat);
    lua_setfield(L, -2, "beat");
    lua_pushinteger(L, g.bar);
    lua_setfield(L, -2, "bar");
    lua_pushnumber(L, g.bpm_q16 / 65536.0);
    lua_setfield(L, -2, "bpm");
    return 1;
}

static int l_clock(lua_State *L)
{
    LIVE();
    lua_pushinteger(L, audio_clock());
    return 1;
}

static int l_heard(lua_State *L)
{
    LIVE();
    lua_pushinteger(L, audio_heard());
    return 1;
}

static const char *const s_kinds[] = { "note", "cc", "bend", "pressure", "program", "param", NULL };

static uint8_t u8f(lua_State *L, int t, int i)
{
    lua_Integer v = (lua_geti(L, t, i), luaL_optinteger(L, -1, 0));
    lua_pop(L, 1);
    return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}

// {tick, kind, ...; id=, ref=} at t into *it.
static void read_item(lua_State *L, int t, aud_item_t *it)
{
    memset(it, 0, sizeof(*it));
    lua_geti(L, t, 1);
    lua_Integer tick = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    it->tick = tick < 0 ? 0 : (uint32_t)tick;
    lua_geti(L, t, 2);
    it->kind = (uint8_t)luaL_checkoption(L, -1, NULL, s_kinds);
    lua_pop(L, 1);
    it->id = (uint8_t)fnum(L, t, "id", 0);
    it->ref = (uint8_t)fnum(L, t, "ref", 0);
    switch (it->kind) {
    case AUD_IT_NOTE:
        it->a = u8f(L, t, 3);
        it->b = u8f(L, t, 4);
        it->v = (int32_t)inum(L, t, 5, 0);
        break;
    case AUD_IT_CC:
        it->a = u8f(L, t, 3);
        it->b = u8f(L, t, 4);
        break;
    case AUD_IT_BEND: {
        float v = inum(L, t, 3, 0);     // channel: -1..1 of the range; with ref: semitones
        it->v = it->ref ? (int32_t)(v * 256) : (int32_t)(v < -1 ? -8192 : v > 1 ? 8191 : v * 8191);
        break;
    }
    case AUD_IT_PRESSURE: {
        float v = inum(L, t, 3, 0);     // 0..1
        it->a = 0xFF;
        it->b = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 127 : v * 127);
        break;
    }
    case AUD_IT_PROGRAM:
        it->a = u8f(L, t, 3);
        break;
    case AUD_IT_PARAM: {
        lua_geti(L, t, 3);
        int p = check_param(L, lua_gettop(L));
        lua_pop(L, 1);
        it->a = (uint8_t)p;
        it->v = audio_param_value(p, inum(L, t, 4, 0));
        break;
    }
    }
}

// The item list at idx as a scratch array (a userdata the GC frees).
static aud_item_t *read_items(lua_State *L, int idx, int *n_out)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_Integer n = luaL_len(L, idx);
    luaL_argcheck(L, n >= 0 && n <= AUD_STAGE_ITEMS, idx, "at most 256 items");
    lh_charge_steps(L, (unsigned)n * ITEM_STEPS);
    aud_item_t *items = lua_newuserdatauv(L, (size_t)(n ? n : 1) * sizeof(aud_item_t), 0);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_geti(L, idx, i);
        luaL_argcheck(L, lua_istable(L, -1), idx, "items are {tick, kind, ...}");
        read_item(L, lua_gettop(L), &items[i - 1]);
        lua_pop(L, 1);
    }
    *n_out = (int)n;
    return items;
}

static void read_edit_opts(lua_State *L, int idx, aud_edit_opts_t *o)
{
    memset(o, 0, sizeof(*o));
    if (!lua_istable(L, idx)) {
        return;
    }
    o->quant = opt_quant(L, idx, "quant");
    lua_Number at = fnum(L, idx, "at", 0);
    o->at = at <= 0 ? 0 : (uint32_t)at;
    o->trim = fbool(L, idx, "trim", false);
    o->kill = fbool(L, idx, "kill", false);
    o->reset = fbool(L, idx, "reset", false);
}

static uint32_t check_tick(lua_State *L, int idx)
{
    lua_Integer t = luaL_checkinteger(L, idx);
    return t < 0 ? 0 : (uint32_t)t;
}

// audio.insert(ch, items, {at=, quant=}) -> the tick item offsets count from
static int l_insert(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    int n;
    aud_item_t *items = read_items(L, 2, &n);
    aud_edit_opts_t o;
    read_edit_opts(L, 3, &o);
    return push_tick(L, audio_insert(ch, items, n, &o));
}

// audio.update(ch, from, to, items, opts): replaces what starts in [from, to)
static int l_update(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    uint32_t from = check_tick(L, 2), to = check_tick(L, 3);
    int n;
    aud_item_t *items = read_items(L, 4, &n);
    aud_edit_opts_t o;
    read_edit_opts(L, 5, &o);
    return push_tick(L, audio_update(ch, from, to, items, n, &o));
}

static int l_delete(lua_State *L)
{
    LIVE();
    int ch = check_ch(L, 1);
    uint32_t from = check_tick(L, 2), to = check_tick(L, 3);
    aud_edit_opts_t o;
    read_edit_opts(L, 4, &o);
    return push_tick(L, audio_delete(ch, from, to, &o));
}

// audio.stop{channel=0..15 or "all", quant=, kill=, reset=}
static int l_stop(lua_State *L)
{
    LIVE();
    int ch = AUD_ALL;
    aud_edit_opts_t o;
    read_edit_opts(L, 1, &o);
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "channel");
        if (lua_type(L, -1) == LUA_TNUMBER) {
            ch = check_ch(L, lua_gettop(L));
        } else if (!lua_isnil(L, -1) && strcmp(luaL_checkstring(L, -1), "all")) {
            luaL_error(L, "audio.stop: channel is 0..15 or \"all\"");
        }
        lua_pop(L, 1);
    }
    return push_tick(L, audio_stop(ch, &o));
}

// ---------------------------------------------------------------------------
// Stats

static int stats_tostring(lua_State *L)
{
    lua_getfield(L, 1, "cpu");
    lua_getfield(L, 1, "peak_cpu");
    lua_getfield(L, 1, "underruns");
    lua_getfield(L, 1, "dropped_events");
    lua_getfield(L, 1, "voices_active");
    lua_getfield(L, 1, "pool_used");
    lua_getfield(L, 1, "pool_free");
    char buf[160];
    snprintf(buf, sizeof(buf), "cpu %.1f%% (peak %.1f%%), underruns %d, dropped %d, voices %d, pool %d/%d KB",
             (double)lua_tonumber(L, 2), (double)lua_tonumber(L, 3), (int)lua_tointeger(L, 4),
             (int)lua_tointeger(L, 5), (int)lua_tointeger(L, 6), (int)(lua_tointeger(L, 7) / 1024),
             (int)((lua_tointeger(L, 7) + lua_tointeger(L, 8)) / 1024));
    lua_pushstring(L, buf);
    return 1;
}

static int l_stats(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_AUDIO, "audio");
    audio_stats_t st;
    audio_stats(&st);
    lua_createtable(L, 0, 20);
#define SETI(k, v) (lua_pushinteger(L, (lua_Integer)(v)), lua_setfield(L, -2, k))
    lua_pushnumber(L, st.cpu);
    lua_setfield(L, -2, "cpu");
    lua_pushnumber(L, st.peak_cpu);
    lua_setfield(L, -2, "peak_cpu");
    SETI("underruns", st.underruns);
    SETI("dropped_events", st.dropped_events);
    SETI("voices_active", st.voices_active);
    SETI("pool_used", st.pool_used);
    SETI("pool_free", st.pool_free);
    SETI("rate", st.rate);
    SETI("channels", st.channels);
    SETI("stale", st.stale);
    SETI("ignored", st.ignored);
    SETI("ramps_dropped", st.ramps_dropped);
    SETI("tl_rejected", st.tl_rejected);
    SETI("tl_late", st.tl_late);
    SETI("tl_free", st.tl_free);
    SETI("clipped", st.clipped);
    SETI("latency", st.latency_frames);
#undef SETI
    lua_pushboolean(L, st.running);
    lua_setfield(L, -2, "running");
    lua_pushboolean(L, st.dac_awake);
    lua_setfield(L, -2, "dac_awake");
    luaL_setmetatable(L, STATS_META);
    return 1;
}

// ---------------------------------------------------------------------------

void lh_open_audio(lua_State *L)
{
    static const luaL_Reg song_methods[] = {{"play", m_play}, {"pause", m_pause}, {"stop", m_stop}, {NULL, NULL}};
    static const luaL_Reg fns[] = {
        {"available", l_available}, {"init", l_init}, {"reset", l_reset}, {"synth", l_synth}, {"synth_set", l_synth_set},
        {"bank", l_bank}, {"bank_add", l_bank_add}, {"program", l_program}, {"assign", l_assign},
        {"volume", l_volume}, {"note_on", l_note_on}, {"note_off", l_note_off}, {"chord", l_chord}, {"cc", l_cc},
        {"bend", l_bend}, {"pressure", l_pressure}, {"midi", l_midi}, {"all_off", l_all_off},
        {"param", l_param}, {"note_param", l_note_param}, {"note_ramp", l_note_ramp},
        {"note_bend", l_note_bend}, {"note_pressure", l_note_pressure}, {"nrpn", l_nrpn}, {"rpn", l_rpn},
        {"map_cc", l_map_cc}, {"pressure_target", l_pressure_target}, {"mpe", l_mpe}, {"song", l_song},
        {"transport", l_transport}, {"transport_stop", l_transport_stop}, {"grid", l_grid},
        {"clock", l_clock}, {"heard", l_heard}, {"insert", l_insert}, {"update", l_update},
        {"delete", l_delete}, {"stop", l_stop}, {"stats", l_stats}, {NULL, NULL}};
    luaL_newmetatable(L, SONG_META);
    luaL_newlib(L, song_methods);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "song");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    luaL_newmetatable(L, STATS_META);
    lua_pushcfunction(L, stats_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
    luaL_newlib(L, fns);
    lua_setglobal(L, "audio");
}

void lh_audio_stop(void)
{
    audio_release();
}

// "audio" console command: the same figures as audio.stats(), and the
// Settings controls for a badge without a screen at hand.
int lh_audio_cmd(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "stats";
    if (!strcmp(sub, "stats")) {
        audio_stats_t st;
        audio_stats(&st);
        printf("audio: %s, %s, volume %d%s\n", audio_available() ? "DAC found" : "no DAC",
               st.running ? (st.dac_awake ? "running" : "running, DAC asleep") : "idle",
               audio_setting_volume(), audio_setting_muted() ? " (muted)" : "");
        if (st.running) {
            printf("cpu %.1f%% peak %.1f%%, underruns %lu, dropped %lu, voices %lu, pool %lu used %lu free\n",
                   (double)st.cpu, (double)st.peak_cpu, (unsigned long)st.underruns,
                   (unsigned long)st.dropped_events, (unsigned long)st.voices_active,
                   (unsigned long)st.pool_used, (unsigned long)st.pool_free);
            printf("%lu Hz x %lu, latency %lu frames, stale %lu, late %lu, rejected %lu, clipped %lu\n",
                   (unsigned long)st.rate, (unsigned long)st.channels, (unsigned long)st.latency_frames,
                   (unsigned long)st.stale, (unsigned long)st.tl_late, (unsigned long)st.tl_rejected,
                   (unsigned long)st.clipped);
        }
        return 0;
    }
    if (!strcmp(sub, "tone") && argc == 3) {
        int rc = audio_test_tone(!strcmp(argv[2], "on"));
        printf("tone: %s\n", rc < 0 ? err_text(rc) : "ok");
        return rc < 0;
    }
    if (!strcmp(sub, "volume") && argc == 3) {
        audio_setting_set_volume(atoi(argv[2]));
        return 0;
    }
    if (!strcmp(sub, "mute") && argc == 3) {
        audio_setting_set_muted(!strcmp(argv[2], "on"));
        return 0;
    }
    printf("usage: audio [stats|tone on|off|volume 0..10|mute on|off]\n");
    return 1;
}
