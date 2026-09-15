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

/*  THE PAGE'S OSC ENCODER, AGAINST BYTES WRITTEN BY HAND.

    clients/console/plumbing/osc.js writes every datagram the console sends:
    each GO, each standby move, each field committed. The engine's decoder is
    held in tests/OscCodecTests.cpp to packets assembled there byte by byte,
    and this file holds the page's encoder to the same packets, built the same
    way - a string helper, a u32 and a u64 - rather than to anything the engine
    produced. A fixture made by the code on the other end of the wire would
    prove only that the two agree with each other, which is the failure the
    separate-codec rule exists to catch. */

import { test } from "node:test";
import assert from "node:assert/strict";

import { oscString, oscEncode, str, int, dbl } from "../../clients/console/plumbing/osc.js";

/*  An OSC string on the wire: its bytes, a NUL, then padding to four.
    OscCodecTests' `str`, with the UTF-8 left to the caller where it matters. */
function s(text) {
  const raw = [];

  for (const c of text) raw.push(c.charCodeAt(0));

  raw.push(0);
  while (raw.length % 4 !== 0) raw.push(0);

  return raw;
}

function u32(v) {
  return [(v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff];
}

function u64(high, low) {
  return [...u32(high), ...u32(low)];
}

const bytes = (packet) => Array.from(packet);

test("the specification's own example: 440 as a float", () => {
  /*  440.0f is 0x43DC0000 - sign 0, exponent 135, mantissa 0x5C0000 - which
      OscCodecTests checks by hand for the same reason. */
  assert.deepEqual(bytes(oscEncode("/oscillator/4/frequency", [{ tag: "f", value: 440 }])),
                   [...s("/oscillator/4/frequency"), ...s(",f"), ...u32(0x43dc0000)]);
});

test("a string pads to four, and an aligned one takes four more", () => {
  /*  The rule that desyncs a reader when it is got wrong: the terminator comes
      BEFORE the padding, so a string already a multiple of four long takes
      four more bytes, not none. */
  assert.equal(oscString("").length, 4);
  assert.equal(oscString("abc").length, 4);
  assert.equal(oscString("abcd").length, 8);
  assert.equal(oscString("/foo").length, 8);

  assert.deepEqual(bytes(oscEncode("/x", [str("abcd"), str("abc")])),
                   [...s("/x"), ...s(",ss"), ...s("abcd"), ...s("abc")]);
});

test("every tag the page sends, against bytes written by hand", () => {
  /*  OscCodecTests' "every type tag" packet, cut down to the tags this page
      has a use for: i f d s T F. The page sends no h, b, N, I or t. */
  assert.deepEqual(
    bytes(oscEncode("/every", [int(1000), { tag: "f", value: 1 }, dbl(1), str("hello"),
                               { tag: "T" }, { tag: "F" }])),
    [...s("/every"), ...s(",ifdsTF"),
     ...u32(0x000003e8),                // i  1000
     ...u32(0x3f800000),                // f  1.0
     ...u64(0x3ff00000, 0x00000000),    // d  1.0
     ...s("hello")]);                   // s, and T F carry no payload at all
});

test("a negative whole number goes as two's complement", () => {
  assert.deepEqual(bytes(oscEncode("/i", [int(-1)])), [...s("/i"), ...s(",i"), ...u32(0xffffffff)]);
});

test("an aim is sent as a double, because a float would move it", () => {
  /*  osc.js's reason for `dbl`: an offset of 1.1 seconds through a float32 is
      not 1.1, and the aim is compared with what the engine spells back. The
      two spellings, by hand. */
  assert.deepEqual(bytes(oscEncode("/a", [dbl(1.1)])),
                   [...s("/a"), ...s(",d"), ...u64(0x3ff19999, 0x9999999a)]);
  assert.deepEqual(bytes(oscEncode("/a", [{ tag: "f", value: 1.1 }])),
                   [...s("/a"), ...s(",f"), ...u32(0x3f8ccccd)]);
});

test("text that is not ASCII goes as UTF-8", () => {
  /*  "Été": U+00C9 is C3 89, t is 74, U+00E9 is C3 A9 - five bytes, a NUL, and
      two of padding. A show written in French is the ordinary case here. */
  assert.deepEqual(bytes(oscEncode("/n", [str("Été")])),
                   [...s("/n"), ...s(",s"), 0xc3, 0x89, 0x74, 0xc3, 0xa9, 0x00, 0x00, 0x00]);
});

test("a list goes as one string, which is how the door takes one", () => {
  /*  §14.6: a fade's points, or a route's gains, are written as the text the
      file uses, one argument. A datagram carrying them as N numbers would be
      written as its first (EngineNamespace takes the first argument), so the
      page must never send one that way. */
  const packet = oscEncode("/godot/cue/E4GP6QSC/points", [str("0 0 0.5 -30 1 -10")]);

  assert.deepEqual(bytes(packet),
                   [...s("/godot/cue/E4GP6QSC/points"), ...s(",s"), ...s("0 0 0.5 -30 1 -10")]);
});
