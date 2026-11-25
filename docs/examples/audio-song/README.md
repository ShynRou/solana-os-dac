# Song player

Plays `music/demo.mid` (8 bars, format 1: lead on channel 0, bass on 1, drums
on 9) on three synth patches, looped.

- `main.lua`, `app.json`: the app (permission `audio`).
- LEFT/RIGHT: tempo scale 0.5 to 2 (`song:play{tempo=...}`), UP/DOWN: the
  bass's filter (CC 74, which the song sweeps too), SELECT: pause and resume.
- The big counter is bar.beat of what you *hear* (`audio.heard()`), so it
  stays on the beat whatever the output latency.

The sequencer runs inside the mixer, so the song keeps time while Lua is
busy or collecting garbage; the app only reacts to keys.
