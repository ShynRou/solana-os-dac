-- Regression test (not shipped): the audio module's Lua surface. Needs a DAC
-- (or the emulator). Lines start "t-audio"; the last is "t-audio done".
local function check(name, ok, extra)
  print(("t-audio %s %s%s"):format(ok and "PASS" or "FAIL", name, extra and (" " .. tostring(extra)) or ""))
end
ui.label(8, 8, "t-audio: see the console", {size = 12})

if not audio.available() then
  print("t-audio SKIP no DAC")
  print("t-audio done")
  return
end

check("init", audio.init{rate = 32000, channels = 2} == true)
local lead = audio.synth{wave = "saw", adsr = {5, 50, 0.7, 100}, filter = {cutoff = 0.5, res = 0.2}}
check("synth handle", math.type(lead) == "integer" and lead > 0, lead)
check("synth bad wave", not pcall(audio.synth, {wave = "fm"}))
local b, err = audio.bank{{file = "nope.wav"}}
check("bank missing file", b == nil and tostring(err):find("nope.wav", 1, true) ~= nil, err)
check("assign", audio.assign(0, lead) == true)
check("channel range", not pcall(audio.assign, 16, lead))
local n = audio.note_on(0, 60, 100, 0, {cutoff = 0.8, pan = -0.5})
check("note handle", math.type(n) == "integer" and n >= 0x40000000, n)
check("note_param", audio.note_param(n, "resonance", 0.5) == true)
check("note_ramp", audio.note_ramp(n, "cutoff", 0.2, 300) == true)
check("unknown param", not pcall(audio.param, 0, "nope", 1))
check("bundle names", not pcall(audio.note_on, 0, 61, 100, 0, {wobble = 1}))
check("param", audio.param(0, "cutoff", 0.6) == true)
check("cc", audio.cc(0, 74, 90) == true)
check("bend", audio.bend(0, 0.5) == true)
check("nrpn", audio.nrpn(0, 1, 40) == true)
check("map_cc", audio.map_cc(0, 16, "vibrato_rate", 0.1, 12) == true)
local g0 = audio.transport{bpm = 120, ppqn = 480, beats = 4}
local g = audio.grid()
check("grid", g.bar == 1920 and g.beat == 480 and g.ppqn == 480 and math.abs(g.bpm - 120) < 0.01,
  ("%d %d %d %.2f"):format(g.bar, g.beat, g.ppqn, g.bpm))
local at = audio.insert(1, {{0, "note", 48, 100, 240}, {480, "cc", 74, 30}, {960, "param", "cutoff", 0.4}},
                        {quant = "bar"})
check("insert on the bar", math.type(at) == "integer" and at % g.bar == 0, at)
check("bad item", not pcall(audio.insert, 1, {{0, "chord"}}))
check("stop channel", math.type(audio.stop{channel = 1}) == "integer")
local before = audio.stats().dropped_events
for i = 1, 1000 do audio.cc(2, 1, i % 128) end
local after = audio.stats().dropped_events
check("flood counts drops", after > before, after - before)
check("stats tostring", tostring(audio.stats()):find("underruns", 1, true) ~= nil)

local t0, c0, waited, done = nil, nil, 0, false
function on_tick(ms)
  if done then return end
  waited = waited + ms
  if not t0 then t0, c0 = waited, audio.clock() return end
  if waited < 600 then return end
  local s = audio.stats()
  check("running", s.running and s.rate == 32000 and s.channels == 2)
  check("voices", s.voices_active >= 1, s.voices_active)
  check("clock moves", audio.clock() > c0, audio.clock() - c0)
  check("heard behind clock", audio.heard() <= audio.clock())
  check("note_off handle", audio.note_off(n, 0) == true)
  check("all_off", audio.all_off(true) == true)
  print("t-audio stats " .. tostring(s))
  print("t-audio done")
  done = true
end
