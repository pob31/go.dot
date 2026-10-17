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

/*  THE PAGE'S GESTURES, AS RULES (namespace draft §75, WT.1).

    What a finger - or a mouse, the same events (ARL) - does to the show, worked
    out with no document and no event: PANIC's press, a swipe that kills, how
    far a finger off a strip divides what a pixel moves, a playhead dragged, a
    level moved along the fader's travel and any other number by the master
    dial's law. Pure, so `node --test` reads every rule the gestures use.

    THE NUMBERS ARE THE APP'S (ARM), and the app took them from the desktop:
    the Android remote's `Panic`, `Scrub`, `FaderDrag` and the running pane's
    swipe (go.dot-remote, model/ and ui/), the desktop's `surface::dialTurned`
    and its fader curve (src/wfg/engine/surface). A rule changed there is
    changed here, and the tests say which numbers they are. */

//  How long a second press counts as the second level (§4.4's double Esc; the app's, the desktop's, a surface's).
export const DOUBLE_PRESS_MS = 750;

//  How long a finger held still is a long press, and how far it may wander and still be a tap.
export const LONG_PRESS_MS = 500;
export const SLOP_PX = 10;

//  A run swiped past this share of its row's width is killed; one still there this long after slides back.
export const SWIPE_KILL_SHARE = 0.6;
export const SWIPE_BACK_MS = 1500;

//  Every this many pixels off a strip halves what a pixel moves: the playhead's, and a number's.
export const SCRUB_UNIT_PX = 40;
export const NUMBER_UNIT_PX = 48;

//  The finest a finger off the strip gets.
export const FINEST_RATE = 1 / 256;

//  A playhead held at the screen's edge keeps moving at this many pixels a second; a seek at most this often.
export const PUSH_PIXELS_PER_SECOND = 90;
export const EDGE_PX = 28;
export const SEEK_EVERY_MS = 200;

//  A held number's value at most this often; a value sent shown this long until the engine agrees.
export const WRITE_EVERY_MS = 20;
export const SENT_SHOWN_MS = 1500;

//  A level's whole travel, dragged; any other number, a detent of the master dial for this many pixels.
export const LEVEL_TRAVEL_PX = 240;
export const DETENT_PX = 8;

/*  PANIC (ARI, the app's ANW): a press stops everything gracefully, a second
    within the window drops everything. `nextIsDouble` is what the word under
    the button says before the press. */
export class Panic {
  constructor() {
    this.last = null;
  }

  nextIsDouble(now) {
    return this.last !== null && now >= this.last && now - this.last <= DOUBLE_PRESS_MS;
  }

  /** The command a press at `now` sends. */
  press(now) {
    const second = this.nextIsDouble(now);
    this.last = now;
    return second ? "run.killAll" : "run.stopAll";
  }

  /** When the next press stops being the second level, or null before any press. */
  armedUntil() {
    return this.last === null ? null : this.last + DOUBLE_PRESS_MS;
  }
}

/*  WHAT A PIXEL MOVES, a finger `distance` pixels off the strip: all of it on
    the strip, half for every `unit` off it, smoothly, down to a 256th. */
export function rateFor(distance, unit) {
  if (!(distance > 0) || !(unit > 0)) return 1;
  return Math.max(FINEST_RATE, Math.pow(0.5, distance / unit));
}

/** "1/8" for an eighth; nothing at full rate. */
export function rateText(rate) {
  return rate < 1 ? "1/" + Math.round(1 / rate) : "";
}

/*  HOW FAR OUTSIDE A BAND a point is: nought within it, else its distance to
    the nearer edge. The band is the strip's, across its own axis. */
export function outside(at, low, high) {
  if (at < low) return low - at;
  if (at > high) return at - high;
  return 0;
}

/** Whether a run swiped `dx` pixels across a row `width` wide is killed: far enough either way. */
export function swipeKills(dx, width) {
  return width > 0 && Math.abs(dx) >= SWIPE_KILL_SHARE * width;
}

/*  A PLAYHEAD DRAGGED (the app's `Scrub`): relative from where the finger
    landed - a grab is not a seek - moving by the strip's seconds a pixel, at
    the rate the finger's distance off the strip gives; pushed on while held at
    the screen's edge; sent at most every 200 ms, or only on release for a
    scene. */
export class Scrub {
  constructor() {
    this.active = false;
    this.target = 0;
    this.rate = 1;
  }

  begin({ position, to, secondsPerPixel, unit = SCRUB_UNIT_PX, x = 0, from = 0, onRelease = false }) {
    this.active = true;
    this.sendsOnRelease = onRelease;
    this.earliest = Math.max(0, from);
    this.latest = to;
    this.secondsPerPixel = secondsPerPixel;
    this.unit = unit > 0 ? unit : SCRUB_UNIT_PX;
    this.lastX = x;
    this.rate = 1;
    this.target = this.clamp(position);
    this.sentAt = this.target;
    this.sentMs = -1e9;
  }

  end() {
    this.active = false;
  }

