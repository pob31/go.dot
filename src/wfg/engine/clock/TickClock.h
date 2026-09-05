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
    Where the 50 Hz tick sits, in samples (PRD §3.4).

    Arithmetic only. It owns no thread, reads no clock and has no state that
    changes on its own - give it a sample position and it says which tick that
    is, give it a tick and it says which sample. TickThread does the waiting;
    this decides what there is to wait for.

    TICK n SITS AT SAMPLE n × samplesPerTick, exactly, and samplesPerTick is
    sampleRate / 50 with no remainder. A rate where that division is not exact
    is REFUSED rather than rounded: a tick at 882.02 samples would drift by a
    whole sample every fifty ticks, and a show that ran for an hour would end up
    3600 samples out from where its log says it was. Every rate anybody uses
    divides exactly - 22050, 44100, 48000, 88200, 96000, 192000 all do - so the
    refusal costs nothing real and buys exactness.

    THE INDEX AND THE POSITION ARE EXACT; THE MOMENT OF PROCESSING IS NOT. The
    tick thread can only observe the sample counter between blocks, so tick n is
    processed after the first block whose end reaches n × samplesPerTick. That
    is late by up to one block, plus however long the thread took to wake. What
    it is never wrong about is WHICH tick it is and WHERE that tick sits, and
    those are the two numbers the log records. Lateness is measured and
    reported, not hidden (see TickThread).

    REBASING, and why the counter underneath never resets. A sample rate can
    change under a running show - PRD §6.2's Dante clock domain moving is the
    case that actually happens - and timecode re-anchoring (§3.14) will want the
    same machinery later. Rebasing changes samples-per-tick FROM A GIVEN TICK
    ONWARDS, keeping every tick index before it exactly where it was. So the
    index sequence stays monotonic and gapless across the change, which is what
    the event log needs, and no arithmetic anywhere has to special-case it.

    Vendor-free.
*/

#include <cstdint>
#include <optional>

namespace wfg
{
    class TickClock
    {
    public:
        /** PRD §3.4's tick rate. Not a setting: the whole control plane's rate
            caps, the outbound coalescing and the parameter table's rate_cap
            column are all quoted against this one number. */
        static constexpr int rateHz = 50;

        /*  sampleRate / 50, or nullopt when that division has a remainder or
            the rate is not positive.

            The parameter is `rateInHz` rather than the obvious `sampleRate`
            because this class has a sampleRate() accessor, and GCC's -Wshadow
            objects to a parameter that shadows a member - including a member
            FUNCTION, and including inside a static one. MSVC says nothing, so
            it would have been a Linux-only build failure. */
        static std::optional<int> samplesPerTickFor (int rateInHz) noexcept;

        /** A clock with tick 0 at sample 0. nullopt for a rate 50 does not
            divide exactly. */
        static std::optional<TickClock> create (int rateInHz) noexcept;

        int sampleRate() const noexcept     { return currentRate; }
        int samplesPerTick() const noexcept { return currentSamplesPerTick; }

        //======================================================================
        /*  The sample tick `index` sits at.

            Meaningful for `index` at or after anchorTick(). Before it the
            answer would need the samples-per-tick that was in force back then,
            which this object no longer holds - a rebase deliberately keeps only
            what it needs to go forwards. Asking about an earlier tick returns
            the extrapolation under the CURRENT ratio, which is right when
            nothing has rebased and is otherwise only useful as an estimate. */
        std::int64_t sampleForTick (std::int64_t index) const noexcept;

        /*  The highest tick index whose sample position is at or before
            `samples` - that is, the last tick that has come due.

            Returns anchorTick() - 1 when `samples` is before the anchor, which
            reads as "no tick since the anchor has come due yet". */
        std::int64_t ticksReachedAt (std::int64_t samples) const noexcept;

        /** True when tick `index` has come due at sample position `samples`. */
        bool hasReached (std::int64_t index, std::int64_t samples) const noexcept
        {
            return samples >= sampleForTick (index);
        }

        //======================================================================
        /*  Changes the rate from tick `atTick` onwards, leaving every earlier
            tick where it is.

            False, and nothing changes, when the new rate does not divide by 50
            or when `atTick` is before the current anchor - rewriting where a
            tick already processed sat would put the log and the engine into
            disagreement about a moment that has already happened.

            Rebasing to the rate already in force is allowed and moves the
            anchor forward, which is how a caller re-anchors without changing
            anything (PRD §3.14's timecode case, later). */
        bool rebase (std::int64_t atTick, int newSampleRate) noexcept;

        /** The tick the current ratio starts from, and the sample it sits at.
            Both 0 until something rebases. */
        std::int64_t anchorTick() const noexcept   { return tickAnchor; }
        std::int64_t anchorSample() const noexcept { return sampleAnchor; }

    private:
        TickClock (int rate, int perTick) noexcept
            : currentRate (rate), currentSamplesPerTick (perTick) {}

        std::int64_t tickAnchor = 0;
        std::int64_t sampleAnchor = 0;
        int currentRate = 0;
        int currentSamplesPerTick = 0;
    };
}
