-- The smallest and the largest of what a device reported over the last two
-- seconds, sent on as two values each time one arrives.
--
-- Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later,
-- Copyright (C) 2026 Pierre-Olivier Boulant

local window = 2.0 * 50     -- two seconds, in ticks
local now = 0
local seen = {}             -- { tick, value }

godot.tick(function (n) now = n end)

godot.hear('/wfs/source/1/x', function (x)
    seen[#seen + 1] = { now, x }
    while #seen > 0 and seen[1][1] <= now - window do
        table.remove(seen, 1)
    end
    local low, high = math.huge, -math.huge
    for _, entry in ipairs(seen) do
        low = math.min(low, entry[2])
        high = math.max(high, entry[2])
    end
    godot.send('/wfs/source/1/xRange', low, high)
end)
