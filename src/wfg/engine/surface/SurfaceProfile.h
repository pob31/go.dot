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

#include <array>
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
        bool hasMeters = false;         // mcu, d700: a meter per strip, as channel pressure
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
        §3.9d's decision to take with the hardware in hand, and the
        transport's REC does nothing.

        MUTE ON A STRIP KILLS WHAT IT PLAYS (author, 2026-09-25: "Can the mute
        switch of a sampler fader be a kill switch for it? Not temporary
        muting, kill as the X in the active cue list panel."): `run.kill` on
        the run holding the strip, while something sounds on it - the running
        pane's cross, under the hand already on the surface.

        SOLO ON A STRIP LOCKS ITS BANK TO IT (author, 2026-09-25: "The solo
        switch could be engaged on a track to prevent other faders in the bank
        to trigger. The solo switch blink before the sample is triggered.
        Stays on while it plays and is turned off once the sample has finished
        playing or is stopped."): `run.solo` on the run holding the strip, as
        a toggle. Its light flashes while the soloed clip waits for its start,
        is lit while it sounds, and goes out with the solo, which the engine
        lets go of when the clip stops.

        REC ON A STRIP SETS WHERE ITS FADER STARTS (author, 2026-09-25:
        "Pressing Rec on a sampler fader sets the starting level. Confirm with
        a LED pulse."): the level the fader is at, written as its member's
        `initialLevel` - an edit to the show and one undo step. */
    enum class Action { none, gate, go, stop, rewind, forward, kill, solo, startLevel };

    /*  AND IT SAYS SO: the red MUTE light is on for half a second after a
        kill it sent (author, 2026-09-25: "Can you flash for 0.5s the red mute
        switch to have feedback on the killed sample?"). A press with nothing
        to kill lights nothing - which is its own answer. */
    inline constexpr std::int64_t killFlashTicks = TickClock::rateHz / 2;

    /*  AND REC SAYS SO THE SAME WAY: lit for half a second after it wrote the
        starting level ("Confirm with a LED pulse"). A press that wrote nothing
        - a free strip, a dca strip, a locked show - lights nothing. */
    inline constexpr std::int64_t startLevelFlashTicks = TickClock::rateHz / 2;

    /*  A FLASHING LIGHT IS BLINKED BY THE BRIDGE, a quarter of a second on and
        a quarter off: the D700 takes a light as on or off and nothing else
        (control guide §4.2), so MCU's own flash - a velocity of one - lit it
        steadily, and a solo waiting for its clip looked like one already
        sounding (author, 2026-09-25: "Solo could be blinking before the
        sample is started to show which one should be triggered"). */
    inline constexpr std::int64_t blinkHalfTicks = TickClock::rateHz / 4;

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

    /*  A FADER LET GO IS SENT ITS LEVEL AGAIN, `motorReasserts` times, this
        many ticks apart (author, 2026-09-25: "After using the Rec track
        button, the fader reverts to the old initial level and jumps to the new
        one when triggered"). A D700 puts a released fader back where the host
        last put it, and the one position sent the moment the hand lifts can
        reach it too soon to count: the fader went back to the level of an
        earlier ride, -0.6 dB where the hand had left it at -10.7. */
    inline constexpr std::int64_t motorReassertTicks = 10;
    inline constexpr int motorReasserts = 3;

    /*  WHERE AN UNTOUCHED FADER SAYS IT IS counts once it is further than this
        from where it was sent: the motor goes back there. Closer is the motor
        settling, and chasing it would keep it twitching. */
    inline constexpr int motorSlack = 64;

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

    /*  HOW BRIGHT A SOUNDING STRIP IS: HALF ITS LEVEL, HALF ITS MOVEMENT
        (author, 2026-09-25, in three steps: "Can the brightness of the RGB LEDs
        be modulated by the sound level or variations of it? ... Don't use the
        full 16 or 24 bit resolution. It can be squashed, but
        variation/modulation is a better clue"; then "at louder volume the
        modulation gets a bit lost ... Could the system be a bit more
        adaptive?"; then "The low level sounds with a little variation come
        out with as much variation in the lights as a more dynamic sound.
        Maybe make part of the LED level match the long term level of the
        music and the other 'half' the shorter term variations").

        THE LEVEL: the clip's envelope (its analysed peak, in dB below full
        scale) averaged over `pulseLevelSeconds`, squashed between
        `pulseLevelFloorDb` (nothing) and `pulseLevelCeilingDb` (all of its
        share) - quiet material glows low, loud material high.

        THE MOVEMENT: the envelope's height above a faster average (over
        `pulseAverageSeconds`), against how far it has strayed lately (a mean
        over `pulseSpreadSeconds`) times `pulseSpreadsForFull` - so a dense
        passage that moves a decibel or two still shows it - but never less
        than `pulseScaleLeastDb`, so a quiet sound that barely moves stays
        nearly still. Steady is the middle of its share, a hit the top, a dip
        the bottom.

        `pulseLevelShare` of the light is the level's and the rest the
        movement's, above `pulseFloor`, which is a glow and never dark (dark is
        silence). A flash is held and let go by `pulseReleasePerTick`, so the
        colour's own rate limit, ten writes a second, cannot skip it. These
        are the eye's brightness; what the LEDs are sent is shaped after, by
        the `led` numbers below. */
    inline constexpr double pulseFloor = 0.2;
    inline constexpr double pulseLevelShare = 0.5;
    inline constexpr double pulseLevelSeconds = 3.0;
    inline constexpr double pulseLevelFloorDb = -48.0;
    inline constexpr double pulseLevelCeilingDb = -6.0;
    inline constexpr double pulseAverageSeconds = 0.6;
    inline constexpr double pulseReleasePerTick = 0.04;
    inline constexpr double pulseSpreadSeconds = 1.5;
    inline constexpr double pulseSpreadsForFull = 1.5;
    inline constexpr double pulseScaleLeastDb = 4.0;
    inline constexpr double pulseScaleMostDb = 12.0;

    /*  WHAT AN RGB SURFACE'S LEDS ARE SENT for a colour the eye should see
        (author, 2026-09-25: "The white 'looks' louder. I think the LED's of
        the D700 are not super linear and not all channels match totally").
        The total light is held to `ledLightBudget` channels' worth - white
        lights all three and was three times a pure red; each channel has its
        trim, for LEDs that do not match; and the LEDs' response is
        straightened by `ledGamma`, since what they are sent is light and what
        the colour describes is how it looks. */
    inline constexpr double ledLightBudget = 1.5;
    inline constexpr double ledGamma = 2.0;
    inline constexpr double ledRedTrim = 1.0;
    inline constexpr double ledGreenTrim = 1.0;
    inline constexpr double ledBlueTrim = 1.0;

    /*  A SAMPLER STRIP'S METER, AFTER THE FADER (author, 2026-09-25: "On the
        sampler fader displays of the D700 can we have a post fader level
        meter too?"): the held run's `meter` - what left its track after the
        EQ, the inserts and the fader - the loudest of the ticks since the
        last message, as MCU's channel pressure (control guide §4.7).

        Sent every `meterEveryTicks` while the strip sounds, even unchanged,
        since a Mackie meter falls by itself between messages: every third
        tick is 16.7 a second, where the guide found 18 smooth and Asparion's
        own default is 5. Dark once, when it stops.

        `meterStepsDb` is where each of the D700's eleven lit steps begins,
        bottom to top: the n-th reached lights step n, and nothing below the
        first. A bench guess on the pattern desks use, closer together near
        the top, where headroom is read. */
    inline constexpr int meterEveryTicks = 3;
    inline constexpr std::array<double, 11> meterStepsDb { { -60.0, -50.0, -40.0, -30.0, -24.0, -18.0,
                                                             -12.0, -9.0, -6.0, -3.0, -1.0 } };

    /** The step a peak in dB lights, 0 to 11. */
    int meterStepFor (double db) noexcept;

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
