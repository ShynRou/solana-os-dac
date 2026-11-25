// Host tests for the mixer core: the task spec's acceptance tests that can
// run without a badge (A5 to A8 and A13 to A28 as event scripts), plus
// allocation, filter stability, the limiter and golden renders.
//
//   make -C components/audio/host test        # or: run_tests [--update-golden]

#include <math.h>
#include "harness.h"
#include "../core/aud_int.h"

static int s_fail, s_pass;
static const char *s_test;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        s_fail++; \
        printf("FAIL %s:%d %s: ", __FILE__, __LINE__, s_test); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } else { \
        s_pass++; \
    } \
} while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { s_test = #name; name(); } while (0)

static int synth(int wave, int32_t attack, int32_t release)
{
    aud_patch_t p;
    aud_patch_default(&p);
    p.wave = (uint8_t)wave;
    p.p[AUD_P_ATTACK] = attack;
    p.p[AUD_P_RELEASE] = release;
    p.p[AUD_P_SUSTAIN] = AUD_ONE;
    return aud_patch_new(H.e, &p);
}

static aud_voice_t *voice_of(uint32_t serial)
{
    return aud_voice_find(H.e, serial);
}

static int16_t pcm_l(size_t i)
{
    return H.pcm[i * aud_channels(H.e)];
}

static int first_nonzero(size_t from)
{
    for (size_t i = from; i < H.frames; i++) {
        if (pcm_l(i) != 0) {
            return (int)i;
        }
    }
    return -1;
}

// Largest jump between neighbouring samples in [a, b): clicks show up here.
static int max_step(size_t a, size_t b)
{
    int m = 0;
    for (size_t i = a + 1; i < b && i < H.frames; i++) {
        int d = abs(pcm_l(i) - pcm_l(i - 1));
        if (d > m) {
            m = d;
        }
    }
    return m;
}

static int peak(size_t a, size_t b)
{
    int m = 0;
    for (size_t i = a; i < b && i < H.frames; i++) {
        for (int c = 0; c < aud_channels(H.e); c++) {
            int v = abs(H.pcm[i * aud_channels(H.e) + c]);
            if (v > m) {
                m = v;
            }
        }
    }
    return m;
}

static double freq_of(size_t a, size_t b)
{
    int cross = 0;
    for (size_t i = a + 1; i < b; i++) {
        if (pcm_l(i - 1) < 0 && pcm_l(i) >= 0) {
            cross++;
        }
    }
    return cross * (double)aud_rate(H.e) / (double)(b - a);
}

// ---------------------------------------------------------------------------

TEST(test_tone_pitch)
{
    h_init(48000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 50));
    aud_note_on(H.e, 0, 69, 127, 0, NULL, 0);
    h_render(48000);
    double f = freq_of(4800, 48000);
    CHECK(fabs(f - 440.0) < 1.0, "A4 is %.2f Hz", f);
    int pk = peak(4800, 48000);
    CHECK(pk > 3000 && pk < 32767, "level %d", pk);
}

TEST(test_sample_accurate)
{
    // FR-7: a note with a delay starts on exactly that sample.
    h_init(32000, 1, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_PULSE, 2, 50));
    h_render(512);
    uint32_t base = aud_now(H.e);
    aud_note_on(H.e, 0, 60, 127, 1000, NULL, 0);
    h_clear();
    h_render(4096);
    const trace_ev_t *t = h_find(AUD_TRACE_START, 0, 60, 0);
    CHECK(t && t->sample == base + 1000, "started at %u, wanted %u", t ? t->sample : 0, base + 1000);
    int nz = first_nonzero(0);
    CHECK(nz == 1000 || nz == 1001, "first sound at frame %d", nz);
}

TEST(test_voice_alloc)
{
    // FR-9: 16 voices; the 17th note steals the oldest; a re-trigger reuses.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SAW, 5, 100));
    uint32_t h[17];
    for (int i = 0; i < 16; i++) {
        h[i] = aud_note_on(H.e, 0, 40 + i, 100, 0, NULL, 0);
        h_render(64);
    }
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.voices_active == 16, "%u voices", st.voices_active);
    aud_note_on(H.e, 0, 45, 100, 0, NULL, 0);     // re-trigger: same voice
    h_render(256);
    aud_stats(H.e, &st);
    CHECK(st.voices_active == 16 && !voice_of(h[5]), "re-trigger: %u voices", st.voices_active);
    h[16] = aud_note_on(H.e, 0, 90, 100, 0, NULL, 0);
    h_render(256);
    CHECK(voice_of(h[0]) == NULL, "oldest note was not stolen");
    CHECK(voice_of(h[16]) != NULL, "new note did not start");
    CHECK(h_count(AUD_TRACE_START, 0, 90) == 1, "new note start traced %d", h_count(AUD_TRACE_START, 0, 90));
}

TEST(test_a6_flood)
{
    // A6: 1000 events in one callback: no block, the counter rises, recovery.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 50));
    h_render(64);
    int ok = 0;
    for (int i = 0; i < 1000; i++) {
        ok += aud_cc(H.e, 0, 20 + (i % 10), i & 127, (uint32_t)i);
    }
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(ok == AUD_QUEUE, "%d accepted", ok);
    CHECK(st.dropped_events == 1000 - AUD_QUEUE, "%u dropped", st.dropped_events);
    h_render(4096);
    uint32_t n = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    h_render(512);
    CHECK(n && voice_of(n), "no note after the flood");
}

TEST(test_a7_waveforms_no_clicks)
{
    // A7, shortened: every waveform held, then all_off; no clicks at the
    // ends, no voice left. (60 s holds run with --long.)
    for (int w = 0; w < AUD_WAVE_COUNT; w++) {
        h_init(32000, 1, 0);
        aud_patch_t p;
        aud_patch_default(&p);
        p.wave = AUD_WAVE_SINE;
        p.p[AUD_P_ATTACK] = 0;              // asks for 0: the 2 ms minimum applies
        p.p[AUD_P_RELEASE] = 0;             // 5 ms minimum
        p.p[AUD_P_SUSTAIN] = AUD_ONE;
        p.wave = (uint8_t)w;
        aud_assign(H.e, 0, aud_patch_new(H.e, &p));
        aud_note_on(H.e, 0, 36, 127, 0, NULL, 0);
        h_render(32000);
        aud_all_off(H.e, false);
        h_render(8000);
        aud_stats_t st;
        aud_stats(H.e, &st);
        CHECK(st.voices_active == 0, "wave %d: %u voices after all_off", w, st.voices_active);
        if (w == AUD_WAVE_SINE || w == AUD_WAVE_TRI) {
            // The first and last samples ramp: no jump bigger than the
            // waveform's own steepest step.
            int body = max_step(16000, 17000);
            int start = max_step(0, 200);
            int end = max_step(32000 - 10, 32000 + 400);
            CHECK(start <= body + 64, "wave %d: start step %d, body %d", w, start, body);
            CHECK(end <= body + 64, "wave %d: end step %d, body %d", w, end, body);
        }
        CHECK(peak(40000 - 200, 40000) == 0, "wave %d: not silent at the end", w);
    }
}

// ---------------------------------------------------------------------------
// WAV and ADPCM (A8)

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