  moveTo(x, distance) {
    if (!this.active) return this.target;
    this.rate = rateFor(distance, this.unit);
    this.target = this.clamp(this.target + (x - this.lastX) * this.secondsPerPixel * this.rate);
    this.lastX = x;
    return this.target;
  }

  push(direction, seconds, distance) {
    if (!this.active || direction === 0 || !(seconds > 0)) return this.target;
    this.rate = rateFor(distance, this.unit);
    this.target = this.clamp(this.target + direction * PUSH_PIXELS_PER_SECOND * seconds * this.secondsPerPixel * this.rate);
    return this.target;
  }

  /** Whether a seek is due now: moved since the last, not a scene, and the interval past. */
  due(now, interval = SEEK_EVERY_MS) {
    if (!this.active || this.sendsOnRelease || !this.unsent() || now - this.sentMs < interval) return false;
    this.sentMs = now;
    this.sentAt = this.target;
    return true;
  }

  /** At release: whether the last place is still to be sent. */
  settle() {
    if (!this.active) return false;
    const moved = this.unsent();
    this.sentAt = this.target;
    return moved;
  }

  unsent() {
    return Math.abs(this.target - this.sentAt) > 1e-9;
  }

  clamp(seconds) {
    const held = Math.max(this.earliest, seconds);
    return this.latest > this.earliest ? Math.min(held, this.latest) : held;
  }
}

//==============================================================================
/*  THE FADER'S LAW (the desktop's generic curve, surface/FaderCurve.h): four
    points, bottom to top, a straight line between each two. The bottom is
    silence, -120 dB as the table spells it. */
export const SILENCE_DB = -120;
const FADER = [[0, SILENCE_DB], [0.2, -60], [0.85, 0], [1, 12]];

export function fractionForDb(db) {
  if (!Number.isFinite(db)) return 0;
  const level = Math.min(Math.max(db, FADER[0][1]), FADER[FADER.length - 1][1]);

  for (let at = 1; at < FADER.length; at++) {
    const [lowPlace, lowDb] = FADER[at - 1];
    const [highPlace, highDb] = FADER[at];
    if (level > highDb) continue;
    const span = highDb - lowDb;
    return span > 0 ? lowPlace + (highPlace - lowPlace) * (level - lowDb) / span : lowPlace;
  }

  return 1;
}

export function dbForFraction(fraction) {
  if (!Number.isFinite(fraction)) return SILENCE_DB;
  const place = Math.min(Math.max(fraction, 0), 1);

  for (let at = 1; at < FADER.length; at++) {
    const [lowPlace, lowDb] = FADER[at - 1];
    const [highPlace, highDb] = FADER[at];
    if (place > highPlace) continue;
    const span = highPlace - lowPlace;
    return span > 0 ? lowDb + (highDb - lowDb) * (place - lowPlace) / span : lowDb;
  }

  return FADER[FADER.length - 1][1];
}

//  C++'s std::round, half away from nought; JavaScript's rounds a half up.
function halfAway(x) {
  return Math.sign(x) * Math.round(Math.abs(x));
}

//  A step that is a whole fraction of one is rounded by its count, so 0.1 stays exact.
export function roundedTo(value, step) {
  if (step > 0 && step < 1) {
    const per = Math.round(1 / step);
    if (per > 0 && Math.abs(per * step - 1) < 1e-9) return halfAway(value * per) / per;
  }
  return halfAway(value / step) * step;
}

function clamp(value, low, high) {
  return Math.min(Math.max(value, low), high);
}

/*  A LEVEL DRAGGED (the app's `FaderDrag`): from where it is, along the
    fader's travel - `along` pixels up of a travel `LEVEL_TRAVEL_PX` long, at
    the rate the finger's distance off the box gives - kept to a tenth of a
    decibel, and the bottom silence exactly. */
export class LevelDrag {
  constructor(startDb, lengthPx = LEVEL_TRAVEL_PX, unitPx = NUMBER_UNIT_PX) {
    this.place = fractionForDb(startDb);
    this.length = lengthPx;
    this.unit = unitPx;
    this.rate = 1;
  }

  move(along, across) {
    this.rate = rateFor(across, this.unit);
    if (this.length > 0) this.place = clamp(this.place + along / this.length * this.rate, 0, 1);
    return this.level();
  }

  level() {
    return this.place <= 0 ? SILENCE_DB : roundedTo(dbForFraction(this.place), 0.1);
  }
}

//==============================================================================
/*  THE MASTER DIAL'S LAW (the desktop's `surface::dialTurned`, which the wheel
    over a number turns by too, §74 AQP): a number turned `steps` detents,
    kept in its range. `range` is { integer, min, max, unit }, an end that is
    not a number being none. The desktop's constants, restated. */
const OCTAVES_PER_DETENT = 1 / 16;
const GAIN_STEP_DB = 0.5;
const WIDTH_DOUBLINGS_PER_DETENT = 1 / 8;
const LEVEL_TRAVEL_PER_DETENT = 1 / 127;
const PARAMETER_TRAVEL_PER_DETENT = 1 / 128;
const FINE_SECONDS = 0.1;
const COARSE_SECONDS = 1;
const COARSE_FROM_SECONDS = 10;
const LOWEST_SEMITONE = -52;
const SPEED_FIGURES = 4;

