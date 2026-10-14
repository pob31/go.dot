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
    GO.DOT'S SAMPLE COUNT AGAINST THE STEADY CLOCK, AS A STRAIGHT LINE: THE
    COUNT'S LINE (namespace draft §60, TG.6; TG.11's faults, part 2).

    WHAT IT IS FOR. A source read on the steady clock - the wall clock, MIDI
    Time Code arriving on a cable - is placed on Go.dot's samples by asking
    which sample was playing at a steady moment. Nothing reads that directly:
    the count moves a block at a time, so a sample read at a moment is anything
    up to a block - 3 to 40 ms - behind it. What there is, every tick, is the
    tick's CLOCK PAIR, the count and the steady moment it was read at; this
    puts a straight line through the last `pairSeconds` of them by least
    squares (LineFit.h) and reads a moment's sample off it. Its slope is the
    interface's crystal against the steady clock, which does not change, so
    two minutes of pairs know it to a part per million and every sample read
    off it falls on one straight line - which is what a watcher fitting its own
    line through what it read needs, since every wander of this one would be a
    wander of that one.

    WHY NOT `ClockEstimate` (clock/ClockEstimate.h), which the video renderer
    and the MTC lane keep. It follows the pairs one by one: each moves its
    offset by a tenth of its error and its slope by a hundredth of the error
    over the time since the last pair, clamped within half a percent. Pairs a
    block apart in their noise walk that slope to its clamp (TG.6, measured at
    256 and 512 samples), and the offset rings with it by a millisecond and more
    for seconds - harmless a tick ahead, where the lane and the renderer use
    it, but MTC's watcher, fitting its line through samples read off it,
    re-anchored on every ring: 25 records a 75-second pass in TG.11's driver,
    on 128-sample blocks and with no tick noted behind, where LTC wrote eight;
    285 and 555 in ten minutes on 256- and 512-sample blocks in CodeWatchTests'
    cable, with or without the tick's stalls (§60.7, TG.11's faults, part 2).
    A tick thread that fell behind and runs the ticks it owes back to back, a
    third of a millisecond apart on one count, kicks that slope harder still,
    each pair's error over so short a time (reasoned, from the arithmetic). A
    late tick's pairs are no harm here: the count and the moment are read
    together, late or not, and a line through them is a line through points on
    it.

    A YOUNG LINE'S SLOPE SAYS LITTLE, AND A CRYSTAL'S IS KNOWN: two pairs 20 ms
    apart on the same block say the count stands still, and a line through a
    second of 512-sample blocks knows its slope to a part in a thousand -
    which, read through, is a code a part in a thousand fast for as long as it
    lasts, and a watcher re-anchoring on it (measured: four `running` in MTC's
    first four seconds). But an interface's crystal is within a hundred parts
    per million of what it says, and so is the steady clock's. So `sampleAt`
    reads a line through the pairs' middle at a slope weighed between the
    line's own and the nominal one (`priorPartsPerMillion` its error), each by
    how well it is known: the nominal alone for the first pairs, the line's
    own within a few seconds of small blocks or half a minute of large ones,
    and after a slip the slope the line had, as well as it was known - a slip
    moves the count, not the crystal. ClockWatch reads `line` itself and waits
    a second (`youngUntil`) before it reads anything at all, as it always has.

    A SLIP OF GO.DOT'S COUNT under the steady one - an interface that dropped a
    buffer and lost its samples, a count that paused or reset - is the last
    `slipPairs` pairs, together, further from the line than their scatter ever
    puts them, and at one distance from it; the line starts again from the
    pair that told it, and `feed` says so. A pair far off the line that is not
    yet a slip is not fitted through - a thread preempted between reading the
    count and reading the clock, or a count running LATE: an interface's
    callback or the hosted pump held back while the machine is loaded, which
    falls further behind with every block it does not deliver and then
    delivers them back to back. Such pairs string out over tens or hundreds of
    milliseconds and are no slip: the line stands where it was, which is where
    the count comes back to, and a moment read meanwhile is read where the
    crystal puts it. A count late at ONE distance for a while looks like a slip
    until it catches up, so a line may hold a slip before believing it
    (`lateHoldSeconds`, MTC's reader's), fitting nothing meanwhile; the wall
    clock's watcher believes one at once, as TG.6 measured it.

    PURE AND TICK-OWNED: no clock of its own, no JUCE, no thread. Its window
    allocates as it keeps two minutes of pairs - the tick thread's, never the
    audio thread's.
*/