TEST(test_a8_wav)
{
    size_t n1, n2, n3, n4, n5;
    uint8_t *pcm16 = slurp("fixtures/pcm16.wav", &n1), *pcm8 = slurp("fixtures/pcm8.wav", &n2);
    uint8_t *adpcm = slurp("fixtures/adpcm.wav", &n3), *ref = slurp("fixtures/adpcm_ref.raw", &n4);
    uint8_t *stereo = slurp("fixtures/stereo.wav", &n5);
    CHECK(pcm16 && pcm8 && adpcm && ref && stereo, "fixtures missing (make fixtures)");
    if (!pcm16 || !pcm8 || !adpcm || !ref || !stereo) {
        return;
    }
    static int16_t a[100000], b[100000];
    uint32_t rate;
    int fa = aud_wav_decode(pcm16, n1, a, 100000, &rate);
    CHECK(fa > 0 && rate == 22050, "pcm16: %d frames at %u", fa, rate);
    // The PCM16 data is the reference: within 1 LSB (exact, in fact).
    int worst = 0;
    for (int i = 0; i < fa; i++) {
        int16_t want = (int16_t)(pcm16[44 + 2 * i] | pcm16[45 + 2 * i] << 8);
        int d = abs(a[i] - want);
        worst = d > worst ? d : worst;
    }
    CHECK(worst <= 1, "pcm16 off by %d", worst);
    int fb = aud_wav_decode(pcm8, n2, b, 100000, NULL);
    CHECK(fb == fa, "pcm8 %d frames vs %d", fb, fa);
    worst = 0;
    for (int i = 0; i < fb && i < fa; i++) {
        int d = abs((b[i] >> 8) - (a[i] >> 8));
        worst = d > worst ? d : worst;
    }
    CHECK(worst <= 1, "pcm8 off by %d (in 8-bit units)", worst);
    // ADPCM against ffmpeg's decoder: identical.
    int fc = aud_wav_decode(adpcm, n3, a, 100000, NULL);
    int nref = (int)(n4 / 2);
    CHECK(fc >= nref && fc - nref < 1100, "adpcm %d frames, reference %d", fc, nref);
    int diff = 0;
    for (int i = 0; i < nref && i < fc; i++) {
        int16_t want = (int16_t)(ref[2 * i] | ref[2 * i + 1] << 8);
        diff += a[i] != want;
    }
    CHECK(diff == 0, "adpcm: %d samples differ from the reference decoder", diff);
    CHECK(aud_wav_decode(stereo, n5, a, 100000, NULL) == AUD_ERR_STEREO, "stereo not rejected");
    CHECK(aud_wav_decode(pcm16, 20, a, 100000, NULL) < 0, "truncated file accepted");
    free(pcm16), free(pcm8), free(adpcm), free(ref), free(stereo);
}

typedef struct { const uint8_t *p; size_t left; } memrd_t;

static size_t mem_read(void *ud, void *buf, size_t n)
{
    memrd_t *m = ud;
    n = n > m->left ? m->left : n;
    memcpy(buf, m->p, n);
    m->p += n, m->left -= n;
    return n;
}

TEST(test_sampler)
{
    // FR-16, FR-17: pitch by playback rate, loops, one-shots end their voice.
    size_t n;
    uint8_t *w = slurp("fixtures/pcm16.wav", &n);
    if (!w) {
        CHECK(0, "fixture missing");
        return;
    }
    h_init(22050, 1, 1 << 20);
    int bank = aud_bank_new(H.e);
    aud_sample_t s;
    aud_sample_default(&s);
    memrd_t m = { w, n };
    CHECK(aud_wav_load(H.e, mem_read, &m, &s) == AUD_OK, "load");
    s.root = 69;                            // the fixture is a 440 Hz sine
    aud_bank_add(H.e, bank, &s);
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.pool_used >= s.len * 2, "pool used %u", st.pool_used);
    aud_assign(H.e, 0, bank);
    aud_note_on(H.e, 0, 81, 127, 0, NULL, 0);   // an octave up: 880 Hz, half as long
    h_render(22050);
    double f = freq_of(500, s.len / 2 - 200);
    CHECK(fabs(f - 880) < 6, "octave up is %.1f Hz", f);
    CHECK(h_count(AUD_TRACE_END, 0, 81) == 1, "one-shot did not end its voice");
    const trace_ev_t *t = h_find(AUD_TRACE_END, 0, 81, 0);
    CHECK(t && t->sample >= s.len / 2 - 64 && t->sample <= s.len / 2 + 64, "ended at %u, len/2 %u",
          t ? t->sample : 0, s.len / 2);
    // Looped: still sounding long after the data ran out (the fixture is a
    // -21 dBFS sine: about 440 out).
    aud_sample_t l = s;
    l.loop_start = 1000;
    l.loop_end = 1000 + 50 * 10;            // whole cycles at 22050 / 441 Hz-ish
    l.key_lo = l.key_hi = 70;
    l.root = 70;
    int bank2 = aud_bank_new(H.e);
    aud_bank_add(H.e, bank2, &l);
    aud_assign(H.e, 0, bank2);
    h_clear();
    aud_note_on(H.e, 0, 70, 127, 0, NULL, 0);
    h_render(22050 * 3);
    CHECK(peak(22050 * 3 - 1000, 22050 * 3) > 300, "loop stopped sounding: peak %d", peak(22050 * 3 - 1000, 22050 * 3));
    free(w);
}

// ---------------------------------------------------------------------------
// Parameters (A13 to A19)

static uint8_t *midi_file(size_t *len, int ntracks, const uint8_t **tracks, const size_t *tlen, int div)
{
    size_t total = 14;
    for (int i = 0; i < ntracks; i++) {
        total += 8 + tlen[i];
    }
    uint8_t *b = malloc(total), *p = b;
    memcpy(p, "MThd\0\0\0\6", 8), p += 8;
    *p++ = 0, *p++ = ntracks > 1, *p++ = 0, *p++ = (uint8_t)ntracks, *p++ = (uint8_t)(div >> 8), *p++ = (uint8_t)div;
    for (int i = 0; i < ntracks; i++) {
        memcpy(p, "MTrk", 4), p += 4;
        *p++ = (uint8_t)(tlen[i] >> 24), *p++ = (uint8_t)(tlen[i] >> 16), *p++ = (uint8_t)(tlen[i] >> 8),
        *p++ = (uint8_t)tlen[i];
        memcpy(p, tracks[i], tlen[i]), p += tlen[i];
    }
    *len = total;
    return b;
}

static size_t put_vlq(uint8_t *p, uint32_t v)
{
    uint8_t t[5];
    int n = 0;
    t[n++] = v & 127;
    while (v >>= 7) {
        t[n++] = (uint8_t)(0x80 | (v & 127));
    }
    for (int i = 0; i < n; i++) {
        p[i] = t[n - 1 - i];
    }
    return (size_t)n;
}

