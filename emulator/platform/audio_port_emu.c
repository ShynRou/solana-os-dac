// audio_port for the browser (task spec phase 6): the same mixer core and
// audio.c as the badge, played through WebAudio. Each block the mixer writes
// becomes an AudioBuffer scheduled right after the previous one on the
// AudioContext's clock; the mixer task sleeps while more than AHEAD_S is
// queued, which paces it like the badge's DMA ring paces it there.
//
// Browsers start an AudioContext suspended until the page gets a click or a
// key; the context resumes on the first one (Module.audioResume() does it
// too, for a page that wants its own button).

#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_port.h"
#include "emu.h"

#define AHEAD_S     0.08        // queued audio the mixer keeps ahead (about the badge's 48 ms + a block)

static int s_rate;
static uint32_t s_underruns;

EM_JS(int, emu_audio_supported, (void), {
    return (typeof AudioContext !== 'undefined' || typeof webkitAudioContext !== 'undefined') ? 1 : 0;
});

EM_JS(int, emu_audio_open, (int rate, int channels), {
    try {
        const AC = window.AudioContext || window.webkitAudioContext;
        const ctx = new AC({ sampleRate: rate });
        Module.audio = { ctx: ctx, cursor: 0, channels: channels, rate: rate, started: false };
        if (!Module.audioResume) {
            Module.audioResume = () => { if (Module.audio && Module.audio.ctx.state === 'suspended') Module.audio.ctx.resume(); };
            ['pointerdown', 'keydown', 'touchstart'].forEach(ev =>
                window.addEventListener(ev, () => Module.audioResume(), { capture: true }));
        }
        Module.audioResume();
        return 0;
    } catch (e) {
        console.warn('audio: ' + e);
        return -1;
    }
});

// Queue one block; returns 1 when it started after a gap (an underrun).
EM_JS(int, emu_audio_push, (const int16_t *pcm, int frames), {
    const a = Module.audio;
    if (!a) return 0;
    const ctx = a.ctx;
    const buf = ctx.createBuffer(a.channels, frames, a.rate);
    const src = HEAP16.subarray(pcm >> 1, (pcm >> 1) + frames * a.channels);
    for (let c = 0; c < a.channels; c++) {
        const out = buf.getChannelData(c);
        for (let i = 0; i < frames; i++) out[i] = src[i * a.channels + c] / 32768;
    }
    const node = ctx.createBufferSource();
    node.buffer = buf;
    node.connect(ctx.destination);
    const now = ctx.currentTime;
    let gap = 0;
    if (a.cursor < now) {
        gap = a.started && ctx.state === 'running' ? 1 : 0;
        a.cursor = now + 0.03;
    }
    node.start(a.cursor);
    a.cursor += frames / a.rate;
    a.started = true;
    return gap;
});

// Seconds queued ahead of the context's clock (0 while it is suspended and
// nothing can play: the mixer then waits as it would for a full ring).
EM_JS(double, emu_audio_ahead, (void), {
    const a = Module.audio;
    if (!a) return 0;
    if (a.ctx.state !== 'running') return 1e9;
    return a.cursor - a.ctx.currentTime;
});

EM_JS(void, emu_audio_suspend, (int asleep), {
    const a = Module.audio;
    if (!a) return;
    if (asleep) { a.ctx.suspend(); } else { a.ctx.resume(); a.cursor = 0; a.started = false; }
});

EM_JS(void, emu_audio_close, (void), {
    if (Module.audio) { Module.audio.ctx.close(); Module.audio = null; }
});

bool audio_port_probe(void)
{
    return emu_audio_supported() != 0;
}

int audio_port_start(uint32_t rate, int channels, int block)
{
    s_rate = (int)rate;
    return emu_audio_open((int)rate, channels);
}

int audio_port_write(const int16_t *pcm, int frames, uint32_t timeout_ms)
{
    int64_t until = emu_now_us() + (int64_t)timeout_ms * 1000;
    while (emu_audio_ahead() > AHEAD_S) {
        if (emu_now_us() >= until) {
            return 0;       // suspended (no click yet): drop the block, as a stalled DMA would
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_underruns += (uint32_t)emu_audio_push(pcm, frames);
    return frames;
}

void audio_port_stop(void)
{
    emu_audio_close();
}

void audio_port_sleep(bool asleep)
{
    emu_audio_suspend(asleep);
}

uint32_t audio_port_underruns(void)
{
    return s_underruns;
}

uint32_t audio_port_latency(void)
{
    return (uint32_t)(AHEAD_S * s_rate);
}
