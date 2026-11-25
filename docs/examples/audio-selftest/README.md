# Audio self-test

The audio module's test bench (task spec phase 7): every kind of sound the
engine makes, with its own figures on screen.

- `main.lua`, `app.json`: the app (permission `audio`).
- `smp/*.wav`, `music/demo.mid`: a drum kit (PCM16 and PCM8), a plucked
  string (IMA ADPCM) and an 8-bar song, made by
  `components/audio/tools/make_demo_assets.py` (no third-party audio).

Rows: a 440 Hz tone, the five waveforms, the five noises
(white, pink, brown, metallic, digital), a 5-note strummed chord on a synth
capped at 3 voices followed by a mono legato line, a 16-voice chord (the CPU figure for
acceptance A1/A2), the sample kit, the demo song looped, a 120 BPM click track
streamed onto channel 9's timeline two bars ahead while every tick churns
the heap so the garbage collector keeps running (A5), a flood of 1,000 events in one callback (A6), a
per-note filter sweep on one of two notes (A14), all off, and the output
format (32 kHz stereo, 48 kHz stereo, 22.05 kHz stereo, 32 kHz mono).

The top lines show `audio.stats()`: CPU, peak CPU, underruns, dropped events,
voices and the sample pool. Every 5 s the same line goes to the console,
starting `t-audio`, so a long run can be logged:

```
t-audio cpu 3.2% (peak 5.1%), underruns 0, dropped 0, voices 4, pool 98/1024 KB
```

Without a DAC the app says so and every call is a no-op (A3). In the
emulator it plays through WebAudio after the first click or key (A12).
