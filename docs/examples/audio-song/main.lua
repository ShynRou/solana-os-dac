-- Song player: a bundled Standard MIDI File on three synth patches, with the
-- tempo on LEFT/RIGHT, the bass filter on UP/DOWN (CC 74, which the song also
-- sweeps), and a bar counter locked to the music with audio.heard().
--
-- Permission "audio". music/demo.mid: components/audio/tools/make_demo_assets.py.

ui.bg("bg")
ui.label(10, 4, "Song player", {size = 20})
local pos = ui.label(10, 40, "", {size = 32})
local info = ui.label(10, 90, "", {w = 300})
local bar = ui.bar(10, 120, 300, 10, 0)
ui.hint("LEFT/RIGHT tempo   UP/DOWN filter   SELECT play/pause   CANCEL exit")

if not audio.available() then
  info:text("This badge has no audio DAC.")
  return
end

audio.assign(0, audio.synth{wave = "pulse", pw = 0.25, adsr = {5, 100, 0.6, 180},
                             lfo = {rate = 5, vibrato = 0.1, delay = 250}, volume = 0.5})
audio.assign(1, audio.synth{wave = "saw", adsr = {2, 150, 0.6, 60},
                             filter = {cutoff = 0.4, res = 0.5, env = 0.3}, volume = 0.6})
-- Drums from the synth too: a short noise burst; every note on channel 9 is
-- the same hit, the velocity sets the level.
audio.assign(9, audio.synth{wave = "noise", adsr = {1, 60, 0, 40},
                             filter = {cutoff = 0.7}, volume = 0.5})

local song = assert(audio.song("music/demo.mid"))
local tempo, cutoff, playing = 1.0, 64, false

local function play()
  song:play{loop = true, tempo = tempo, quant = "now"}
  playing = true
end

local function status()
  info:text(("tempo x%.2f   filter %d   %s"):format(tempo, cutoff, playing and "playing" or "paused"))
end

function on_key(k, rep)
  if k == "select" and not rep then
    if playing then song:pause() playing = false else play() end
  elseif k == "left" or k == "right" then
    tempo = math.max(0.5, math.min(2.0, tempo + (k == "right" and 0.05 or -0.05)))
    if playing then
      song:pause()                        -- pause and play again: it resumes where it was,
      play()                              -- with the new scale from here on
    end
  elseif k == "up" or k == "down" then
    cutoff = math.max(0, math.min(127, cutoff + (k == "up" and 8 or -8)))
    audio.cc(1, 74, cutoff)
  else
    return k ~= "back"
  end
  status()
  return true
end

function on_tick()
  local g = audio.grid()
  local t = audio.heard()
  pos:text(("%d.%d"):format(t // g.bar + 1, t % g.bar // g.beat + 1))
  bar:set(t % g.bar * 100 // g.bar)
end

play()
status()
