#pragma once
// Host test harness: an engine on the PC, rendering into memory.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/aud.h"

typedef struct {
    int what, ch, note;
    uint32_t serial, sample, tick;
} trace_ev_t;

typedef struct {
    aud_engine_t *e;
    void *cold, *pool;
    int16_t *pcm;           // everything rendered since the last h_clear
    size_t frames, cap;
    trace_ev_t trace[8192];
    int ntrace;
    int block;              // frames per aud_render call
} harness_t;

extern harness_t H;

void h_init(uint32_t rate, int channels, size_t pool);
void h_free(void);
void h_clear(void);                         // forget the captured PCM and trace
void h_render(uint32_t frames);             // render in blocks of H.block
void h_render_ms(uint32_t ms);
int h_wav_write(const char *path);          // the captured PCM as a WAV
uint64_t h_hash(void);                      // FNV-1a over the captured PCM
int h_count(int what, int ch, int note);    // trace events matching (-1: any)
const trace_ev_t *h_find(int what, int ch, int note, int nth);
int h_script(const char *path);             // run an event script (render.c's format)
