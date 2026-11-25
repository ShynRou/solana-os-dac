#pragma once

// The output device under audio.c: the badge's I2S0 + TLV320DAC3100
// (audio_port_esp.c), or the emulator's WebAudio (emulator/platform/
// audio_port_emu.c). audio.c calls these from its own tasks only.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool audio_port_probe(void);                                // the DAC is there (boot)
// Start the clocks and the DAC for this format. block: frames per write.
int audio_port_start(uint32_t rate, int channels, int block);
// Write one block of interleaved PCM16; blocks until the output has room
// (at most timeout_ms). Returns frames written.
int audio_port_write(const int16_t *pcm, int frames, uint32_t timeout_ms);
void audio_port_stop(void);
void audio_port_sleep(bool asleep);                         // DAC power down / up (FR-5)
uint32_t audio_port_underruns(void);
uint32_t audio_port_latency(void);                          // frames between write and the jack
