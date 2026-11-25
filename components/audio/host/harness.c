// Host harness and the event-script interpreter (see render.c for the format).

#include <ctype.h>
#include "harness.h"

harness_t H;

static void trace_fn(void *ud, int what, int ch, int note, uint32_t serial, uint32_t sample, uint32_t tick)
{
    (void)ud;
    if (H.ntrace < (int)(sizeof(H.trace) / sizeof(H.trace[0]))) {
        H.trace[H.ntrace++] = (trace_ev_t){ what, ch, note, serial, sample, tick };
    }
}

void h_init(uint32_t rate, int channels, size_t pool)
{
    h_free();
    H.e = calloc(1, aud_engine_size());
    H.cold = calloc(1, aud_cold_size());
    H.pool = pool ? calloc(1, pool) : NULL;
    aud_init(H.e, H.cold, H.pool, pool, rate, channels);
    aud_set_trace(H.e, trace_fn, NULL);
    H.block = 256;
    h_clear();
}

void h_free(void)
{
    free(H.e);
    free(H.cold);
    free(H.pool);
    free(H.pcm);
    H.e = NULL, H.cold = H.pool = NULL, H.pcm = NULL;
    H.cap = H.frames = 0;
}

void h_clear(void)
{
    H.frames = 0;
    H.ntrace = 0;
}

void h_render(uint32_t frames)
{
    int nch = aud_channels(H.e);
    while (frames) {
        int n = frames < (uint32_t)H.block ? (int)frames : H.block;
        if (H.frames + (size_t)n > H.cap) {
            H.cap = (H.cap + (size_t)n) * 2;
            H.pcm = realloc(H.pcm, H.cap * nch * sizeof(int16_t));
        }
        aud_render(H.e, H.pcm + H.frames * nch, n);
        H.frames += (size_t)n;
        frames -= (uint32_t)n;
    }
}

void h_render_ms(uint32_t ms)
{
    h_render((uint32_t)((uint64_t)ms * aud_rate(H.e) / 1000));
}

static void put32(FILE *f, uint32_t v)
{
    fputc(v & 255, f), fputc(v >> 8 & 255, f), fputc(v >> 16 & 255, f), fputc(v >> 24, f);
}

static void put16(FILE *f, uint16_t v)
{
    fputc(v & 255, f), fputc(v >> 8, f);
}

int h_wav_write(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    int nch = aud_channels(H.e);
    uint32_t rate = aud_rate(H.e), bytes = (uint32_t)(H.frames * nch * 2);
    fwrite("RIFF", 1, 4, f), put32(f, 36 + bytes), fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16), put16(f, 1), put16(f, (uint16_t)nch), put32(f, rate), put32(f, rate * nch * 2);
    put16(f, (uint16_t)(nch * 2)), put16(f, 16), fwrite("data", 1, 4, f), put32(f, bytes);
    for (size_t i = 0; i < H.frames * nch; i++) {
        put16(f, (uint16_t)H.pcm[i]);
    }
    fclose(f);
    return 0;
}