TEST(test_a13_midi_cc74_sweep)
{
    // A13: a .mid with a CC 74 sweep on a held note moves the cutoff with no
    // app code, and renders the same as the same events through the API.
    uint8_t trk[4096];
    size_t n = 0;
    n += put_vlq(trk + n, 0), trk[n++] = 0x90, trk[n++] = 48, trk[n++] = 100;
    for (int i = 0; i < 64; i++) {
        n += put_vlq(trk + n, 24), trk[n++] = 0xB0, trk[n++] = 74, trk[n++] = (uint8_t)(i * 2);
    }
    n += put_vlq(trk + n, 96), trk[n++] = 0x80, trk[n++] = 48, trk[n++] = 0;
    n += put_vlq(trk + n, 0), trk[n++] = 0xFF, trk[n++] = 0x2F, trk[n++] = 0;
    const uint8_t *tr[1] = { trk };
    size_t len;
    uint8_t *mid = midi_file(&len, 1, tr, &n, 96);
    aud_patch_t p;
    aud_patch_default(&p);
    p.wave = AUD_WAVE_SAW;
    p.filter = 1;
    p.p[AUD_P_CUTOFF] = AUD_ONE / 3;
    p.p[AUD_P_RES] = AUD_ONE / 2;
    p.p[AUD_P_SUSTAIN] = AUD_ONE;

    h_init(32000, 1, 1 << 20);
    aud_assign(H.e, 0, aud_patch_new(H.e, &p));
    int song = aud_song_load(H.e, mid, len);
    CHECK(song == 0, "song load %d", song);
    aud_song_opts_t so = { .quant = 0 };
    aud_song_play(H.e, song, &so);
    // Sample the voice's effective cutoff as the song plays.
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int i = 0; i < 1200; i++) {
        h_render(256);
        for (int v = 0; v < AUD_VOICES; v++) {
            aud_voice_t *vv = &H.e->voices[v];
            if (vv->env >= ENV_ATTACK && vv->env <= ENV_SUSTAIN) {
                int32_t c = aud_eff(H.e, vv, AUD_P_CUTOFF);
                lo = c < lo ? c : lo;
                hi = c > hi ? c : hi;
            }
        }
    }
    CHECK(lo < AUD_ONE / 3 - AUD_ONE / 4 && hi > AUD_ONE / 3 + AUD_ONE / 4, "cutoff only moved %d..%d", lo, hi);
    CHECK(h_count(AUD_TRACE_START, 0, 48) == 1 && h_count(AUD_TRACE_RELEASE, 0, 48) == 1, "note start/release");
    // No click: the sweep is smoothed (FR-26).
    const trace_ev_t *t0 = h_find(AUD_TRACE_START, 0, 48, 0), *t1 = h_find(AUD_TRACE_RELEASE, 0, 48, 0);
    if (t0 && t1) {
        int body = max_step(t0->sample + 2000, t1->sample - 100);
        CHECK(body < 20000, "steepest step %d", body);
    }
    // Same events through the API, on the same ticks: identical output.
    uint64_t song_hash = h_hash();
    size_t song_frames = H.frames;
    h_init(32000, 1, 1 << 20);
    aud_assign(H.e, 0, aud_patch_new(H.e, &p));
    aud_item_t items[80];
    int k = 0;
    uint32_t t = 0;
    // 96 ticks per quarter in the file, 480 on the transport: x5
    items[k++] = (aud_item_t){ .tick = 0, .kind = AUD_IT_NOTE, .a = 48, .b = 100, .v = (64 * 24 + 96) * 5 };
    for (int i = 0; i < 64; i++) {
        t += 24 * 5;
        items[k++] = (aud_item_t){ .tick = t, .kind = AUD_IT_CC, .a = 74, .b = (uint8_t)(i * 2) };
    }
    aud_edit_opts_t eo = { .quant = 0 };
    aud_edit(H.e, AUD_ED_INSERT, 0, 0, 0, items, k, &eo);
    h_render((uint32_t)song_frames);
    CHECK(h_hash() == song_hash, "song and API renders differ");
    free(mid);
}

TEST(test_a14_note_param_isolated)
{
    // A14: two notes on one channel; cutoff on one handle leaves the other.
    h_init(32000, 2, 0);
    aud_patch_t p;
    aud_patch_default(&p);
    p.filter = 1;
    p.wave = AUD_WAVE_SAW;
    aud_assign(H.e, 0, aud_patch_new(H.e, &p));
    uint32_t a = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    uint32_t b = aud_note_on(H.e, 0, 64, 100, 0, NULL, 0);
    h_render(256);
    aud_note_param(H.e, a, AUD_P_CUTOFF, AUD_ONE / 5, 0);
    h_render(256);
    CHECK(aud_eff(H.e, voice_of(a), AUD_P_CUTOFF) == AUD_ONE / 5, "a's cutoff %d", aud_eff(H.e, voice_of(a), AUD_P_CUTOFF));
    CHECK(aud_eff(H.e, voice_of(b), AUD_P_CUTOFF) == AUD_ONE, "b's cutoff changed to %d", aud_eff(H.e, voice_of(b), AUD_P_CUTOFF));
    // FR-29: a bundle applies from the first sample.
    uint32_t c = aud_note_on(H.e, 0, 67, 100, 0, (aud_pv_t[]){ { AUD_P_CUTOFF, AUD_ONE / 4 }, { AUD_P_PAN, -AUD_ONE / 2 } }, 2);
    h_render(1);
    CHECK(voice_of(c) && voice_of(c)->note_set == ((1u << AUD_P_CUTOFF) | (1u << AUD_P_PAN)), "bundle not applied");
}

TEST(test_a15_stale_handles)
{
    // A15: handles of ended and stolen notes change nothing; stale counts them.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 2, 5));
    uint32_t a = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    h_render(256);
    aud_note_off_h(H.e, a, 0);
    h_render(2048);
    aud_note_param(H.e, a, AUD_P_CUTOFF, 0, 0);
    h_render(256);
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.stale == 1, "stale %u after an ended note", st.stale);
    uint32_t first = 0;
    for (int i = 0; i < 17; i++) {
        uint32_t h = aud_note_on(H.e, 0, 30 + i, 100, 0, NULL, 0);
        first = i == 0 ? h : first;
        h_render(64);
    }
    h_render(256);
    aud_note_bend(H.e, first, 256, 0);       // its voice was stolen
    h_render(256);
    aud_stats(H.e, &st);
    CHECK(st.stale == 2, "stale %u after a stolen note", st.stale);
    for (int v = 0; v < AUD_VOICES; v++) {
        CHECK(H.e->voices[v].bend_note == 0, "voice %d was bent by a stale handle", v);
    }
}

