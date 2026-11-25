# Drum pad

A sample kit on the d-pad and a beat that records what you play.

- `main.lua`, `app.json`: the app (permission `audio`).
- `smp/*.wav`: kick, snare and hat from `components/audio/tools/make_demo_assets.py`.

UP kick, DOWN snare, LEFT hat, RIGHT open hat (the hat sample a few semitones
down, louder). SELECT starts a 100 BPM loop on the tick clock: the app keeps
two bars queued on channel 9's timeline (`audio.insert` from `on_tick`), and
every pad you hit while it runs is placed at the nearest sixteenth of what you
*heard* (`audio.heard()`), then the bars already queued are replaced with
`audio.update` so the new hit sounds from the next bar on.

To use your own sounds, convert them first:

```
components/audio/tools/audio_prep.sh -t 1 -g -1 my-kick.flac smp/kick.wav
```