uint64_t h_hash(void)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)H.pcm;
    for (size_t i = 0; i < H.frames * aud_channels(H.e) * 2; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

int h_count(int what, int ch, int note)
{
    int n = 0;
    for (int i = 0; i < H.ntrace; i++) {
        const trace_ev_t *t = &H.trace[i];
        n += (what < 0 || t->what == what) && (ch < 0 || t->ch == ch) && (note < 0 || t->note == note);
    }
    return n;
}

const trace_ev_t *h_find(int what, int ch, int note, int nth)
{
    for (int i = 0; i < H.ntrace; i++) {
        const trace_ev_t *t = &H.trace[i];
        if ((what < 0 || t->what == what) && (ch < 0 || t->ch == ch) && (note < 0 || t->note == note) && nth-- == 0) {
            return t;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Event scripts

static int32_t q16(const char *s)
{
    return (int32_t)(atof(s) * 65536.0 + (atof(s) >= 0 ? 0.5 : -0.5));
}

static const char *const s_waves[] = {"sine", "pulse", "saw", "triangle", "noise"};

static int kv(char *tok, char **k, char **v)
{
    char *eq = strchr(tok, '=');
    if (!eq) {
        return 0;
    }
    *eq = 0;
    *k = tok;
    *v = eq + 1;
    return 1;
}

static size_t file_read(void *ud, void *buf, size_t n)
{
    return fread(buf, 1, n, ud);
}

int h_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "%s: can't open\n", path);
        return -1;
    }
    char line[512], dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        slash[1] = 0;
    } else {
        dir[0] = 0;
    }
    int handles[32] = {0};
    uint32_t last = 0, now_ms = 0;
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        char *tok[32];
        int n = 0;
        for (char *t = strtok(line, " \t\r\n"); t && n < 32; t = strtok(NULL, " \t\r\n")) {
            tok[n++] = t;
        }
        if (!n) {
            continue;
        }
        const char *cmd = tok[0];
        aud_engine_t *e = H.e;
        if (!strcmp(cmd, "init")) {
            h_init((uint32_t)atoi(tok[1]), n > 2 ? atoi(tok[2]) : 2, 1 << 20);
        } else if (!strcmp(cmd, "synth")) {
            aud_patch_t p;
            aud_patch_default(&p);
            for (int i = 2; i < n; i++) {
                char *k, *v;
                if (!kv(tok[i], &k, &v)) {
                    continue;
                }
                if (!strcmp(k, "wave")) {
                    for (int w = 0; w < AUD_WAVE_COUNT; w++) {
                        if (!strcmp(v, s_waves[w])) {
                            p.wave = (uint8_t)w;
                        }
                    }
                } else if (!strcmp(k, "filter")) {
                    p.filter = (uint8_t)atoi(v);
                } else if (!strcmp(k, "key")) {
                    p.key_track = q16(v);
                } else if (!strcmp(k, "lfo_cutoff")) {
                    p.lfo_cutoff = q16(v);
                } else if (!strcmp(k, "targets")) {
                    p.lfo_targets = (uint8_t)atoi(v);
                } else {
                    int pi = aud_param_find(k);
                    if (pi < 0) {
                        fprintf(stderr, "%s:%d: unknown parameter %s\n", path, lineno, k);
                        fclose(f);
                        return -1;
                    }
                    p.p[pi] = aud_param_from_q16(pi, q16(v));
                }
            }
            handles[atoi(tok[1]) & 31] = aud_patch_new(e, &p);
        } else if (!strcmp(cmd, "bank")) {
            handles[atoi(tok[1]) & 31] = aud_bank_new(e);
        } else if (!strcmp(cmd, "sample")) {
            aud_sample_t s;
            aud_sample_default(&s);
            char file[300] = "";
            uint32_t ls = 0, le = 0;
            for (int i = 2; i < n; i++) {
                char *k, *v;
                if (!kv(tok[i], &k, &v)) {
                    continue;
                }
                if (!strcmp(k, "file")) snprintf(file, sizeof(file), "%s%s", dir, v);
                else if (!strcmp(k, "root")) s.root = (uint8_t)atoi(v);
                else if (!strcmp(k, "lo")) s.key_lo = (uint8_t)atoi(v);
                else if (!strcmp(k, "hi")) s.key_hi = (uint8_t)atoi(v);
                else if (!strcmp(k, "loop")) { ls = (uint32_t)atoi(v); le = (uint32_t)atoi(strchr(v, ',') + 1); }
                else if (!strcmp(k, "release")) s.adsr[3] = atoi(v);
            }
            FILE *wf = fopen(file, "rb");
            if (!wf || aud_wav_load(e, file_read, wf, &s) != AUD_OK) {
                fprintf(stderr, "%s:%d: can't load %s\n", path, lineno, file);
                if (wf) fclose(wf);
                fclose(f);
                return -1;
            }
            fclose(wf);
            if (le) {
                s.loop_start = ls, s.loop_end = le;
            }
            aud_bank_add(e, handles[atoi(tok[1]) & 31], &s);
        } else if (!strcmp(cmd, "assign")) {
            aud_assign(e, atoi(tok[1]), handles[atoi(tok[2]) & 31]);
        } else if (!strcmp(cmd, "at")) {
            uint32_t ms = (uint32_t)atoi(tok[1]);
            if (ms > now_ms) {
                h_render_ms(ms - now_ms);
                now_ms = ms;
            }
        } else if (!strcmp(cmd, "on")) {
            aud_pv_t b[8];
            int nb = 0;
            for (int i = 4; i < n && nb < 8; i++) {
                char *k, *v;
                if (kv(tok[i], &k, &v) && aud_param_find(k) >= 0) {
                    int pi = aud_param_find(k);
                    b[nb++] = (aud_pv_t){ (uint8_t)pi, aud_param_from_q16(pi, q16(v)) };
                }
            }
            last = aud_note_on(e, atoi(tok[1]), atoi(tok[2]), atoi(tok[3]), 0, b, nb);
        } else if (!strcmp(cmd, "off")) {
            aud_note_off(e, atoi(tok[1]), atoi(tok[2]), 0);
        } else if (!strcmp(cmd, "offh")) {
            aud_note_off_h(e, last, 0);
        } else if (!strcmp(cmd, "cc")) {
            aud_cc(e, atoi(tok[1]), atoi(tok[2]), atoi(tok[3]), 0);
        } else if (!strcmp(cmd, "bend")) {
            aud_bend(e, atoi(tok[1]), atoi(tok[2]), 0);
        } else if (!strcmp(cmd, "param")) {
            int pi = aud_param_find(tok[2]);
            aud_param(e, atoi(tok[1]), pi, aud_param_from_q16(pi, q16(tok[3])), 0);
        } else if (!strcmp(cmd, "nparam")) {
            int pi = aud_param_find(tok[1]);
            aud_note_param(e, last, pi, aud_param_from_q16(pi, q16(tok[2])), 0);
        } else if (!strcmp(cmd, "nramp")) {
            int pi = aud_param_find(tok[1]);
            aud_note_ramp(e, last, pi, aud_param_from_q16(pi, q16(tok[2])), (uint32_t)atoi(tok[3]), 0);
        } else if (!strcmp(cmd, "alloff")) {
            aud_all_off(e, n > 1 && !strcmp(tok[1], "kill"));
        } else if (!strcmp(cmd, "master")) {
            aud_set_master(e, (uint32_t)q16(tok[1]));
        } else {
            fprintf(stderr, "%s:%d: unknown command %s\n", path, lineno, cmd);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}
