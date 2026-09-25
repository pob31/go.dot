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

#pragma once

/*
    WHAT EACH KIND OF SURFACE HAS, AND WHAT ITS BUTTONS MEAN IN GO.DOT - the
    profile table of namespace draft §16.6, keyed by the word a show writes in
    `surface/profile`.

    PRD §3.16: a device profile is topology plus protocol. The protocol is
    McuCodec's bytes; this is the topology - how many strips a port carries,
    whether a strip has a motor, a touch sense, an encoder, a scribble strip,
    an RGB surround - and the one thing a protocol cannot say, which is what a
    button is FOR. The codec names buttons by the Mackie layout; this says
    that the V-Pot's press is a strip's gate and that PLAY is GO.

    A NEW SURFACE IS A NEW WORD HERE (§16.10): the Icon V1 and P1 arrive as
    rows in these tables, not as a second bridge.

    THE CONSTANTS THE BENCH AND THE ROOM WILL REVISE live here, each named
    once (plan decision 14): the motor's step, the colour rate and its idle
    re-assert, the encoder's detent and the double-STOP window. M27 and M28
    measure the two colour numbers on the unit.

    std only.
*/

#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/surface/McuCodec.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace wfg::surface
{
    enum class Profile { virtualPanel, mcu, d700, midiPads };

    /** The profile a show's word names: "virtual", "mcu", "d700" or
        "midiPads". Nothing for any other word. */
    std::optional<Profile> profileFor (std::string_view word);

    /** And the word, for a sentence about one. */
    std::string_view profileWord (Profile profile) noexcept;

    /*  WHAT A SURFACE OF ONE PROFILE PHYSICALLY HAS. */
    struct Topology
    {
        /*  The bank is the port (control guide §1.1): strip i of a Mackie
            surface is on port i / 8, element i % 8. Nought where strips are
            not banked by port at all - a pad controller addresses its strips
            by note, and the virtual panel has no port. */
        int stripsPerPort = 8;

        bool hasFaders = true;          // motor faders, fourteen bits
        bool hasTouch = true;           // a touch sense on each fader
        bool hasEncoders = true;        // V-Pots with rings
        bool hasDisplay = true;         // a scribble strip per strip
        bool hasRgb = false;            // d700: an RGB surround on each encoder
        bool nativeDisplay = false;     // d700: 12 + 12 + 8 and track numbers; mcu: two rows of 7 through 0x12
        bool hasPads = false;           // midiPads: notes with velocity and pressure
        bool drivenOverMidi = true;     // false for the virtual panel, which is the client's own

        /*  WHERE A LEVEL SITS ON THE TRAVEL: the law its engraving was drawn
            for, where somebody has measured it (FaderCurve.h). */
        FaderLaw faderLaw = FaderLaw::generic;
    };

    Topology topologyOf (Profile profile) noexcept;

    /*  What a hardware button on a Mackie surface does in Go.dot.

        THE STRIP'S GATE IS THE V-POT PRESS (plan decision 12), and SELECT is
        left alone: on the D700 an element's identity is its button note, so
        encoder three's press, ring and colour all key off one number, and the
        button that presses a strip is the one wearing its colour. On a dca
        strip the gate resets the DCA's trim to nought.

        THE TRANSPORT (§16.6): PLAY is `go`; STOP is `run.stopAll` and STOP
        again inside `doubleStopTicks` is `run.killAll` - PRD §4.4's first two
        levels under the hand already on the surface; rewind and forward move
        the standby. The bank and channel arrows do nothing, because banking is
        §3.9d's decision to take with the hardware in hand, and REC does
        nothing. */
    enum class Action { none, gate, go, stop, rewind, forward };

    /** What a button means on a surface of this profile. `none` for every
        button a profile does not use, and for every button of a profile that
        has none (the virtual panel, a pad controller). */
    Action actionFor (Profile profile, ButtonId button) noexcept;

    //==========================================================================
    //  The numbers the bench and the room revise, each in one place.

    /** An encoder detent moves the strip's target this far. */
    inline constexpr double encoderStepDb = 0.5;

    /*  A MOTOR MOVES AT MOST THIS FAR IN ONE TICK: a twentieth of its travel,
        so a flight from end to end takes twenty ticks, four tenths of a
        second. Driven the whole way in one message a fader hits its end stop at
        full speed, and the bottom is where a parked channel lives
        (docs/D700_CONTROL_GUIDE.md §4.1: never command full travel,
        interpolate over roughly twenty steps). */
    inline constexpr int motorStepPerTick = 819;

    /*  AT MOST THIS MANY COLOUR WRITES A SECOND TO ONE RGB ELEMENT (PRD §3.30:
        "no faster than about ten times a second"). M27 revises it on the
        unit. */
    inline constexpr double colourMaxPerSecond = 10.0;

    /*  The same limit in ticks between two writes to one element, rounded up
        so the rate is never exceeded: five at 50 Hz. */
    inline constexpr std::int64_t colourIntervalTicks = []
    {
        const auto ticks = static_cast<double> (TickClock::rateHz) / colourMaxPerSecond;
        const auto whole = static_cast<std::int64_t> (ticks);
        return static_cast<double> (whole) < ticks ? whole + 1
                                                   : (whole > 0 ? whole : std::int64_t { 1 });
    }();

    /*  AN UNCHANGED COLOUR IS WRITTEN AGAIN THIS OFTEN, because the D700's
        firmware takes its LEDs back with an idle animation when nothing
        drives them (control guide §4.4). Two seconds; M28 measures how soon
        the animation really returns. */
    inline constexpr std::int64_t idleColourReassertTicks = 2 * TickClock::rateHz;

    /*  TWO STOPS INSIDE THIS MANY TICKS ARE DOUBLE ESC: 750 ms at 50 Hz,
        rounded up, the second counting when it lands within the window. The
        desktop client makes the same reading of its Esc key in
        `src/wfg/client/model/Panic.h` (`Panic::doublePressMs`); the engine
        links no client, so the number is restated here and the two must move
        together. A press counts from the press before it, as it does there. */
    inline constexpr std::int64_t doubleStopTicks = (750 * TickClock::rateHz + 999) / 1000;
}