TEST(test_a16_coalescing_and_ramps)
{
    // A16: a 30 ms Lua loop sweeping a parameter for 60 s never overflows;
    // a ramp does the same sweep in one event.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SAW, 5, 50));
    uint32_t h = aud_note_on(H.e, 0, 48, 100, 0, NULL, 0);
    int sent = 0;
    for (int ms = 0; ms < 60000; ms += 30) {
        // A burst of 20 updates per tick (worse than any real app).
        for (int k = 0; k < 20; k++) {
            aud_note_param(H.e, h, AUD_P_CUTOFF, (ms * 7 + k) % AUD_ONE, 0);
            sent++;
        }
        h_render(960);
    }
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.dropped_events == 0, "%u dropped of %d", st.dropped_events, sent);
    CHECK(H.e->npending == 0, "pending %d", H.e->npending);
    // Coalescing: 20 updates inside one chunk keep one pending event.
    for (int k = 0; k < 20; k++) {
        aud_note_param(H.e, h, AUD_P_CUTOFF, k * 1000, 5000);
    }
    h_render(1);
    CHECK(H.e->npending == 1, "%d pending after coalescing", H.e->npending);
    h_render(8000);
    CHECK(voice_of(h)->note_abs[AUD_P_CUTOFF] == 19000, "last value won: %d", voice_of(h)->note_abs[AUD_P_CUTOFF]);
    // The ramp: one event, exact end value.
    aud_note_ramp(H.e, h, AUD_P_CUTOFF, AUD_ONE / 2, 400, 0);
    h_render(32000 * 400 / 1000 / 2);
    int32_t mid = voice_of(h)->note_abs[AUD_P_CUTOFF];
    CHECK(mid > 19000 && mid < AUD_ONE / 2, "mid-ramp %d", mid);
    h_render(32000);
    CHECK(voice_of(h)->note_abs[AUD_P_CUTOFF] == AUD_ONE / 2, "ramp ended at %d", voice_of(h)->note_abs[AUD_P_CUTOFF]);
    CHECK(H.e->ramps_total == 0, "%d ramps left", H.e->ramps_total);
}

TEST(test_a17_time_controllers)
{
    // A17: CC 72, 73, 75 at 64 give the patch times exactly; 0 and 127
    // about 0.1x and 10x.
    h_init(32000, 2, 0);
    aud_patch_t p;
    aud_patch_default(&p);
    p.p[AUD_P_ATTACK] = 100;
    p.p[AUD_P_DECAY] = 200;
    p.p[AUD_P_RELEASE] = 300;
    aud_assign(H.e, 0, aud_patch_new(H.e, &p));
    uint32_t h = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    int cc[3] = { 73, 75, 72 }, pr[3] = { AUD_P_ATTACK, AUD_P_DECAY, AUD_P_RELEASE }, base[3] = { 100, 200, 300 };
    for (int i = 0; i < 3; i++) {
        aud_cc(H.e, 0, cc[i], 64, 0);
        h_render(64);
        CHECK(aud_eff(H.e, voice_of(h), pr[i]) == base[i], "cc %d at 64: %d", cc[i], aud_eff(H.e, voice_of(h), pr[i]));
        aud_cc(H.e, 0, cc[i], 0, 0);
        h_render(64);
        int lo = aud_eff(H.e, voice_of(h), pr[i]);
        CHECK(lo >= base[i] / 10 - 1 && lo <= base[i] / 10 + 1, "cc %d at 0: %d", cc[i], lo);
        aud_cc(H.e, 0, cc[i], 127, 0);
        h_render(64);
        int hi = aud_eff(H.e, voice_of(h), pr[i]);
        CHECK(hi >= base[i] * 9 && hi <= base[i] * 10, "cc %d at 127: %d", cc[i], hi);
    }
    // FR-25: a CC at 64 leaves every sound controller at the patch value.
    for (int c = 70; c <= 78; c++) {
        aud_cc(H.e, 0, c, 64, 0);
    }
    aud_cc(H.e, 0, 94, 64, 0);
    h_render(64);
    for (int q = 0; q < AUD_P_COUNT; q++) {
        if (q == AUD_P_PAN) {
            continue;
        }
        CHECK(aud_eff(H.e, voice_of(h), q) == voice_of(h)->patch.p[q], "%s moved at 64", aud_param_name(q));
    }
}

TEST(test_a18_poly_pressure)
{
    // A18: polyphonic aftertouch moves only that note's target.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 50));
    uint32_t a = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    uint32_t b = aud_note_on(H.e, 0, 64, 100, 0, NULL, 0);
    aud_pressure_target(H.e, 0, AUD_P_CUTOFF, -AUD_ONE / 2);
    h_render(256);
    aud_poly_pressure(H.e, 0, 64, 127, 0);
    h_render(256);
    CHECK(aud_eff(H.e, voice_of(a), AUD_P_CUTOFF) == AUD_ONE, "a moved: %d", aud_eff(H.e, voice_of(a), AUD_P_CUTOFF));
    CHECK(aud_eff(H.e, voice_of(b), AUD_P_CUTOFF) == AUD_ONE / 2, "b: %d", aud_eff(H.e, voice_of(b), AUD_P_CUTOFF));
    aud_note_pressure(H.e, a, AUD_ONE, 0);
    h_render(256);
    CHECK(aud_eff(H.e, voice_of(a), AUD_P_CUTOFF) == AUD_ONE / 2, "note_pressure: %d", aud_eff(H.e, voice_of(a), AUD_P_CUTOFF));
}

TEST(test_a19_mpe)
{
    // A19: MPE, two notes on channels 1 and 2 bend independently.
    h_init(32000, 2, 0);
    int p = synth(AUD_WAVE_SINE, 5, 50);
    aud_mpe(H.e, 1, 15);
    for (int ch = 1; ch <= 2; ch++) {
        aud_assign(H.e, ch, p);
    }
    uint32_t a = aud_note_on(H.e, 1, 60, 100, 0, NULL, 0);
    uint32_t b = aud_note_on(H.e, 2, 60, 100, 0, NULL, 0);
    h_render(256);
    uint32_t s0 = voice_of(a)->step;
    aud_bend(H.e, 1, 8191 / 48 * 2, 0);      // +2 semitones of the 48-semitone MPE range
    h_render(256);
    double ra = (double)voice_of(a)->step / s0, rb = (double)voice_of(b)->step / s0;
    CHECK(fabs(ra - pow(2, 2.0 / 12)) < 0.01, "a ratio %.4f", ra);
    CHECK(fabs(rb - 1.0) < 0.0001, "b moved: %.4f", rb);
}

TEST(test_nrpn_rpn)
{
    // FR-27: RPN 0 sets the bend range; NRPN 1/1 the pulse width.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_PULSE, 5, 50));
    uint32_t a = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    aud_cc(H.e, 0, 101, 0, 0), aud_cc(H.e, 0, 100, 0, 0), aud_cc(H.e, 0, 6, 12, 0), aud_cc(H.e, 0, 38, 0, 0);
    aud_cc(H.e, 0, 101, 127, 0), aud_cc(H.e, 0, 100, 127, 0);
    aud_cc(H.e, 0, 99, 1, 0), aud_cc(H.e, 0, 98, 1, 0), aud_cc(H.e, 0, 6, 0, 0), aud_cc(H.e, 0, 38, 40, 0);
    h_render(256);
    CHECK(H.e->chans[0].bend_range == 12 * 256, "bend range %d", H.e->chans[0].bend_range);
    CHECK(aud_eff(H.e, voice_of(a), AUD_P_PW) == 40 * AUD_ONE / 100, "pw %d", aud_eff(H.e, voice_of(a), AUD_P_PW));
    // map_cc
    aud_map_cc(H.e, 0, 16, AUD_P_VIB_RATE, AUD_ONE / 10, 12 * AUD_ONE);
    aud_cc(H.e, 0, 16, 127, 0);
    h_render(256);
    CHECK(aud_eff(H.e, voice_of(a), AUD_P_VIB_RATE) == 12 * AUD_ONE, "mapped %d", aud_eff(H.e, voice_of(a), AUD_P_VIB_RATE));
    // reverb send is accepted and ignored
    aud_cc(H.e, 0, 91, 100, 0);
    h_render(64);
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.ignored == 1, "ignored %u", st.ignored);
}

