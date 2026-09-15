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

/*  OSC on the wire, and the typed values a command's arguments are made of.
    Pure: nothing here touches the page, which is what lets 5.18 check the
    encoder in node against the byte fixtures tests/OscCodecTests.cpp wrote. */

/*  OSC 1.0 on the wire: four-byte aligned everywhere, big-endian everywhere.
    Written out rather than pulled from a library because it is forty lines and
    a dependency this page could not fetch anyway - the engine serves this
    folder and nothing else, and a client that needed a CDN would be useless in
    the one room it exists for. */
function oscString(text) {
  const raw = new TextEncoder().encode(text);
  const size = (raw.length + 4) & ~3;          // at least one NUL, then pad to four
  const out = new Uint8Array(size);
  out.set(raw);
  return out;
}

function oscEncode(address, args) {
  let tags = ",";
  for (const arg of args) tags += arg.tag;

  const parts = [oscString(address), oscString(tags)];

  for (const arg of args) {
    if (arg.tag === "T" || arg.tag === "F") continue;      // the tag IS the value

    if (arg.tag === "s") { parts.push(oscString(String(arg.value))); continue; }

    /*  Eight bytes for a double and four for everything else, which is the
        whole of OSC 1.0's difference between them. */
    const wide = arg.tag === "d";
    const buffer = new ArrayBuffer(wide ? 8 : 4);
    const view = new DataView(buffer);

    if (wide) view.setFloat64(0, Number(arg.value), false);
    else if (arg.tag === "i") view.setInt32(0, Number(arg.value) | 0, false);
    else view.setFloat32(0, Number(arg.value), false);

    parts.push(new Uint8Array(buffer));
  }

  let total = 0;
  for (const part of parts) total += part.length;

  const packet = new Uint8Array(total);
  let at = 0;

  for (const part of parts) { packet.set(part, at); at += part.length; }

  return packet;
}


const str = (value) => ({ tag: "s", value: String(value) });
const int = (value) => ({ tag: "i", value: Number(value) });

/*  A DOUBLE, because `list.aim` asks for one. An offset in seconds through a
    float32 would round - 1.1 seconds is not 1.1 in 32 bits - and the aim is
    compared with what the engine spells back, so the two have to agree exactly
    or the slider would drift under its own readout. */
const dbl = (value) => ({ tag: "d", value: Number(value) });

export { oscString, oscEncode, str, int, dbl };