function grainFor(step) {
  if (!(step > 0) || step >= 1) return 1;
  return Math.pow(10, Math.floor(Math.log10(step)));
}

function keptToGrain(value, moved, steps, grain) {
  let next = roundedTo(moved, grain);
  if (steps !== 0 && Math.abs(next - value) < grain * 0.5) next = roundedTo(value + (steps > 0 ? grain : -grain), grain);
  return next;
}

function speedTurned(value, steps, low, high) {
  const size = Math.abs(value);
  const magnitude = size > 0 ? Math.max(0.5, 12 * Math.log2(size) - LOWEST_SEMITONE + 1) : 0;
  const at = value < 0 ? -magnitude : magnitude;
  const nearest = halfAway(at);
  let rung = nearest + steps;

  if (Math.abs(at - nearest) > 0.02) rung = (steps > 0 ? Math.ceil(at) : Math.floor(at)) + (steps > 0 ? steps - 1 : steps + 1);
  if (Math.abs(rung) < 0.5) return clamp(0, low, high);

  const speed = Math.pow(2, (Math.abs(rung) - 1 + LOWEST_SEMITONE) / 12);
  const scale = Math.pow(10, SPEED_FIGURES - 1 - Math.floor(Math.log10(speed)));
  return clamp((rung < 0 ? -1 : 1) * halfAway(speed * scale) / scale, low, high);
}

export function dialTurned(range, value, steps) {
  const low = Number.isFinite(range.min) ? range.min : -Infinity;
  const high = Number.isFinite(range.max) ? range.max : Infinity;

  if (steps === 0) return clamp(value, low, high);
  if (range.integer) return clamp(halfAway(value) + steps, low, high);

  if (range.unit === "Hz" && low > 0) {
    const moved = clamp(Math.max(value, low) * Math.pow(2, steps * OCTAVES_PER_DETENT), low, high);
    return clamp(roundedTo(moved, moved < 1000 ? 0.1 : 1), low, high);
  }

  if (range.unit === "dB") {
    if (low <= SILENCE_DB) {
      const at = clamp(fractionForDb(value) + steps * LEVEL_TRAVEL_PER_DETENT, 0, 1);
      if (!(at > 0)) return clamp(SILENCE_DB, low, high);
      return clamp(roundedTo(dbForFraction(at), 0.1), low, high);
    }

    return clamp(roundedTo(value + steps * GAIN_STEP_DB, 0.1), low, high);
  }

  if (range.unit === "s") {
    const upwards = steps > 0;
    let at = Math.max(value, 0);

    //  Detent by detent, so a turn across ten seconds changes its step there.
    for (let left = Math.abs(steps); left > 0; left--) {
      const coarse = upwards ? at >= COARSE_FROM_SECONDS - 1e-9 : at > COARSE_FROM_SECONDS + 1e-9;
      const step = coarse ? COARSE_SECONDS : FINE_SECONDS;
      at = roundedTo(at + (upwards ? step : -step), step);
    }

    return clamp(Math.max(at, 0), low, high);
  }

  if (range.unit === "x") return speedTurned(value, steps, low, high);

  if (low > 0 && high / low >= 50 && high < Infinity) {
    const moved = clamp(Math.max(value, low) * Math.pow(2, steps * WIDTH_DOUBLINGS_PER_DETENT), low, high);
    return clamp(roundedTo(moved, 0.01), low, high);
  }

  if (low > -Infinity && high < Infinity && high > low) {
    const step = (high - low) * PARAMETER_TRAVEL_PER_DETENT;
    return clamp(keptToGrain(value, value + steps * step, steps, grainFor(step)), low, high);
  }

  return clamp(keptToGrain(value, value + steps, steps, 0.001), low, high);
}

/*  ANY OTHER NUMBER DRAGGED: a detent of the dial for every `DETENT_PX` up,
    at the rate the finger's distance off the box gives, the part of a detent
    not yet reached carried to the next move - so a finger far off the box
    still gets there, slowly. */
export class DialDrag {
  constructor(range, start, detentPx = DETENT_PX, unitPx = NUMBER_UNIT_PX) {
    this.range = range;
    this.value = start;
    this.detent = detentPx;
    this.unit = unitPx;
    this.carried = 0;
    this.rate = 1;
  }

  move(along, across) {
    this.rate = rateFor(across, this.unit);
    this.carried += along / this.detent * this.rate;
    const steps = Math.trunc(this.carried);

    if (steps !== 0) {
      this.carried -= steps;
      this.value = dialTurned(this.range, this.value, steps);
    }

    return this.value;
  }
}

/** Whether a number's row is a LEVEL - a decibel row reaching silence - which drags along the fader. */
export function isLevel(range) {
  return range.unit === "dB" && Number.isFinite(range.min) && range.min <= SILENCE_DB;
}