#include <wfg/engine/timecode/LineFit.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace wfg::timecode
{
    class CountLine
    {
    public:
        /*  A line whose slips are told only once they have held `slipHoldSeconds`
            (nought: at once, the wall clock's watcher's, TG.6's as measured);
            MTC's reader holds them `lateHoldSeconds`. */
        explicit CountLine (double slipHoldSeconds = 0.0) noexcept
            : slipHold (static_cast<std::int64_t> (slipHoldSeconds * 1.0e9))
        {
        }

        /*  THE TICK'S CLOCK PAIR: Go.dot's sample `sample` was counted when the
            steady clock read `nanos`, at `rate` samples a second. True when
            this pair told a SLIP and the line started again from it. A rate
            changed forgets everything first: the samples counted something
            else. A pair with no rate or no count says nothing. */
        bool feed (std::int64_t sample, std::int64_t nanos, int rate);

        //  Everything forgotten, the slope it kept with it: the next pair is a first.
        void forget() noexcept;

        //  Whether it has a pair to read a sample off since it last started.
        bool known() const noexcept { return ! pairs.empty(); }

        /*  THE LINE least squares put through the pairs, its `sample` a steady
            millisecond and its `position` the count; invalid until two pairs,
            and young until `youngUntil`. */
        const linefit::Line& line() const noexcept { return fitted; }

        //  The steady nanoseconds until which the line is young: a second after its first pair.
        std::int64_t youngUntil() const noexcept { return quietUntil; }

        /*  GO.DOT'S SAMPLE AT A STEADY MOMENT, unrounded: on the line through
            the pairs' middle at the slope weighed between the line's and the
            nominal (or kept) one. Only when `known`. */
        double sampleAt (std::int64_t nanos) const noexcept;

        /*  AND BACK (namespace draft §58, RD.9): the steady moment, in
            nanoseconds, at which the count reaches `sample`, on the same line
            `sampleAt` reads - its inverse. Only when `known`. */
        double momentAt (double sample) const noexcept;

        /*  THE SLOPE `sampleAt` READS AT, samples a steady millisecond - the
            line's own weighed with the nominal or kept one - and how well it is
            known, the same units. */
        double samplesPerMillisecond (double& error) const noexcept { return slopeNow (error); }

        //  The steady millisecond of the newest pair fitted through; nought before one.
        double newestMillisecond() const noexcept { return pairs.empty() ? 0.0 : pairs.back().sample; }

        //======================================================================
        //  Two minutes of pairs, young for a second, fitted twice a second once it is not.
        static constexpr double pairSeconds = 120.0;
        static constexpr double quietSeconds = 1.0;
        static constexpr double fitSeconds = 0.5;

        /*  A SLIP: the last `slipPairs` pairs' mean distance from the line
            further than `slipSigmas` times what that mean's scatter is, and
            than `slipMilliseconds` - and the pairs within `slipWidths` times
            the line's scatter of each other, or `slipMilliseconds`: at one
            distance, not strung out by a count running late and catching up.
            A pair on its own further than `slipSigmas` times the line's
            scatter, and than `slipMilliseconds`, is not fitted through. */
        static constexpr std::size_t slipPairs = 8;
        static constexpr double slipSigmas = 6.0;
        static constexpr double slipMilliseconds = 3.0;
        static constexpr double slipWidths = 4.0;

        /*  HOW LONG A SLIP MUST HOLD BEFORE MTC'S READER BELIEVES IT: a count
            that is LATE at one distance - the hosted pump of a loaded machine
            running 70 ms behind its deadlines for half a second, measured -
            looks like a slip until it catches up, and a line started again
            from it, and again when it catches up, moves every moment read
            through it by as much, twice. Two seconds outlasts what was seen
            on a machine busy on every core (1.8 s); a count that really lost
            its samples has its moments read that much off for those two
            seconds, which a code's watcher says as a re-anchor or two. */
        static constexpr double lateHoldSeconds = 2.0;

        //  How far from nominal a crystal against the steady clock is believed to be, until pairs say better.
        static constexpr double priorPartsPerMillion = 100.0;

    private:
        void restart() noexcept;

        //  The slope `sampleAt` reads at, samples a steady millisecond, and how well it is known.
        double slopeNow (double& error) const noexcept;

        int rateFed = 0;

        /*  The pairs, each a steady millisecond (LineFit's `sample`) against
            the count read at it (its `position`); the line through them and
            the steady millisecond it was fitted at; until when it is young;
            and the last pairs' distances from it, in samples, and their steady
            milliseconds. */
        std::deque<linefit::Point> pairs;
        linefit::Line fitted;
        double fittedAt = 0.0;
        std::int64_t quietUntil = 0;
        std::array<double, slipPairs> recentOffs {};
        std::array<double, slipPairs> recentAt {};
        std::size_t recentNext = 0;
        std::size_t recentCount = 0;

        /*  How long a slip must hold; since when the pairs have said one (-1:
            they do not), and the steady millisecond the pairs that say it
            began at. Pairs are not fitted through while one is held. */
        std::int64_t slipHold = 0;
        std::int64_t slipSince = -1;
        double slipFrom = 0.0;

        /*  THE SLOPE A LINE STARTING AGAIN KEEPS, samples a steady
            millisecond, and how well it was known: the line's before a slip,
            nought for the nominal. */
        double keptSlope = 0.0;
        double keptError = 0.0;
    };
}
