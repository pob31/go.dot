// The smallest and the largest of what a device reported over the last two
// seconds, sent on as two values each time one arrives.
//
// Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later,
// Copyright (C) 2026 Pierre-Olivier Boulant

const window = 2.0 * 50;    // two seconds, in ticks
let now = 0;
const seen = [];            // [tick, value]

godot.tick((n) => { now = n; });

godot.hear('/wfs/source/1/x', (x) => {
    seen.push([now, x]);
    while (seen.length > 0 && seen[0][0] <= now - window)
        seen.shift();
    const values = seen.map((s) => s[1]);
    godot.send('/wfs/source/1/xRange', Math.min(...values), Math.max(...values));
});
