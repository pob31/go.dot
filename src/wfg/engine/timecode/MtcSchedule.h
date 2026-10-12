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
    What bytes MIDI Time Code sends, and at which sample (namespace draft §59,
    TC.2; AHG, AHO). The MTC lane's thread (TC.5) turns each due sample into a
    moment and sends what this says; nothing here knows a port or a clock, so
    the bytes and their places can be proven on their own.

    A FULL FRAME, `F0 7F 7F 01 01 hh mm ss ff F7`, says where the code is in
    one message: device 7F, every device (AHO), and the rate's code in the top
    of the hours byte. It is sent at a start, a jump, the hold's release and tx
    back on, so a desk chasing the code relocates at once instead of waiting
    two frames for a cycle.

    QUARTER FRAMES, `F1 (piece << 4 | nibble)`, carry the same label eight
    nibbles at a time, four a frame: frames, seconds, minutes and hours, low
    nibble then high, the last piece holding the rate's code beside the hours'
    top bit. A CYCLE of eight takes two frames. Each begins with piece 0 on the
    first sample of a frame and encodes THAT frame (AHO); a receiver adds the
    two frames the cycle took. So a run started on frame F has cycle `c` begin
    on frame `F + 2c` and say its label - `labelOf`, so the drop-frame names
    and the wrap at 24 hours come out as they do everywhere else.

    THE RATE CODES (AHG): 0 for 24, 1 for 25, 2 for 29.97 drop-frame, 3 for 30.
    23.976 has none of its own and is sent as 24, and 29.97 non-drop as 30, each
    at its own pace: the code names the labels' count, the timing their speed.

    WHEN: quarter `j` of a run is due on the first sample at or after its exact
    moment, `zero + ceil (j * sampleRate * den / (4 * num))` - FrameClock.h's
    one rule, so the fourth quarter after a frame's start is that frame's start
    to the sample, and the same sample LTC begins the frame on.

    PURE: the bytes come back in fixed arrays and the cursor is a value, so the
    lane's thread can build a message a quarter frame without allocating.
*/

#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <cstdint>

namespace wfg::timecode
{
    constexpr int mtcQuartersPerFrame = 4;
    constexpr int mtcPiecesPerCycle = 8;

    //  The longest message MTC sends here: a full frame.
    constexpr int mtcFullFrameSize = 10;
    constexpr int mtcQuarterFrameSize = 2;

    /*  The full frame for a label at a rate: `F0 7F 7F 01 01 hh mm ss ff F7`,
        `hh` being `(mtcCode << 5) | hours`. The label is normalised first. */
    std::array<std::uint8_t, mtcFullFrameSize> fullFrame (const Label& label, Rate rate) noexcept;

    /*  The low nibble quarter frame `piece` (0-7) carries for a label:
        0-1 the frames' low nibble and top bit, 2-3 the seconds', 4-5 the
        minutes', 6 the hours' low nibble, 7 `(mtcCode << 1) | hours' top bit`.
        The piece is taken modulo 8. */
    std::uint8_t quarterNibble (const Label& label, Rate rate, int piece) noexcept;

    //  The two bytes of a quarter frame: `F1`, then `(piece << 4) | nibble`.
    std::array<std::uint8_t, mtcQuarterFrameSize> quarterFrame (const Label& label, Rate rate, int piece) noexcept;

    /*  The sample quarter `quarter` of a run is due on, counted from the run's
        zero: `ceil (quarter * sampleRate * den / (4 * num))`. */
    std::int64_t quarterDueSample (std::int64_t quarter, std::int64_t sampleRate, Rate rate) noexcept;

    /*  ONE MESSAGE and when it is due: the sample, counted on the same clock
        as the cursor's zero, and the bytes, of which the first `size` are the
        message - two for a quarter frame, ten for a full frame. */
    struct MtcMessage
    {
        std::int64_t dueSample = 0;
        std::array<std::uint8_t, mtcFullFrameSize> bytes {};
        int size = 0;

        bool isFullFrame() const noexcept { return size == mtcFullFrameSize; }
    };

    /*  WHERE A RUN'S MTC HAS GOT TO: the next message and its sample, in
        order, from a start frame. A value with nothing behind it - the lane
        keeps one per run and copies it freely.

        `next` is what is due next and `advance` moves past it; the lane sends
        every message whose sample has come, in order. A new cursor sends a
        full frame first, at its first cycle's sample, so a start and a jump
        (a new cursor on the new frame) both begin by saying where the code
        is. `seek` re-seats it at the first cycle that begins at or after a
        sample - for the hold's release and tx back on, with a full frame
        first; for a quarter frame dropped for lateness, without one, the code
        simply picking up at the next cycle (AHQ). A cycle is never entered
        part way: a receiver given pieces from the middle of one would put
        together a label that never was. */
    class MtcCursor
    {
    public:
        MtcCursor() noexcept;

        /*  A run on `rate` at `sampleRate` that starts on frame `startFrame`
            (counted from 00:00:00:00 at the rate, as `frameOf` gives it) and
            whose first sample is `zeroSample`. */
        MtcCursor (Rate rate, std::int64_t sampleRate, std::int64_t startFrame, std::int64_t zeroSample) noexcept;

        //  The message due next, which `advance` moves past.
        MtcMessage next() const noexcept;
        void advance() noexcept;

        /*  Re-seats the cursor on the first cycle whose piece 0 is due at or
            after `sample` (never before the run's first), a full frame first
            if asked for. */
        void seek (std::int64_t sample, bool withFullFrame) noexcept;

        //  Whether the message due next is a full frame.
        bool fullFrameFirst() const noexcept { return fullFramePending; }

        //  The quarter due next, counted from the run's first (0, 1, 2, ...).
        std::int64_t quarter() const noexcept { return nextQuarter; }

        /*  The frame the current cycle encodes, counted from 00:00:00:00 and
            not wrapped (`startFrame + 2c`), and its label, which is. */
        std::int64_t cycleFrame() const noexcept;
        Label cycleLabel() const noexcept { return encoded; }

    private:
        Rate runRate = Rate::fps25;
        std::int64_t runSampleRate = 48000;
        std::int64_t firstFrame = 0;
        std::int64_t zero = 0;
        std::int64_t nextQuarter = 0;
        bool fullFramePending = true;
        Label encoded {};

        void seatOnQuarter (std::int64_t quarterIndex, bool withFullFrame) noexcept;
    };
}
