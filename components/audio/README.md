# components/audio: the audio module

Synth and sampler voices, a MIDI file sequencer and a tick clock with
per-channel timelines, mixed on the badge and played through a TLV320DAC3100
on the headphone jack. Apps reach it through the Lua `audio` module
(permission `audio`, [docs/LUA_API.md](../../docs/LUA_API.md#audio-permission-audio-solana-os-220));
this file is about the component itself. It implements the "BP26 badge audio
engine" task spec.

It is an optional module: without the DAC (or with `CONFIG_SOLANA_AUDIO` off)
`audio.available()` is false and every call is a no-op, and the OS behaves as
before.

## Layout

| Path | What |
| --- | --- |
| `core/` | The portable engine: no ESP-IDF, FreeRTOS or floating point. The badge, the emulator and the host tests compile these files unchanged. `aud.h` is its API. |
| `audio.c`, `include/audio.h` | The service: lazy start, the mixer task, memory, Settings (volume, mute, test tone), the C API apps and the OS call. |
| `include/audio_port.h` | The output device under the service. |
| `port/audio_port_esp.c`, `port/dac_tlv320.c` | The badge's output: I2S0 and the DAC over the shared I2C bus. |
| `port/audio_port_none.c` | `CONFIG_SOLANA_AUDIO` off. |
| `../../emulator/platform/audio_port_emu.c` | The browser's output: WebAudio. |
| `host/` | Host tests: `make test` (ASan, UBSan), golden renders, `make render` writes WAVs. |
| `tools/` | `audio_prep.sh` (any audio to a badge sample), `make_demo_assets.py` (the examples' sounds), `gen_tables.py` (`core/aud_tables.c`). |

## How it hooks into the OS

The OS knows four things about audio, each one line or one small file:

1. **Boot**: `app_main()` calls `audio_boot()` after `os_start()` (the button
   task opened the I2C bus). It probes 0x18 and loads the Settings values.
   Nothing else starts.
2. **Lua**: `main/lua/lua_audio.c` is the `audio` module; `lua_host.c` opens it
   with the others and calls `lh_audio_stop()` (`audio_release()`) when an app
   ends, after `on_exit`, the way LEDs go off. `LUA_PERM_AUDIO` is bit 10 in
   `lua_pkg.h`, named `"audio"`, shown as "Play sound through the headphone
   jack" in the store and the Apps list.
3. **Settings**: a Sound row opens `main/apps/sound/sound_app.c` (volume 0..10
   in 3 dB steps, mute, a 440 Hz test tone, the DAC's status). The values live
   in the component's own NVS namespace `audio`; `os_settings` is untouched.
4. **Console**: `audio [stats|tone on|off|volume 0..10|mute on|off]`.

The BSP gained `bsp_i2c_bus()` (`components/bsp/include/bsp_i2c.h`): the bus
the buttons already opened, so the DAC shares it. The IDF `i2c_master` driver
locks the bus per transfer, so the expander's reads and the DAC's writes never
collide; the DAC is written only at start, sleep and wake. The pins are in
`board.h` (`BOARD_I2S_BCK` 5, `BOARD_I2S_WS` 2, `BOARD_I2S_DOUT` 26,
`BOARD_DAC_ADDR` 0x18) and menuconfig "Solana OS audio" overrides them.

## Runtime

```
Lua task (prio 2)                      mixer task "audio" (prio 6)           I2S0 DMA
audio.note_on ... -> producer rings -> aud_render(256 frames) -> int16 -> 6 x 256 frames -> DAC
                      (never block)     events split the block            (48 ms at 32 kHz)
                                        at their sample
```

- **Lazy start.** The first app call allocates the engine and starts the
  output and the task; `audio_release()` stops and frees everything.
- **Memory.** Hot state (voices, channels, rings, mix buffers) in internal
  RAM: 22.6 KB on the badge (`aud_engine_size()`). Timelines, staging, banks,
  patches and songs (78 KB, `aud_cold_size()`) and the sample pool in PSRAM.
  The pool is `CONFIG_SOLANA_AUDIO_POOL_KB` (1 MB), never so much that free
  PSRAM drops under `CONFIG_SOLANA_AUDIO_RESERVE_KB` (512 KB): option 1 of the
  spec's memory risk. Plus a 4 KB task stack, a 1 KB block buffer and the
  6 KB DMA ring.
- **Threads.** App calls are the engine's single producer; `s_lock`
  serialises them with `audio_release()` from the OS task. The mixer never
  takes it. Calls never wait for the mixer: a full queue drops the event and
  counts it.
- **Volume.** Settings volume x mute x `audio.volume()` is the engine's master
  gain (digital, smoothed). The DAC's analog stage sits at about -6 dB.
- **DAC sleep.** After `CONFIG_SOLANA_AUDIO_SILENCE_S` (10 s) with nothing
  sounding or scheduled, the mixer mutes the DAC, powers its headphone driver,
  DAC and PLL down, stops I2S and stops rendering. The next call wakes it
  (the headphone driver ramps up over about 150 ms).
- **CPU.** `audio.stats().cpu` is render time over block time, smoothed;
  `peak_cpu` is the worst block.

## The DAC

`port/dac_tlv320.c`. No MCLK is wired: the DAC's PLL runs from BCLK
(32 x fs for 16-bit stereo frames). PLL values come from Adafruit's
CircuitPython driver's BCLK table (22.05 and 48 kHz) and the same derivation
for 32 kHz; all three meet the datasheet's limits (comments in the file).
Interface I2S, 16 bit, BCLK and WCLK inputs; DACs to the headphone mixer
amplifiers; headphone drivers on at 1.65 V common mode; class-D off. The start
reads back J, NDAC and the data path and fails if they differ.

## Wiring (spec phase 0)

| Signal | Badge | DAC board |
| --- | --- | --- |
| I2S BCLK | GPIO 5 | BCK |
| I2S WS | GPIO 2 (strapping) | WSEL |
| I2S DOUT | GPIO 26 (strapping) | DIN |
| I2C SDA | GPIO 24 (shared with the TCA9534) | SDA |
| I2C SCL | GPIO 23 | SCL |
| 3V3, GND | | VIN, GND |
| Reset | | RST to 3V3 via 10 k, 1 uF to GND |

MCLK not connected. Headphones only (the speaker needs 5 V).

## Tests

- `host/`: `make test` runs 29 tests (2,956 checks) under ASan and UBSan:
  acceptance A5 to A8 and A13 to A28 on the core, golden render hashes, and a
  CPU figure for the host. `make fixtures` regenerates the WAV fixtures (ffmpeg).
- `main/lua/tests/t-audio`, `t-audio-noperm`: the Lua surface and the
  permission error (A11). Both pass in the emulator.
- `docs/examples/audio-selftest`: the self-test app (tones, waveforms, 16
  voices, sample kit, demo song, click track, event flood, per-note sweep,
  format switch) with the stats on screen and every 5 s on the console.
- `docs/examples/audio-drumpad`, `docs/examples/audio-song`: examples.

## Still to check on a badge

Everything here builds for the badge and runs in the emulator; these need the
hardware:

- Phase 0: the wiring above, a cold boot and a reset with the DAC fitted
  (GPIO 2 and 26 are strapping pins), the bus scan at 0x18.
- The DAC set-up: PLL lock at each rate (Adafruit saw hiss at 22.05 kHz from a
  jittery BCLK on their board), output level and the -6 dB analog stage, the
  headphone pop on wake.
- A1, A2 (CPU and underruns with Wi-Fi and the display busy), A3 (no DAC), A4
  (50 launches, free PSRAM back to its start), A9 (headphone plug), A10
  (Settings across a reboot).
- Internal RAM: the engine is 22.6 KB plus stack, block and DMA (about 34 KB),
  over the spec's 24 KB estimate. The channel layer (4.7 KB) could move to
  PSRAM if SRAM is short.
