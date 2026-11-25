// WAV loader (FR-15, FR-18): PCM 8 and 16 bit, and IMA ADPCM (the WAV block
// layout ffmpeg's adpcm_ima_wav writes), mono only, decoded once to PCM16.
// A "smpl" chunk's first loop becomes the sample's loop.

#include <string.h>
#include "aud_int.h"

static const int16_t s_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t s_ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    aud_read_fn read;
    void *ud;
    uint8_t buf[256];
    size_t len, off;
    bool eof;
} in_t;

static size_t in_read(in_t *in, void *dst, size_t n)
{
    size_t got = 0;
    uint8_t *d = dst;
    while (got < n) {
        if (in->off == in->len) {
            if (in->eof) {
                break;
            }
            in->len = in->read(in->ud, in->buf, sizeof(in->buf));
            in->off = 0;
            if (!in->len) {
                in->eof = true;
                break;
            }
        }
        size_t k = in->len - in->off < n - got ? in->len - in->off : n - got;
        if (d) {
            memcpy(d + got, in->buf + in->off, k);
        }
        in->off += k;
        got += k;
    }
    return got;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

typedef int16_t *(*alloc_fn)(void *ctx, size_t frames);

static int parse(in_t *in, alloc_fn alloc, void *ctx, aud_sample_t *s, size_t *frames_out)
{
    uint8_t h[12];
    if (in_read(in, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        return AUD_ERR_FORMAT;
    }
    uint16_t fmt = 0, chans = 0, align = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false, have_data = false;
    int16_t *out = NULL;
    size_t frames = 0;
    for (;;) {
        uint8_t ch[8];
        if (in_read(in, ch, 8) != 8) {
            break;
        }
        uint32_t size = le32(ch + 4), pad = size & 1;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[16];
            if (size < 16 || in_read(in, f, 16) != 16) {
                return AUD_ERR_FORMAT;
            }
            fmt = le16(f);
            chans = le16(f + 2);
            rate = le32(f + 4);
            align = le16(f + 12);
            bits = le16(f + 14);
            in_read(in, NULL, size - 16 + pad);
            have_fmt = true;
            if (chans != 1) {
                return chans == 2 ? AUD_ERR_STEREO : AUD_ERR_FORMAT;
            }
            if (rate < 1000 || rate > 48000 || !((fmt == 1 && (bits == 8 || bits == 16)) ||
                    (fmt == 0x11 && bits == 4 && align >= 5))) {
                return AUD_ERR_FORMAT;
            }
        } else if (!memcmp(ch, "data", 4) && !have_data) {
            if (!have_fmt) {
                return AUD_ERR_FORMAT;
            }
            if (fmt == 1) {
                frames = bits == 16 ? size / 2 : size;
            } else {
                uint32_t per = (uint32_t)(align - 4) * 2 + 1, blocks = size / align, rest = size % align;
                frames = (size_t)blocks * per + (rest > 4 ? (rest - 4) * 2 + 1 : rest == 4 ? 1 : 0);
            }
            if (!frames) {
                return AUD_ERR_FORMAT;
            }
            out = alloc(ctx, frames);
            if (!out) {
                return AUD_ERR_NOMEM;
            }
            size_t got = 0;
            if (fmt == 1 && bits == 16) {
                uint8_t b[2];
                while (got < frames && in_read(in, b, 2) == 2) {
                    out[got++] = (int16_t)le16(b);
                }
            } else if (fmt == 1) {
                uint8_t b;
                while (got < frames && in_read(in, &b, 1) == 1) {
                    out[got++] = (int16_t)(((int)b - 128) * 256);
                }
            } else {
                uint32_t left = size;
                while (left >= 4 && got < frames) {
                    uint32_t blk = left < align ? left : align;
                    uint8_t hd[4];
                    if (in_read(in, hd, 4) != 4) {
                        break;
                    }
                    int32_t pred = (int16_t)le16(hd);
                    int idx = hd[2] > 88 ? 88 : hd[2];
                    out[got++] = (int16_t)pred;
                    for (uint32_t i = 4; i < blk && got < frames; i++) {
                        uint8_t byte;
                        if (in_read(in, &byte, 1) != 1) {
                            break;
                        }
                        for (int nib = 0; nib < 2 && got < frames; nib++) {
                            int code = nib ? byte >> 4 : byte & 15;
                            int step = s_ima_step[idx];
                            // ffmpeg's form (its encoder writes for it): one
                            // multiply, no per-bit truncation.
                            int diff = ((2 * (code & 7) + 1) * step) >> 3;
                            pred += code & 8 ? -diff : diff;
                            pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
                            idx += s_ima_index[code];
                            idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
                            out[got++] = (int16_t)pred;
                        }
                    }
                    left -= blk;
                }
            }
            if (got < frames) {
                memset(out + got, 0, (frames - got) * sizeof(int16_t));
            }
            uint32_t used = fmt == 1 ? (uint32_t)(bits == 16 ? got * 2 : got) : size;
            in_read(in, NULL, (size > used ? size - used : 0) + pad);
            have_data = true;
        } else if (!memcmp(ch, "smpl", 4) && size >= 36 && s) {
            uint8_t sm[36 + 24];
            uint32_t want = size >= 60 ? 60 : 36;
            if (in_read(in, sm, want) != want) {
                break;
            }
            if (want == 60 && le32(sm + 28) > 0) {
                s->loop_start = le32(sm + 36 + 8);
                s->loop_end = le32(sm + 36 + 12) + 1;     // inclusive in the file
            }
            in_read(in, NULL, size - want + pad);
        } else {
            in_read(in, NULL, (size_t)size + pad);
        }
    }
    if (!have_data) {
        return AUD_ERR_FORMAT;
    }
    if (s) {
        s->data = out;
        s->len = (uint32_t)frames;
        s->rate = rate;
        if (s->loop_end > s->len) {
            s->loop_end = s->len;
        }
    }
    if (frames_out) {
        *frames_out = frames;
    }
    return AUD_OK;
}

static int16_t *pool_alloc16(void *ctx, size_t frames)
{
    return aud_pool_alloc(ctx, frames * sizeof(int16_t));
}

int aud_wav_load(aud_engine_t *e, aud_read_fn read, void *ud, aud_sample_t *s)
{
    in_t in = { .read = read, .ud = ud };
    size_t mark = e->pool_used;
    s->loop_start = s->loop_end = 0;
    int rc = parse(&in, pool_alloc16, e, s, NULL);
    if (rc != AUD_OK) {
        e->pool_used = mark;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// From memory (host tests)

typedef struct {
    const uint8_t *p;
    size_t left;
} mem_t;

static size_t mem_read(void *ud, void *buf, size_t n)
{
    mem_t *m = ud;
    if (n > m->left) {
        n = m->left;
    }
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return n;
}

typedef struct {
    int16_t *out;
    size_t max;
} buf_t;

static int16_t *buf_alloc(void *ctx, size_t frames)
{
    buf_t *b = ctx;
    return frames <= b->max ? b->out : NULL;
}

int aud_wav_decode(const uint8_t *data, size_t len, int16_t *out, size_t max_frames, uint32_t *rate)
{
    mem_t m = { data, len };
    in_t in = { .read = mem_read, .ud = &m };
    buf_t b = { out, max_frames };
    aud_sample_t s;
    memset(&s, 0, sizeof(s));
    size_t frames = 0;
    int rc = parse(&in, buf_alloc, &b, &s, &frames);
    if (rc != AUD_OK) {
        return rc;
    }
    if (rate) {
        *rate = s.rate;
    }
    return (int)frames;
}
