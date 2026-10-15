-- The average of the last values a device reported - here the last 10 of
-- /wfs/source/1/x - sent on to /wfs/source/1/xAverage each time one arrives.
--
-- Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later,
-- Copyright (C) 2026 Pierre-Olivier Boulant

local size = 10
local last = {}

godot.hear('/wfs/source/1/x', function (x)
    last[#last + 1] = x
    if #last > size then
        table.remove(last, 1)
    end
    local sum = 0
    for _, value in ipairs(last) do
        sum = sum + value
    end
    godot.send('/wfs/source/1/xAverage', sum / #last)
end)