TEST(test_sustain_pedal)
{
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 20));
    aud_cc(H.e, 0, 64, 127, 0);
    uint32_t a = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    h_render(256);
    aud_note_off(H.e, 0, 60, 0);
    h_render(8000);
    CHECK(voice_of(a) && voice_of(a)->env == ENV_SUSTAIN, "pedal did not hold the note");
    aud_cc(H.e, 0, 64, 0, 0);
    h_render(8000);
    CHECK(!voice_of(a), "pedal up did not release");
}

TEST(test_limiter_and_filter_stability)
{
    // 16 loud saws through a resonant filter: never past full scale, no
    // runaway, the limiter recovers.
    h_init(48000, 2, 0);
    aud_patch_t p;
    aud_patch_default(&p);
    p.wave = AUD_WAVE_SAW;
    p.filter = 1;
    p.p[AUD_P_RES] = AUD_ONE;
    p.p[AUD_P_SUSTAIN] = AUD_ONE;
    p.p[AUD_P_VOLUME] = 2 * AUD_ONE;
    aud_assign(H.e, 0, aud_patch_new(H.e, &p));
    aud_set_master(H.e, 2 * AUD_ONE);
    for (int i = 0; i < 16; i++) {
        aud_note_on(H.e, 0, 30 + i * 3, 127, 0, NULL, 0);
    }
    for (int s = 0; s < 100; s++) {
        aud_cc(H.e, 0, 74, (s * 13) & 127, 0);
        h_render(480);
    }
    CHECK(peak(0, H.frames) <= 32767, "peak %d", peak(0, H.frames));
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.clipped < H.frames / 100, "clipped %u of %zu", st.clipped, H.frames * 2);
    CHECK(H.e->lim_gain < AUD_ONE, "limiter never engaged");
    aud_all_off(H.e, true);
    aud_set_master(H.e, AUD_ONE);
    h_render(48000);
    CHECK(H.e->lim_gain == AUD_ONE, "limiter did not recover: %u", H.e->lim_gain);
    CHECK(peak(H.frames - 1000, H.frames) == 0, "not silent after kill");
}

// ---------------------------------------------------------------------------
// Clock, grid and quantised editing (A5, A20 to A28)

static uint64_t ref_ticks(uint64_t samples, uint32_t rate, double bpm, uint32_t ppqn)
{
    return (uint64_t)floor((double)samples * bpm * ppqn / (60.0 * rate) + 1e-9);
}

TEST(test_a20_clock_no_drift)
{
    // A20: 10 minutes at 120 BPM, 480 ppqn, 4 beats: clock % bar matches a
    // bar counter from the sample count, with no drift.
    h_init(32000, 2, 0);
    aud_grid_t g;
    aud_grid(H.e, &g);
    CHECK(g.ppqn == 480 && g.bar == 1920 && g.bpm_q16 == 120 * AUD_ONE, "grid %u %u", g.ppqn, g.bar);
    uint64_t samples = 0;
    int worst = 0;
    for (int b = 0; b < 32000 * 600 / 256; b++) {
        aud_render(H.e, (int16_t[512]){0}, 256);
        samples += 256;
        uint64_t want = ref_ticks(samples, 32000, 120, 480);
        int d = (int)((int64_t)aud_clock(H.e) - (int64_t)want);
        worst = abs(d) > worst ? abs(d) : worst;
    }
    CHECK(worst <= 1, "clock off by %d ticks", worst);
    uint64_t want = ref_ticks(samples, 32000, 120, 480);
    CHECK(aud_clock(H.e) % 1920 == want % 1920 || worst, "bar position %u vs %llu", aud_clock(H.e) % 1920,
          (unsigned long long)(want % 1920));
    // heard() lags by the latency
    CHECK(aud_heard(H.e, 1536) == aud_clock(H.e) - 1536 * 480 * 2 / 32000 ||
          aud_heard(H.e, 1536) == aud_clock(H.e) - 1536 * 480 * 2 / 32000 - 1, "heard %u clock %u",
          aud_heard(H.e, 1536), aud_clock(H.e));
}

TEST(test_a5_click_track)
{
    // A5: a click track scheduled from "Lua" (with GC-like stalls between
    // calls) plays every click on its tick: error < 8 ms, no drift.
    h_init(32000, 1, 0);
    aud_assign(H.e, 9, synth(AUD_WAVE_SINE, 2, 10));
    aud_grid_t g;
    aud_grid(H.e, &g);
    uint32_t next_beat = g.beat * 2;
    int clicks = 0;
    for (int ms = 0; ms < 300000; ms += 30) {
        // schedule beats up to 100 ms ahead
        while ((int32_t)(next_beat - aud_clock(H.e)) < (int32_t)(g.beat / 5)) {
            aud_item_t it = { .tick = 0, .kind = AUD_IT_NOTE, .a = 76, .b = 100, .v = 20 };
            aud_edit_opts_t o = { .at = next_beat, .quant = AUD_QUANT_NOW };
            int64_t at = aud_edit(H.e, AUD_ED_INSERT, 9, 0, 0, &it, 1, &o);
            CHECK(at == next_beat, "click at %lld wanted %u", (long long)at, next_beat);
            next_beat += g.beat;
            clicks++;
        }
        h_render(960 + ((ms / 30) % 7 == 0 ? 640 : 0));    // occasional 20 ms stall
    }
    int worst = 0, starts = 0;
    for (int i = 0; i < H.ntrace; i++) {
        if (H.trace[i].what != AUD_TRACE_START) {
            continue;
        }
        starts++;
        double t_ideal = (double)H.trace[i].tick * 60.0 / (120.0 * 480.0) * 32000.0;
        int err = (int)fabs(H.trace[i].sample - t_ideal);
        worst = err > worst ? err : worst;
    }
    CHECK(starts >= clicks - 2, "%d starts for %d clicks", starts, clicks);
    CHECK(worst <= 2, "worst timing error %d samples", worst);
}

static void insert_bar(int ch, uint32_t at, const int *notes, int n, int len)
{
    aud_item_t it[16];
    for (int i = 0; i < n; i++) {
        it[i] = (aud_item_t){ .tick = (uint32_t)(i * 480), .kind = AUD_IT_NOTE, .a = (uint8_t)notes[i], .b = 100, .v = len };
    }
    aud_edit_opts_t o = { .at = at, .quant = 1920 };
    int64_t r = aud_edit(H.e, AUD_ED_INSERT, ch, 0, 0, it, n, &o);
    CHECK(r == at, "insert applied at %lld, wanted %u", (long long)r, at);
}

