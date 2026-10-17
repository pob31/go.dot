/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*  THE PAGE'S GESTURES, AS RULES (namespace draft §75, WT.1): the app's numbers
    and the desktop's laws, restated and checked here. */

import { test } from "node:test";
import assert from "node:assert/strict";

import {
  Panic, Scrub, LevelDrag, DialDrag, rateFor, rateText, outside, swipeKills, dialTurned,
  fractionForDb, dbForFraction, isLevel, roundedTo, SILENCE_DB, numberText, aimMoved, aimReach,
  offsetText, AIM_REACH_S,
} from "../../clients/console/model/touch.js";

const near = (actual, wanted, within = 1e-9) =>
  assert.ok(Math.abs(actual - wanted) <= within, actual + " is not " + wanted);

test("PANIC: a press stops, a second within 750 ms drops, and the word under it knows which comes next", () => {
  const panic = new Panic();

  assert.equal(panic.nextIsDouble(0), false);
  assert.equal(panic.armedUntil(), null);
  assert.equal(panic.press(1000), "run.stopAll");
  assert.equal(panic.armedUntil(), 1750);
  assert.equal(panic.nextIsDouble(1750), true);
  assert.equal(panic.press(1500), "run.killAll");

  //  Past the window it is the first level again.
  assert.equal(panic.nextIsDouble(2251), false);
  assert.equal(panic.press(3000), "run.stopAll");
});

test("off the strip, every 40 px halves what a pixel moves, down to a 256th; a swipe kills past 0.6 either way", () => {
  assert.equal(rateFor(0, 40), 1);
  near(rateFor(40, 40), 0.5);
  near(rateFor(80, 40), 0.25);
  near(rateFor(20, 40), Math.SQRT1_2);
  assert.equal(rateFor(10000, 40), 1 / 256);
  assert.equal(rateText(0.25), "1/4");
  assert.equal(rateText(1), "");

  assert.equal(outside(5, 0, 10), 0);
  assert.equal(outside(-3, 0, 10), 3);
  assert.equal(outside(14, 0, 10), 4);

  assert.equal(swipeKills(59, 100), false);
  assert.equal(swipeKills(60, 100), true);
  assert.equal(swipeKills(-61, 100), true);
  assert.equal(swipeKills(10, 0), false);
});

test("a playhead dragged: a grab is not a seek, 1:1 on the strip, finer off it, pushed at the edge, a scene on release", () => {
  const scrub = new Scrub();
  scrub.begin({ position: 10, to: 60, secondsPerPixel: 0.1, x: 100 });

  assert.equal(scrub.due(0), false);                 // nothing moved yet
  near(scrub.moveTo(150, 0), 15);                    // 50 px at a tenth of a second each
  assert.equal(scrub.due(0), true);
  assert.equal(scrub.due(100), false);               // not again within 200 ms
  near(scrub.moveTo(250, 40), 20);                   // 100 px at half the rate
  assert.equal(scrub.due(250), true);
  near(scrub.moveTo(-10000, 0), 0);                  // never before the top
  near(scrub.push(1, 1, 0), 9);                      // a second held at the edge: 90 px
  assert.equal(scrub.settle(), true);
  assert.equal(scrub.settle(), false);

  const scene = new Scrub();
  scene.begin({ position: 2, to: 0, secondsPerPixel: 0.05, x: 0, onRelease: true });
  near(scene.moveTo(1000, 0), 52);                   // no end: a scene's own second goes on
  assert.equal(scene.due(10000), false);             // a scene seeks only on release
  assert.equal(scene.settle(), true);
});

test("the fader's law: its four points, and back", () => {
  near(fractionForDb(SILENCE_DB), 0);
  near(fractionForDb(-60), 0.2);
  near(fractionForDb(0), 0.85);
  near(fractionForDb(12), 1);
  near(fractionForDb(-30), 0.525);
  near(dbForFraction(0.525), -30);
  near(dbForFraction(2), 12);
  near(roundedTo(-2.25, 0.1), -2.3);                 // half away from nought, as C++ rounds
});

