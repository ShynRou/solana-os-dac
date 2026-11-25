-- Drum pad: the d-pad plays a sample kit, SELECT starts a beat on the tick
-- clock, and while it runs every pad you hit is also recorded into the loop
-- at the nearest sixteenth, so the pattern builds up bar by bar.
--
-- Permission "audio". Samples: smp/*.wav (components/audio/tools/
-- make_demo_assets.py makes them; audio_prep.sh converts your own).

local PADS = {
  up = {36, "Kick"}, down = {38, "Snare"}, left = {42, "Hat"}, right = {46, "Open hat"},
}
local CH = 9
local STEPS = 16

ui.bg("bg")
ui.label(10, 4, "Drum pad", {size = 20})
local info = ui.label(10, 34, "", {w = 300})
local grid_label = ui.label(10, 70, "", {size = 12, w = 300})
ui.hint("D-pad play   SELECT beat on/off   CANCEL exit")

if not audio.available() then
  info:text("This badge has no audio DAC.")
  return
end

local kit = assert(audio.bank{
  {file = "smp/kick.wav", root = 36, keys = {36, 36}},
  {file = "smp/snare.wav", root = 38, keys = {38, 38}},
  {file = "smp/hat.wav", root = 42, keys = {42, 42}, adsr = {1, 0, 1, 20}},
  {file = "smp/hat.wav", root = 40, keys = {46, 46}, volume = 1.3},   -- lower and louder: "open"
})
audio.assign(CH, kit)

local pattern = {}             -- step -> {note = true}
local playing, next_bar = false, nil
local g

local function show()
  local rows = {}
  for _, n in ipairs({36, 38, 42, 46}) do
    local s = {}
    for i = 0, STEPS - 1 do
      s[#s + 1] = (pattern[i] and pattern[i][n]) and "x" or (i % 4 == 0 and "|" or ".")
    end
    rows[#rows + 1] = ("%-3d %s"):format(n, table.concat(s))
  end
  grid_label:text(table.concat(rows, "\n"))
end

local function bar_items()
  local items = {}
  local step = g.bar // STEPS
  for i = 0, STEPS - 1 do
    for n in pairs(pattern[i] or {}) do
      items[#items + 1] = {i * step, "note", n, 110, step}
    end
  end
  return items
end

-- Two bars ahead of the clock; a changed pattern replaces the queued bars.
local function feed(rewrite)
  local bar = audio.clock() // g.bar
  if rewrite and next_bar then
    for b = bar + 1, next_bar - 1 do
      audio.update(CH, b * g.bar, (b + 1) * g.bar, bar_items(), {quant = "bar"})
    end
  end
  if not next_bar or next_bar <= bar then next_bar = bar + 1 end
  while next_bar <= bar + 2 do
    audio.insert(CH, bar_items(), {at = next_bar * g.bar})
    next_bar = next_bar + 1
  end
end

function on_key(k, rep)
  if k == "select" then
    playing = not playing
    if playing then
      audio.transport{bpm = 100, ppqn = 96, beats = 4}
      g = audio.grid()
      next_bar = nil
      feed()
    else
      audio.stop{channel = CH}
    end
    info:text(playing and "Beat on: pads you hit join the loop" or "Beat off")
    return true
  end
  local pad = PADS[k]
  if pad and not rep then
    audio.note_on(CH, pad[1], 120)
    if playing then
      -- What was heard, not what is being rendered: the step the user hit.
      local pos = audio.heard() % g.bar
      local step = (pos * STEPS + g.bar // 2) // g.bar % STEPS
      pattern[step] = pattern[step] or {}
      pattern[step][pad[1]] = true
      feed(true)
      show()
    end
    info:text(pad[2])
    return true
  end
  return k ~= "back"
end

function on_tick()
  if playing then feed() end
end

show()
info:text("Hit a pad")
