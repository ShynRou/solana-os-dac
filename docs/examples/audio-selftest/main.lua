-- Audio self-test (permission "audio"): tones, every waveform and noise,
-- polyphony limits and chords, 16 voices, the
-- sample kit, a demo song, a click track on the tick clock and an event flood,
-- with the engine's load, underruns, drops and pool use on screen. Pick a test
-- with UP/DOWN and SELECT. The last row switches the output format.
--
-- Console lines start "t-audio" (stats every 5 s), so a script can follow a
-- long run (task spec A1, A2, A5, A6).

local TAG = "t-audio"
local FORMATS = { {32000, 2}, {48000, 2}, {22050, 2}, {32000, 1} }

ui.bg("bg")
ui.label(10, 4, "Audio self-test", {size = 20})
local line1 = ui.label(10, 30, "", {size = 12, w = 300})
local line2 = ui.label(10, 44, "", {size = 12, w = 300})
local msg = ui.label(10, 58, "", {size = 12, w = 300, color = "dim"})

local fi = 1
local NOISES = {"white", "pink", "brown", "metallic", "digital"}
local lead, bass, sine, waves, kit, song
local tone, song_on, click_on = nil, false, false
local next_bar
local since_stats, since_print = 0, 0

local function say(text)
  msg:text(text)
  print(TAG .. " " .. text)
end

local function setup()
  local f = FORMATS[fi]
  assert(audio.init{rate = f[1], channels = f[2]})
  lead = audio.synth{wave = "pulse", pw = 0.3, adsr = {5, 120, 0.6, 200},
                     filter = {cutoff = 0.6, res = 0.3, env = 0.3},
                     lfo = {rate = 5, vibrato = 0.15, delay = 300}, volume = 0.6}
  bass = audio.synth{wave = "saw", adsr = {2, 150, 0.5, 80},
                     filter = {cutoff = 0.35, res = 0.5, env = 0.4}, volume = 0.7}
  sine = audio.synth{wave = "sine", adsr = {5, 0, 1, 60}, volume = 0.4}
  waves = {}
  for i, w in ipairs({"sine", "pulse", "saw", "triangle", "noise"}) do
    waves[i] = audio.synth{wave = w, adsr = {5, 0, 1, 80}, volume = 0.4}
    audio.assign(2 + i, waves[i])                     -- channels 3..7
  end
  for i, nz in ipairs(NOISES) do                    -- channels 10..14
    local hit = nz == "metallic" and {1, 250, 0, 250} or {5, 0, 1, 80}
    audio.assign(9 + i, audio.synth{wave = "noise", noise = nz, adsr = hit, volume = 0.4})
  end
  audio.assign(15, audio.synth{wave = "saw", voices = 3, adsr = {5, 100, 0.6, 250},
                               filter = {cutoff = 0.5, res = 0.2}, volume = 0.4})
  audio.assign(8, audio.synth{wave = "pulse", pw = 0.3, mono = true, legato = true, portamento = 60,
                              adsr = {10, 100, 0.7, 200}, volume = 0.5})
  audio.cc(8, 65, 127)                              -- portamento on
  local err
  kit, err = audio.bank{
    {file = "smp/kick.wav", root = 36, keys = {35, 36}},
    {file = "smp/snare.wav", root = 38, keys = {37, 40}},
    {file = "smp/hat.wav", root = 42, keys = {41, 46}},
    {file = "smp/pluck.wav", root = 60, keys = {48, 84}, adsr = {2, 0, 1, 300}},
  }
  if not kit then say("kit: " .. err) end
  audio.assign(0, lead)
  audio.assign(1, bass)
  audio.assign(2, sine)
  audio.assign(9, kit)
  song, err = audio.song("music/demo.mid")
  if not song then say("song: " .. err) end
  tone, song_on, click_on, next_bar = nil, false, false, nil
end

