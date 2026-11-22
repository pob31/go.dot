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
    Go.dot's own dynamics stage, as a value (namespace draft §141, BHM-BHO):
    a compressor, a gate and a filter on what their detector hears, on every
    sound cue, mic channel and aux, after the EQ and before the inserts (BHU).

    WHAT IS HERE AND WHAT IS NOT. The numbers the author chose to have (BHO):
    the compressor's threshold, ratio, attack, release, knee and make-up; the
    gate's threshold, range, attack, hold and release; the key filter's
    high-pass and low-pass; and whether the detector reads peaks or RMS (the
    author's question of 2026-11-22: "Can the detection have a toggle to switch
    between peak and RMS?" - peak by default). What is fixed, and mine to flag
    (§141): the gate is spatcore's expander at a ratio of
    a hundred, which spatcore calls a gate; one filter pair shapes what both
    stages hear; nothing delays the audio (no lookahead, no reported latency).
    Where the key comes from is not
    a number and is not here: the Runner resolves it, as an insert's (§141).

    NAMES NO JUCE, TRACKTION OR SPATCORE TYPE, for EqSettings's reasons: it
    crosses from the tick thread to the audio side and the client reads it.

    OFF BY CONSTRUCTION. A default-constructed DynSettings is off, and off is
    not called at all - the cue the same to the bit (BHU). PRD §4.6: this is the
    resting state, and a reset writes exactly this.

    EQUALITY IS BITWISE on the floats, as EqSettings's.
*/

#include <bit>
#include <cstdint>

namespace wfg::audio
{
    struct DynSettings
    {
        bool on = false;

        //  What the detector reads: the peak of each sample, or RMS over ten milliseconds (spatcore's window).
        bool rms = false;

        //  The compressor.
        bool compOn = true;
        float thresholdDb = -20.0f;     // -60 .. 0
        float ratio = 4.0f;             // 1 .. 100
        float attackMs = 10.0f;         // 0.05 .. 200
        float releaseMs = 150.0f;       // 5 .. 2000
        float kneeDb = 6.0f;            // 0 .. 24
        float makeupDb = 0.0f;          // -24 .. 24

        //  The gate: spatcore's expander at gateRatio.
        bool gateOn = false;
        float gateThresholdDb = -50.0f; // -90 .. 0
        float gateRangeDb = -40.0f;     // -80 .. 0
        float gateAttackMs = 1.0f;      // 0.05 .. 200
        float gateHoldMs = 50.0f;       // 0 .. 500
        float gateReleaseMs = 150.0f;   // 5 .. 2000

        //  What the detector hears, filtered: a high-pass and a low-pass.
        float keyLowCutHz = 20.0f;      // 20 .. 2000
        float keyHighCutHz = 20000.0f;  // 1000 .. 20000, held under the rate's Nyquist by spatcore

        static constexpr float gateRatio = 100.0f;

        /** The resting state: off. */
        static DynSettings off() noexcept { return {}; }

        /** Whether this stage would change a signal at all. */
        bool isIdentity() const noexcept
        {
            return ! on || (! compOn && ! gateOn && sameFloat (makeupDb, 0.0f));
        }

        bool sameAs (const DynSettings& o) const noexcept
        {
            return on == o.on && rms == o.rms && compOn == o.compOn && gateOn == o.gateOn
                && sameFloat (thresholdDb, o.thresholdDb) && sameFloat (ratio, o.ratio)
                && sameFloat (attackMs, o.attackMs) && sameFloat (releaseMs, o.releaseMs)
                && sameFloat (kneeDb, o.kneeDb) && sameFloat (makeupDb, o.makeupDb)
                && sameFloat (gateThresholdDb, o.gateThresholdDb) && sameFloat (gateRangeDb, o.gateRangeDb)
                && sameFloat (gateAttackMs, o.gateAttackMs) && sameFloat (gateHoldMs, o.gateHoldMs)
                && sameFloat (gateReleaseMs, o.gateReleaseMs)
                && sameFloat (keyLowCutHz, o.keyLowCutHz) && sameFloat (keyHighCutHz, o.keyHighCutHz);
        }

        static bool sameFloat (float a, float b) noexcept
        {
            return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b);
        }
    };
}
