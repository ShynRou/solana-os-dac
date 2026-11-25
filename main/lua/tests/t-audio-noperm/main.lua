-- Regression test (not shipped): an app without the "audio" permission gets
-- the same Lua error as other modules (task spec A11). Lines start "t-audio-noperm".
local want = 'audio: add the "audio" permission to app.json'
for _, name in ipairs({"available", "init", "synth", "bank", "assign", "note_on", "note_off", "cc", "song",
                       "transport", "insert", "stop", "stats", "volume"}) do
  local ok, err = pcall(audio[name], 0, 60, 100)
  print(("t-audio-noperm %s %s%s"):format((not ok and tostring(err):find(want, 1, true)) and "PASS" or "FAIL",
    name, ok and " (no error)" or ""))
end
ui.label(8, 8, "t-audio-noperm: see the console", {size = 12})