local function stats_text()
  local s = audio.stats()
  line1:text(("cpu %.1f%% peak %.1f%%  underruns %d  dropped %d"):format(
    s.cpu, s.peak_cpu, s.underruns, s.dropped_events))
  line2:text(("%d Hz x%d  voices %2d  pool %d/%d KB%s"):format(s.rate, s.channels, s.voices_active,
    s.pool_used // 1024, (s.pool_used + s.pool_free) // 1024, s.dac_awake and "" or "  (DAC asleep)"))
  return s
end

-- The click track: whole bars inserted two ahead of the clock, as an app
-- streaming a pattern would (FR-44 to FR-49).
local function feed_click()
  local g = audio.grid()
  local bar = audio.clock() // g.bar
  if not next_bar or next_bar <= bar then next_bar = bar + 1 end
  while next_bar <= bar + 2 do
    local items = {}
    for b = 0, g.bar // g.beat - 1 do
      items[#items + 1] = {b * g.beat, "note", b == 0 and 36 or 42, b == 0 and 120 or 80, 60}
    end
    audio.insert(9, items, {at = next_bar * g.bar})
    next_bar = next_bar + 1
  end
end

local tests = {
  {"Tone 440 Hz", "on/off: A4 on a sine", function()
    if tone then audio.note_off(tone) tone = nil say("tone off")
    else tone = audio.note_on(2, 69, 100) say("tone on") end
  end},
  {"Waveforms", "sine, pulse, saw, triangle, noise", function()
    for i = 1, 5 do
      audio.note_on(2 + i, 60, 100, (i - 1) * 600)
      audio.note_off(2 + i, 60, (i - 1) * 600 + 450)
    end
    say("five waveforms, 0.6 s each")
  end},
  {"Noises", "white, pink, brown, metallic, digital", function()
    for i, nz in ipairs(NOISES) do
      local note = (nz == "metallic" or nz == "digital") and 72 or 120
      audio.note_on(9 + i, note, 100, (i - 1) * 600)
      audio.note_off(9 + i, note, (i - 1) * 600 + 450)
    end
    say("five noises, 0.6 s each")
  end},
  {"Polyphony", "a 5-note chord capped at 3, then mono legato", function()
    local c = audio.chord(15, {48, 52, 55, 60, 64}, 90, 0, 60)
    audio.note_off(c, 1500)
    for i, n in ipairs({60, 62, 64, 67, 64, 62, 60}) do
      audio.note_on(8, n, 100, 1800 + (i - 1) * 180)
    end
    audio.note_off(8, 60, 1800 + 7 * 180)
    say("strummed chord (top 3 ring), then a gliding mono line")
  end},
  {"16 voices", "a 4 s chord: the CPU figure", function()
    for i = 0, 15 do
      local ch = i % 2 == 0 and 0 or 2
      audio.note_on(ch, 48 + i * 2, 90)
      audio.note_off(ch, 48 + i * 2, 4000)
    end
    say("16 voices for 4 s")
  end},
  {"Sample kit", "kick, snare, hats, pluck", function()
    local pat = {36, 42, 38, 42, 36, 36, 38, 42}
    for i, n in ipairs(pat) do audio.note_on(9, n, 110, (i - 1) * 250) end
    for i, n in ipairs({60, 64, 67, 72}) do audio.note_on(9, n, 100, 2000 + (i - 1) * 200) end
    say("drum bar, then the pluck")
  end},
  {"Demo song", "on/off: the bundled .mid, looped", function()
    if not song then say("no song") return end
    if song_on then song:stop() song_on = false say("song stopped")
    else song:play{loop = true, quant = "now"} song_on = true say("song playing") end
  end},
  {"Click track", "on/off: 120 BPM on the grid, heap churn each tick", function()
    if click_on then audio.stop{channel = 9} click_on = false say("click stopped") return end
    audio.transport{bpm = 120, ppqn = 480, beats = 4}
    next_bar = nil
    click_on = true
    say("click at 120 BPM")
  end},
  {"Flood 1000 events", "the queue fills; nothing blocks", function()
    local before = audio.stats().dropped_events
    for i = 1, 1000 do audio.cc(1, 74, i % 128) end
    say(("1000 events: %d dropped, the rest coalesced"):format(audio.stats().dropped_events - before))
  end},
  {"Per-note sweep", "two notes, one filter sweep", function()
    local a = audio.note_on(0, 60, 100, 0, {cutoff = 0.9})
    local b = audio.note_on(0, 67, 100, 0, {cutoff = 0.9})
    audio.note_ramp(b, "cutoff", 0.05, 1500)
    audio.note_off(a, 2000)
    audio.note_off(b, 2000)
    say("the fifth closes, the root stays open")
  end},
  {"All off", "release everything", function()
    audio.all_off(false)
    audio.stop{channel = "all"}
    tone, song_on, click_on = nil, false, false
    say("all off")
  end},
  {"Format", "", function()
    audio.reset()
    fi = fi % #FORMATS + 1
    setup()
    say(("now %d Hz, %s"):format(FORMATS[fi][1], FORMATS[fi][2] == 2 and "stereo" or "mono"))
  end},
}

local function items()
  local out = {}
  for i, t in ipairs(tests) do
    local sub = t[2]
    if t[1] == "Format" then
      local f = FORMATS[fi]
      sub = ("%d Hz %s; SELECT for the next"):format(f[1], f[2] == 2 and "stereo" or "mono")
    end
    out[i] = {title = t[1], sub = sub}
  end
  return out
end

local list
list = ui.list(0, 74, 320, 128, items(), {on_select = function(i)
  if audio.available() then
    tests[i][3]()
    list:items(items())
  end
end})
ui.hint("SELECT run   CANCEL exit")

if audio.available() then
  setup()
  say("ready")
else
  line1:text("No audio DAC on this badge.")
  line2:text("Every audio call is a no-op (A3).")
  print(TAG .. " no DAC")
end

function on_tick(ms)
  if not audio.available() then return end
  if click_on then
    feed_click()
    -- A5: churn the heap so the collector runs often; the clicks stay in time
    -- (apps can't call collectgarbage("collect") in the sandbox).
    for i = 1, 2000 do local _ = {i, tostring(i)} end
  end
  since_stats = since_stats + ms
  since_print = since_print + ms
  if since_stats >= 500 then
    since_stats = 0
    stats_text()
  end
  if since_print >= 5000 then
    since_print = 0
    print(TAG .. " " .. tostring(audio.stats()))
  end
end