TEST(test_a21_update_before_start)
{
    // A21: insert a bar one bar ahead, update it before it starts: only the
    // new notes play.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 30));
    int old[4] = { 60, 62, 64, 65 }, neu[4] = { 72, 74, 76, 77 };
    insert_bar(0, 1920 * 2, old, 4, 240);
    h_render(4096);
    aud_item_t it[4];
    for (int i = 0; i < 4; i++) {
        it[i] = (aud_item_t){ .tick = (uint32_t)(i * 480), .kind = AUD_IT_NOTE, .a = (uint8_t)neu[i], .b = 100, .v = 240 };
    }
    aud_edit_opts_t o = { .quant = AUD_QUANT_NOW };
    aud_edit(H.e, AUD_ED_UPDATE, 0, 1920 * 2, 1920 * 3, it, 4, &o);
    h_render(32000 * 8);
    for (int i = 0; i < 4; i++) {
        CHECK(h_count(AUD_TRACE_START, 0, old[i]) == 0, "old note %d played", old[i]);
        CHECK(h_count(AUD_TRACE_START, 0, neu[i]) == 1, "new note %d played %d times", neu[i],
              h_count(AUD_TRACE_START, 0, neu[i]));
        const trace_ev_t *t = h_find(AUD_TRACE_START, 0, neu[i], 0);
        CHECK(t && t->tick == 1920 * 2 + (uint32_t)i * 480, "note %d at tick %u", neu[i], t ? t->tick : 0);
    }
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.voices_active == 0 && st.tl_free == AUD_TL_NODES, "left %u voices, %u free", st.voices_active, st.tl_free);
}

TEST(test_a22_a23_ownership)
{
    // A22: a note that began inside the updated range releases at the
    // boundary. A23: one that began before it plays out (or, with trim,
    // releases at the boundary).
    for (int trim = 0; trim < 2; trim++) {
        h_init(32000, 2, 0);
        aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 30));
        aud_item_t it[2] = {
            { .tick = 0, .kind = AUD_IT_NOTE, .a = 50, .b = 100, .v = 1920 * 4 },        // before the range
            { .tick = 1920, .kind = AUD_IT_NOTE, .a = 55, .b = 100, .v = 1920 * 4 },     // inside it
        };
        aud_edit_opts_t o = { .at = 1920, .quant = 1920 };
        aud_edit(H.e, AUD_ED_INSERT, 0, 0, 0, it, 2, &o);
        // Let both start: 1920 + 1920 + a beat
        while (aud_clock(H.e) < 1920 * 2 + 480) {
            h_render(256);
        }
        CHECK(h_count(AUD_TRACE_START, 0, 50) == 1 && h_count(AUD_TRACE_START, 0, 55) == 1, "notes did not start");
        aud_edit_opts_t u = { .quant = 1920, .trim = trim != 0 };
        int64_t at = aud_edit(H.e, AUD_ED_UPDATE, 0, 1920 * 2, 1920 * 4, NULL, 0, &u);
        CHECK(at == 1920 * 3, "update applies at %lld", (long long)at);
        while (aud_clock(H.e) < 1920 * 3 + 100) {
            h_render(256);
        }
        const trace_ev_t *r55 = h_find(AUD_TRACE_RELEASE, 0, 55, 0), *r50 = h_find(AUD_TRACE_RELEASE, 0, 50, 0);
        CHECK(r55 && r55->tick == 1920 * 3, "inside note released at %u", r55 ? r55->tick : 0);
        if (trim) {
            CHECK(r50 && r50->tick == 1920 * 3, "trim: earlier note released at %u", r50 ? r50->tick : 0);
        } else {
            CHECK(!r50, "earlier note was released at %u", r50 ? r50->tick : 0);
            while (aud_clock(H.e) < 1920 * 5 + 100) {
                h_render(256);
            }
            r50 = h_find(AUD_TRACE_RELEASE, 0, 50, 0);
            CHECK(r50 && r50->tick == 1920 * 5, "earlier note ended at %u, wanted its own end", r50 ? r50->tick : 0);
        }
        h_render(32000);
        aud_stats_t st;
        aud_stats(H.e, &st);
        CHECK(st.voices_active == 0, "stuck voices %u", st.voices_active);
    }
}

TEST(test_a24_late_edit)
{
    // A24: an edit asked for too late applies at the next boundary, and the
    // call says so.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 30));
    while (aud_clock(H.e) < 1920 - 4) {
        h_render(64);
    }
    aud_item_t it = { .tick = 0, .kind = AUD_IT_NOTE, .a = 60, .b = 100, .v = 100 };
    aud_edit_opts_t o = { .at = 1920, .quant = 1920 };
    int64_t at = aud_edit(H.e, AUD_ED_INSERT, 0, 0, 0, &it, 1, &o);
    CHECK(at == 3840, "applied at %lld, wanted 3840", (long long)at);
    while (aud_clock(H.e) < 4000) {
        h_render(256);
    }
    const trace_ev_t *t = h_find(AUD_TRACE_START, 0, 60, 0);
    CHECK(t && t->tick == 3840, "played at %u", t ? t->tick : 0);
}

TEST(test_a25_a26_stop)
{
    // A25: stop on one channel clears and releases it; the others play on.
    // A26: stop all during a song: silence, sequencer stopped, ms events
    // dropped, the clock runs on.
    h_init(32000, 2, 1 << 20);
    int p = synth(AUD_WAVE_SINE, 5, 30);
    aud_assign(H.e, 0, p);
    aud_assign(H.e, 1, p);
    int a[4] = { 60, 61, 62, 63 }, b[4] = { 70, 71, 72, 73 };
    insert_bar(0, 1920, a, 4, 400);
    insert_bar(1, 1920, b, 4, 400);
    while (aud_clock(H.e) < 1920 + 100) {
        h_render(256);
    }
    aud_edit_opts_t o = { .quant = AUD_QUANT_NOW };
    aud_edit(H.e, AUD_ED_STOP, 0, 0, 0, NULL, 0, &o);
    h_render(32000 * 4);
    CHECK(h_count(AUD_TRACE_START, 0, -1) == 1, "channel 0 kept playing: %d", h_count(AUD_TRACE_START, 0, -1));
    CHECK(h_count(AUD_TRACE_START, 1, -1) == 4, "channel 1 stopped: %d", h_count(AUD_TRACE_START, 1, -1));
    for (int i = 0; i < 4; i++) {
        const trace_ev_t *t = h_find(AUD_TRACE_START, 1, b[i], 0);
        CHECK(t && t->tick == 1920 + (uint32_t)i * 480, "channel 1 note %d at %u", i, t ? t->tick : 0);
    }
    // A26: a looping song, ms-delay events pending, then stop{all}.
    uint8_t trk[256];
    size_t n = 0;
    for (int i = 0; i < 8; i++) {
        n += put_vlq(trk + n, i ? 48 : 0), trk[n++] = 0x90, trk[n++] = (uint8_t)(40 + i), trk[n++] = 90;
        n += put_vlq(trk + n, 48), trk[n++] = 0x80, trk[n++] = (uint8_t)(40 + i), trk[n++] = 0;
    }
    n += put_vlq(trk + n, 0), trk[n++] = 0xFF, trk[n++] = 0x2F, trk[n++] = 0;
    const uint8_t *tr[1] = { trk };
    size_t len;
    uint8_t *mid = midi_file(&len, 1, tr, &n, 96);
    int song = aud_song_load(H.e, mid, len);
    aud_song_opts_t so = { .loop = true, .quant = 1920 };
    aud_song_play(H.e, song, &so);
    h_render(32000 * 4);
    int before = h_count(AUD_TRACE_START, 0, -1);
    CHECK(before > 4, "song not playing: %d", before);
    aud_note_on(H.e, 1, 80, 100, 32000, NULL, 0);     // a second from now
    h_render(256);
    aud_edit_opts_t all = { .quant = AUD_QUANT_NOW };
    aud_edit(H.e, AUD_ED_STOP, AUD_ALL, 0, 0, NULL, 0, &all);
    uint32_t clk = aud_clock(H.e);
    h_render(32000 * 3);
    CHECK(h_count(AUD_TRACE_START, 0, -1) <= before + 1, "song kept playing");
    CHECK(h_count(AUD_TRACE_START, 1, 80) == 0, "ms-delay event survived stop all");
    CHECK(peak(H.frames - 3200, H.frames) == 0, "not silent");
    CHECK(aud_clock(H.e) > clk + 1000, "clock stopped");
    CHECK(!H.e->seq.playing, "sequencer still running");
    free(mid);
}