test("a level dragged moves along the fader's travel from where it was, finer off the box, and bottoms at silence", () => {
  const drag = new LevelDrag(0, 240, 48);

  near(drag.level(), 0);
  near(drag.move(0, 0), 0);
  near(drag.move(36, 0), 12);                        // 0.85 + 36/240 = 1: the top
  near(drag.move(-36, 48), roundedTo(dbForFraction(1 - 0.075), 0.1));   // half the rate off the box
  assert.equal(drag.move(-1000, 0), SILENCE_DB);
});

test("any other number turns by the master dial's law, a detent every 8 px, the rest carried", () => {
  const seconds = { integer: false, min: 0, max: NaN, unit: "s" };

  near(dialTurned(seconds, 0.5, 1), 0.6);
  near(dialTurned(seconds, 9.9, 2), 11);             // across ten seconds the step grows
  near(dialTurned(seconds, 0.05, -1), 0);

  near(dialTurned({ min: -120, max: 12, unit: "dB" }, 0, 1), roundedTo(dbForFraction(0.85 + 1 / 127), 0.1));
  near(dialTurned({ min: -24, max: 24, unit: "dB" }, 3, 1), 3.5);
  near(dialTurned({ min: 20, max: 20000, unit: "Hz" }, 1000, 16), 2000);
  near(dialTurned({ min: 0, max: 20, unit: "x" }, 1, 12), 2);
  assert.equal(dialTurned({ integer: true, min: 0, max: 10 }, 3, 1), 4);
  near(dialTurned({ min: -1, max: 1, unit: "" }, 0, 1), 0.02);
  near(dialTurned({ min: NaN, max: NaN, unit: "" }, 1.23456, 1), 2.235);

  const drag = new DialDrag(seconds, 1, 8, 48);
  near(drag.move(4, 0), 1);                          // half a detent: nothing yet
  near(drag.move(4, 0), 1.1);                        // the other half
  near(drag.move(8, 48), 1.1);                       // half the rate off the box: half a detent
  near(drag.move(8, 48), 1.2);

  assert.ok(isLevel({ min: -120, max: 12, unit: "dB" }));
  assert.ok(!isLevel({ min: -24, max: 24, unit: "dB" }));
});

test("a number is sent as a box shows it, and the aim moves from the finger, finer off the slider", () => {
  assert.equal(numberText(0.1 + 0.2 + 0.3, false), "0.6");
  assert.equal(numberText(-3.14159, false), "-3.142");
  assert.equal(numberText(3.6, true), "4");

  const slider = { min: -1, max: 60, step: 0.5, width: 244 };
  assert.equal(aimMoved(10, 0, 0, slider), 10);
  assert.equal(aimMoved(10, 40, 0, slider), 20);         // 61 s over 244 px: a quarter of a second a pixel
  assert.equal(aimMoved(10, 40, 48, slider), 15);        // half the rate 48 px off it
  assert.equal(aimMoved(10, -1000, 0, slider), -1);      // never before "before"
  assert.equal(aimMoved(10, 0.8, 0, slider), 10);        // kept to its half-second steps

  //  A tenth of a second is the step now (WT.9), and the number comes out clean: 10.1, never 10.100000000000001.
  const fine = { min: -1, max: 250, step: 0.1, width: 244 };
  assert.equal(aimMoved(10, 0.1, 0, fine), 10.1);
  assert.equal(aimMoved(10, 1, 0, fine), 11);
  assert.equal(String(aimMoved(-1, 244, 0, fine)), "250");
});

test("the aim's slider reaches the whole cue, a minute at least, and says the offset as the window does (WT.9)", () => {
  assert.equal(AIM_REACH_S, 60);
  assert.equal(aimReach([]), 60);           // nothing aimed at
  assert.equal(aimReach([12]), 60);         // a short sound: the minute
  assert.equal(aimReach([250]), 250);       // a four-minute song: its end
  assert.equal(aimReach([250.4]), 260);     // rounded up to the next ten seconds
  assert.equal(aimReach([0, 30, 40]), 70);  // a group: its members summed, its own nought adding nothing
  assert.equal(aimReach([NaN, 0]), 60);     // a length nobody could read

  assert.equal(offsetText(-1), "before");
  assert.equal(offsetText(0), "0.0 s");
  assert.equal(offsetText(12.3), "12.3 s");
  assert.equal(offsetText(70.3), "1:10.3");
  assert.equal(offsetText(252.3), "4:12.3");
  assert.equal(offsetText(3600), "1:00:00.0");
});
