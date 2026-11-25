// CONFIG_SOLANA_AUDIO off: no output, so audio_available() stays false.
#include "audio_port.h"

bool audio_port_probe(void) { return false; }
int audio_port_start(uint32_t rate, int channels, int block) { return -1; }
int audio_port_write(const int16_t *pcm, int frames, uint32_t timeout_ms) { return 0; }
void audio_port_stop(void) {}
void audio_port_sleep(bool asleep) {}
uint32_t audio_port_underruns(void) { return 0; }
uint32_t audio_port_latency(void) { return 0; }
