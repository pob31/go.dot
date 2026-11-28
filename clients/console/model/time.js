// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

/*  HOW A TIME IS WRITTEN ON THE PAGE (namespace draft §151, BKM), as the window
    writes it (client/model/TimeText): the show's `/godot/list/timeFormat`,
    seconds - `83.5` - or the clock - `1:23.5`, the hours only past an hour,
    `1:02:03.5` - tenths kept. Typed either way whatever is shown. */

import { tree } from "../plumbing/tree.js";

export function clockShown() {
  return tree.get("/godot/list/timeFormat", "seconds") === "clock";
}

const two = (n) => String(n).padStart(2, "0");

/*  Minutes and seconds, the hours past an hour, `decimals` places; `trim`
    takes off trailing noughts and a point left alone. Rounded once in units of
    the last place, so 59.96 at a tenth is 1:00.0 and never 0:60.0. */
export function clockTime(seconds, decimals = 1, trim = false) {
  const places = Math.max(0, Math.min(3, decimals));
  const scale = Math.pow(10, places);
  const negative = seconds < 0;
  const units = Math.round(Math.abs(seconds) * scale);
  const whole = Math.floor(units / scale);
  const fraction = units % scale;

  const hours = Math.floor(whole / 3600);
  const minutes = Math.floor(whole / 60) % 60;
  const secs = whole % 60;

  let out = negative && units > 0 ? "-" : "";
  out += hours > 0 ? hours + ":" + two(minutes) + ":" + two(secs) : minutes + ":" + two(secs);

  if (places > 0) {
    let digits = String(fraction).padStart(places, "0");
    if (trim) digits = digits.replace(/0+$/, "");
    if (digits) out += "." + digits;
  }

  return out;
}

/*  A reading to a tenth in the show's format: `12.3` or `0:12.3`. */
export function readingTime(seconds) {
  return clockShown() ? clockTime(seconds, 1) : seconds.toFixed(1);
}

/*  The same with the unit a sentence wants: `12.3 s` or `0:12.3`. */
export function readingWords(seconds) {
  return clockShown() ? clockTime(seconds, 1) : seconds.toFixed(1) + " s";
}

/*  Whether a box holds a clock or a time with its units, rather than a plain number. */
export function typedAsClock(text) {
  return /[:hms]/i.test(String(text));
}

/*  A TIME WITH ITS UNITS (§151, BKQ; the author: "Could we also type in 1h34m3s1 and
    convert to 1:34:03.1?"): numbers each followed by h, m or s, in that order; the
    digits after the s are its fraction (`3s1` is 3.1); a number after the last unit
    with none of its own is the next place down (`2m30`, `1h30`). As the window reads it. */
function unitTime(typed) {
  const text = String(typed).replace(/\s+/g, "").replace(/,/g, ".").toLowerCase();
  const weight = { h: 3600, m: 60, s: 1 };
  const order = { h: 3, m: 2, s: 1 };
  const word = { 3: "h", 2: "m", 1: "s" };
  let total = 0;
  let place = 0;
  let at = 0;

  while (at < text.length) {
    const found = /^[0-9.]+/.exec(text.slice(at));
    if (!found) return NaN;
    const value = Number(found[0]);
    if (!Number.isFinite(value) || value < 0) return NaN;
    at += found[0].length;

    let unit;
    if (at < text.length) {
      const c = text[at++];
      if (!(c in order)) return NaN;
      unit = order[c];
    } else {
      unit = place - 1;
    }

    if (unit <= 0 || (place !== 0 && unit >= place)) return NaN;
    if (place !== 0 && value >= 60) return NaN;

    total += value * weight[word[unit]];
    place = unit;

    if (unit === 1 && at < text.length) {
      const tail = text.slice(at);
      if (Math.floor(value) !== value || !/^[0-9]+$/.test(tail)) return NaN;
      total += Number("0." + tail);
      at = text.length;
    }
  }

  return place !== 0 ? total : NaN;
}

/*  A time as typed: `83.5`, `83,5`, `1:23.5`, `1:02:03.5`, `1h34m3s1`. NaN for what is not one. */
export function typedTime(text) {
  if (/[hms]/i.test(String(text))) return unitTime(text);

  const parts = String(text).trim().split(":");
  if (parts.length > 3 || parts.some((p) => p.trim() === "")) return NaN;

  let total = 0;

  for (let at = 0; at < parts.length; at++) {
    const value = Number(parts[at].trim().replace(",", "."));
    if (!Number.isFinite(value) || value < 0) return NaN;
    if (at > 0 && value >= 60) return NaN;
    if (at + 1 < parts.length && Math.floor(value) !== value) return NaN;
    total = total * 60 + value;
  }

  return total;
}
