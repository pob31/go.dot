-- A value moved part of the way towards each one a device reports - a quarter
-- here - so a jumpy sensor glides. The first value is taken whole.
--
-- Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later,
-- Copyright (C) 2026 Pierre-Olivier Boulant

local part = 0.25
local smooth = nil

godot.hear('/wfs/source/1/x', function (x)
    if smooth == nil then
        smooth = x
    else
        smooth = smooth + (x - smooth) * part
    end
    godot.send('/wfs/source/1/xSmooth', smooth)
end)
