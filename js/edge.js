// A cue fired when a value crosses a line going up, and another going down -
// with a band around the line, so a value that hovers on it fires nothing twice.
//
// Part of Go.dot - https://github.com/pob31/go.dot - GPL-3.0-or-later,
// Copyright (C) 2026 Pierre-Olivier Boulant

const line = 0.8;
const band = 0.05;
let above = false;

godot.hear('/wfs/source/1/x', (x) => {
    if (! above && x > line + band) {
        above = true;
        godot.fire('3.1');      // its number, or its id
    } else if (above && x < line - band) {
        above = false;
        godot.fire('3.2');
    }
});