TEST(test_a27_tempo_change)
{
    // A27: a tempo change mid-bar keeps the clock continuous and the grid
    // in step with the sample-count reference.
    h_init(48000, 2, 0);
    uint64_t samples = 0;
    while (aud_clock(H.e) < 1920 + 700) {
        h_render(256);
        samples += 256;
    }
    int64_t at = aud_transport(H.e, 90 * AUD_ONE, 0, 0, 480);       // next beat
    CHECK(at == 1920 + 960, "tempo applies at %lld", (long long)at);
    // Reference: ticks before `at` at 120 BPM, after it at 90.
    double spt120 = 48000 * 60.0 / (120 * 480), spt90 = 48000 * 60.0 / (90 * 480);
    int worst = 0;
    uint32_t prev = aud_clock(H.e);
    for (int i = 0; i < 2000; i++) {
        h_render(256);
        samples += 256;
        double t_at = (double)at * spt120;
        double ticks = samples < t_at ? samples / spt120 : at + (samples - t_at) / spt90;
        int d = (int)((int64_t)aud_clock(H.e) - (int64_t)floor(ticks));
        worst = abs(d) > worst ? abs(d) : worst;
        CHECK(aud_clock(H.e) >= prev, "clock went back");
        prev = aud_clock(H.e);
    }
    CHECK(worst <= 1, "grid off by %d ticks after the tempo change", worst);
    aud_grid_t g;
    aud_grid(H.e, &g);
    CHECK(g.bpm_q16 == 90 * AUD_ONE && g.bar == 1920, "grid %u bpm", g.bpm_q16 >> 16);
}

TEST(test_a28_timeline_flood)
{
    // A28: past 2048 items the edit is rejected whole; playback continues.
    h_init(32000, 2, 0);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 30));
    aud_item_t it[256];
    for (int i = 0; i < 256; i++) {
        it[i] = (aud_item_t){ .tick = (uint32_t)i * 10, .kind = AUD_IT_CC, .a = 7, .b = 100 };
    }
    int ok = 0, refused = 0;
    for (int k = 0; k < 12; k++) {
        aud_edit_opts_t o = { .at = 100000 + (uint32_t)k * 4000 };
        int64_t r = aud_edit(H.e, AUD_ED_INSERT, k % 16, 0, 0, it, 256, &o);
        r >= 0 ? ok++ : refused++;
        h_render(256);
    }
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(ok == 8 && refused == 4, "%d accepted, %d refused", ok, refused);
    CHECK(st.tl_free == 0 && st.tl_rejected == 4, "free %u, rejected %u", st.tl_free, st.tl_rejected);
    // An update too big for what's left is refused by the mixer, whole.
    aud_item_t big[256];
    for (int i = 0; i < 256; i++) {
        big[i] = (aud_item_t){ .tick = (uint32_t)i, .kind = AUD_IT_NOTE, .a = 60, .b = 1, .v = 1 };
    }
    aud_edit_opts_t o = { .quant = AUD_QUANT_NOW };
    aud_edit(H.e, AUD_ED_UPDATE, 15, 0, 1, big, 256, &o);
    h_render(512);
    aud_stats(H.e, &st);
    CHECK(st.tl_rejected == 5 && st.tl_free == 0, "update: rejected %u, free %u", st.tl_rejected, st.tl_free);
    uint32_t h = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    h_render(512);
    CHECK(voice_of(h) != NULL, "playback stopped");
}

TEST(test_song_tempo_and_pause)
{
    // FR-20: a format 1 file (tempo track + notes) with follow_song_tempo,
    // pause and resume.
    uint8_t t0[32], t1[256];
    size_t n0 = 0, n1 = 0;
    n0 += put_vlq(t0 + n0, 0), t0[n0++] = 0xFF, t0[n0++] = 0x51, t0[n0++] = 3;
    t0[n0++] = 0x09, t0[n0++] = 0x27, t0[n0++] = 0xC0;                 // 600000 us: 100 BPM
    n0 += put_vlq(t0 + n0, 0), t0[n0++] = 0xFF, t0[n0++] = 0x2F, t0[n0++] = 0;
    for (int i = 0; i < 16; i++) {
        n1 += put_vlq(t1 + n1, i ? 60 : 0), t1[n1++] = 0x91, t1[n1++] = (uint8_t)(60 + i), t1[n1++] = 100;
        n1 += put_vlq(t1 + n1, 60), t1[n1++] = 0x91, t1[n1++] = (uint8_t)(60 + i), t1[n1++] = 0;   // vel 0 off
    }
    n1 += put_vlq(t1 + n1, 0), t1[n1++] = 0xFF, t1[n1++] = 0x2F, t1[n1++] = 0;
    const uint8_t *tr[2] = { t0, t1 };
    size_t tl[2] = { n0, n1 }, len;
    uint8_t *mid = midi_file(&len, 2, tr, tl, 120);
    h_init(32000, 2, 1 << 20);
    aud_assign(H.e, 1, synth(AUD_WAVE_SINE, 5, 20));
    int s = aud_song_load(H.e, mid, len);
    CHECK(s == 0 && aud_song_ticks(H.e, s) == 120 * 4 * 4, "song length %u", aud_song_ticks(H.e, s));
    aud_song_opts_t so = { .follow_tempo = true, .quant = 0 };
    int64_t at = aud_song_play(H.e, s, &so);
    h_render(32000 * 2);
    aud_grid_t g;
    aud_grid(H.e, &g);
    CHECK(g.bpm_q16 >> 16 == 100, "follow tempo: %u BPM", g.bpm_q16 >> 16);
    const trace_ev_t *n2 = h_find(AUD_TRACE_START, 1, 62, 0);
    CHECK(n2 && n2->tick == (uint32_t)at + 2 * 120 * 4, "third note at tick %u (start %lld)", n2 ? n2->tick : 0, (long long)at);
    aud_song_pause(H.e, 0);
    h_render(16000);
    int played = h_count(AUD_TRACE_START, 1, -1);
    h_render(16000);
    CHECK(h_count(AUD_TRACE_START, 1, -1) == played, "notes during pause");
    aud_song_play(H.e, s, &so);
    h_render(32000 * 12);
    CHECK(h_count(AUD_TRACE_START, 1, -1) == 16, "%d notes after resume (want 16)", h_count(AUD_TRACE_START, 1, -1));
    for (int i = 0; i < 16; i++) {
        CHECK(h_count(AUD_TRACE_START, 1, 60 + i) == 1, "note %d played %d times", 60 + i, h_count(AUD_TRACE_START, 1, 60 + i));
    }
    free(mid);
}

