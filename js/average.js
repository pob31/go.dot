// The average of the last values a device reported - here the last 10 of
// /wfs/source/1/x - sent on to /wfs/source/1/xAverage each time one arrives.
//
// Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later,
// Copyright (C) 2026 Pierre-Olivier Boulant

const size = 10;
const last = [];

godot.hear('/wfs/source/1/x', (x) => {
    last.push(x);
    if (last.length > size)
        last.shift();
    const sum = last.reduce((a, b) => a + b, 0);
    godot.send('/wfs/source/1/xAverage', sum / last.length);
});