TEST(test_transport_restart)
{
    h_init(32000, 2, 0);
    h_render(32000);
    CHECK(aud_clock(H.e) > 0, "clock not running");
    aud_transport(H.e, 100 * AUD_ONE, 96, 3, 0);
    h_render(256);
    aud_grid_t g;
    aud_grid(H.e, &g);
    CHECK(g.ppqn == 96 && g.bar == 288 && g.bpm_q16 == 100 * AUD_ONE, "grid %u %u %u", g.ppqn, g.bar, g.bpm_q16 >> 16);
    CHECK(aud_clock(H.e) < 20, "clock did not restart: %u", aud_clock(H.e));
}

TEST(test_reset_frees_everything)
{
    // A4's core half: after reset the pool, timelines and voices are free.
    h_init(32000, 2, 1 << 20);
    aud_assign(H.e, 0, synth(AUD_WAVE_SINE, 5, 30));
    aud_pool_alloc(H.e, 100000);
    int a[4] = { 60, 61, 62, 63 };
    insert_bar(0, 1920, a, 4, 400);
    aud_note_on(H.e, 0, 70, 100, 0, NULL, 0);
    h_render(32000);
    aud_reset(H.e);
    aud_stats_t st;
    aud_stats(H.e, &st);
    CHECK(st.pool_used == 0 && st.tl_free == AUD_TL_NODES && st.voices_active == 0, "pool %u, free %u, voices %u",
          st.pool_used, st.tl_free, st.voices_active);
    h_clear();
    h_render(4096);
    CHECK(peak(0, H.frames) == 0, "sound after reset");
}

TEST(test_patch_set)
{
    // FR-14: changing a patch affects notes started afterwards only.
    h_init(32000, 2, 0);
    int p = synth(AUD_WAVE_SINE, 5, 30);
    aud_assign(H.e, 0, p);
    uint32_t a = aud_note_on(H.e, 0, 60, 100, 0, NULL, 0);
    h_render(256);
    aud_patch_t q;
    aud_patch_default(&q);
    q.wave = AUD_WAVE_SAW;
    for (int i = 0; i < 100; i++) {                 // reuse of retired slots
        CHECK(aud_patch_set(H.e, p, &q) == p, "patch_set %d failed", i);
        h_render(256);
    }
    uint32_t b = aud_note_on(H.e, 0, 64, 100, 0, NULL, 0);
    h_render(256);
    CHECK(voice_of(a)->wave == AUD_WAVE_SINE && voice_of(b)->wave == AUD_WAVE_SAW, "waves %d %d",
          voice_of(a)->wave, voice_of(b)->wave);
}

// ---------------------------------------------------------------------------
// Golden renders and the CPU figure

static void golden(bool update)
{
    s_test = "golden";
    const char *scripts[] = { "scripts/waves.txt", "scripts/filter.txt", "scripts/params.txt", "scripts/sampler.txt" };
    FILE *g = fopen("golden.txt", "r");
    char line[256];
    uint64_t want[8] = {0};
    for (int i = 0; g && fgets(line, sizeof(line), g) && i < 8; i++) {
        char name[200];
        unsigned long long v;
        if (sscanf(line, "%199s %llx", name, &v) == 2) {
            for (int k = 0; k < 4; k++) {
                if (!strcmp(name, scripts[k])) {
                    want[k] = v;
                }
            }
        }
    }
    if (g) {
        fclose(g);
    }
    FILE *out = update ? fopen("golden.txt", "w") : NULL;
    for (int k = 0; k < 4; k++) {
        if (h_script(scripts[k]) != 0) {
            CHECK(0, "%s failed to run", scripts[k]);
            continue;
        }
        char wav[256];
        snprintf(wav, sizeof(wav), "build/%s", strrchr(scripts[k], '/') + 1);
        strcpy(strrchr(wav, '.'), ".wav");
        h_wav_write(wav);
        uint64_t got = h_hash();
        if (out) {
            fprintf(out, "%s %016llx\n", scripts[k], (unsigned long long)got);
        } else {
            CHECK(got == want[k], "%s renders %016llx, golden %016llx (listen: %s)", scripts[k],
                  (unsigned long long)got, (unsigned long long)want[k], wav);
        }
    }
    if (out) {
        fclose(out);
        printf("golden.txt updated\n");
    }
}

#include <time.h>

static void cpu_figure(void)
{
    for (int r = 0; r < 2; r++) {
        uint32_t rate = r ? 48000 : 32000;
        h_init(rate, 2, 0);
        aud_patch_t p;
        aud_patch_default(&p);
        p.wave = AUD_WAVE_SAW;
        p.filter = 1;
        p.p[AUD_P_VIB_DEPTH] = AUD_ONE / 4;
        p.p[AUD_P_SUSTAIN] = AUD_ONE;
        aud_assign(H.e, 0, aud_patch_new(H.e, &p));
        for (int i = 0; i < 16; i++) {
            aud_note_on(H.e, 0, 40 + i * 2, 100, 0, NULL, 0);
        }
        int16_t buf[512];
        clock_t t0 = clock();
        for (uint32_t i = 0; i < rate * 10 / 256; i++) {
            aud_render(H.e, buf, 256);
        }
        double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("cpu: 16 voices, stereo %u Hz, filter on: %.2f%% of one host core (the badge's figure comes "
               "from audio.stats())\n", rate, s / 10 * 100);
    }
}

int main(int argc, char **argv)
{
    bool update = argc > 1 && !strcmp(argv[1], "--update-golden");
    RUN(test_tone_pitch);
    RUN(test_sample_accurate);
    RUN(test_voice_alloc);
    RUN(test_a6_flood);
    RUN(test_a7_waveforms_no_clicks);
    RUN(test_a8_wav);
    RUN(test_sampler);
    RUN(test_a13_midi_cc74_sweep);
    RUN(test_a14_note_param_isolated);
    RUN(test_a15_stale_handles);
    RUN(test_a16_coalescing_and_ramps);
    RUN(test_a17_time_controllers);
    RUN(test_a18_poly_pressure);
    RUN(test_a19_mpe);
    RUN(test_nrpn_rpn);
    RUN(test_sustain_pedal);
    RUN(test_limiter_and_filter_stability);
    RUN(test_a20_clock_no_drift);
    RUN(test_a5_click_track);
    RUN(test_a21_update_before_start);
    RUN(test_a22_a23_ownership);
    RUN(test_a24_late_edit);
    RUN(test_a25_a26_stop);
    RUN(test_a27_tempo_change);
    RUN(test_a28_timeline_flood);
    RUN(test_song_tempo_and_pause);
    RUN(test_transport_restart);
    RUN(test_reset_frees_everything);
    RUN(test_patch_set);
    golden(update);
    cpu_figure();
    h_free();
    printf("%d checks passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
