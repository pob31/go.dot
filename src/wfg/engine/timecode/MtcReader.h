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
    MIDI TIME CODE READ: what the incoming code says, and when it said it
    (namespace draft §60, TG.4; AIL, AIR).

    WHAT IT IS FED. Every quarter frame and full frame that came in on the
    show's `sync/mtcIn`, in the order they arrived, each with the moment it
    arrived on the steady clock (midi/MtcInbox.h takes them off the driver's
    thread and stamps them). What it gives back are OBSERVATIONS: this frame,
    at this moment, running forwards or backwards, at this rate - and the
    moment turned into one of Go.dot's samples. TG.5 fits a line through them;
    nothing here judges whether the code is running, stopped or jumped, which
    needs more than one cycle and is the watcher's.

    A CYCLE IS EIGHT QUARTER FRAMES, two frames long, and names ONE frame:
    the frame its piece 0 was sent on - MTC's convention, and the one Go.dot's
    own sender keeps (MtcSchedule.h, §59.7's TC.2: piece 0 leaves on the first
    sample of the frame it encodes, and every further piece a quarter frame
    after the one before). So once a cycle is whole the frame each of its
    pieces was sent on is known: piece `p` of a cycle naming frame F was sent
    `p` quarter frames into F - frame `F + p / 4`, quarter `p % 4` - which is
    why "the code has run on two frames by the time piece 7 arrives" (F plus
    seven quarters, the eighth still to run). Each piece becomes an observation
    of its own, with its own arrival: eight points a cycle for the line, not
    one, so the jitter of a MIDI driver averages out four times faster.

    NOTHING IS SAID UNTIL THE CYCLE IS WHOLE. The label is spread over all
    eight pieces, so a piece's frame is only known once piece 7 (forwards) has
    come, and a cycle with a piece missing is no label at all - pieces put
    together across a gap name a frame that never was. Such a cycle gives no
    observation, and the reader starts again at the next piece that can begin
    one; a code joined part way through a cycle is ignored until a whole cycle
    has passed. The cost is up to two frames of delay before a piece is
    reported, which carries its own arrival moment and so costs the line
    nothing; it is a locate that is heard late (AIR believes one after two
    consistent frames anyway).

    BACKWARDS, the pieces come 7, 6, ... 0, and a cycle begins at piece 7.
    The reader takes the mirror of the forward rule: piece `p` of a cycle
    naming F still stands at F plus `p` quarters - where it would be on a tape
    played forwards - so a code played backwards is read as the same frames
    in descending order, and the cycles name F, F - 2, F - 4. The MTC
    specification is not explicit about a reversed cycle's quarter, and
    senders differ by a quarter frame; code running backwards fires nothing
    (AIR), so what matters there is the direction, which this reports.

    A FULL FRAME, `F0 7F <device> 01 01 hh mm ss ff F7`, IS A LOCATE: the code
    is at this frame now, standing - no direction until quarter frames follow -
    and a cycle part-read is forgotten, since it belongs to before the jump.
    Any device number is read; Go.dot sends 7F, every device (AHO).

    THE RATE is the code in piece 7, or in a full frame's hours byte: 0 for
    24, 1 for 25, 2 for 29.97 drop-frame, 3 for 30. 23.976 and 29.97 non-drop
    have no code of their own and come as 24 and 30 - TG.5 tells them apart by
    the frames' period (AIL). So the frame is counted at the rate the code
    SAYS, from 00:00:00:00, which numbers 23.976 as 24 and 29.97 as 30 do
    alike; a label the code's rate does not have (hours past 23, frames past
    the rate, a name drop-frame leaves out) is no frame.

    FROM A MOMENT TO A SAMPLE through a clock line of the reader's own
    (clock/ClockEstimate.h), fed the very pair the tick hands the MTC lane
    each tick - where Go.dot's samples were and when - and never shared with
    another thread: one estimate a reader, as the video renderer and the lane
    each keep theirs.

    PURE AND TICK-OWNED: no JUCE, no clock of its own, no thread. It allocates
    only when `out` has to grow, which the caller avoids by reserving.
*/

#include <wfg/engine/clock/ClockEstimate.h>
#include <wfg/engine/timecode/Timecode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace wfg::timecode
{
    /*  ONE POINT ON THE INCOMING CODE: the piece (or full frame) that arrived
        at `arrivalNanos` was sent on `frame`, `quarter` quarter frames in. */
    struct MtcObservation
    {
        /*  The frame, counted from 00:00:00:00 at the rate `rateCode` names
            and wrapped into the day, as `frameOf` counts one. */
        std::int64_t frame = 0;

        /*  How many quarter frames into `frame` the piece was sent, 0 to 3;
            nought for a full frame. The code's position is `frame + quarter
            / 4` frames. */
        int quarter = 0;

        //  `frame`'s label, at the rate the code says.
        Label label {};

        //  0 for 24, 1 for 25, 2 for 29.97 drop-frame, 3 for 30.
        int rateCode = 0;

        //  Pieces came 7 to 0; never set on a full frame.
        bool reverse = false;

        //  A full frame: the code is here now, standing.
        bool locate = false;

        //  The steady clock when the message arrived, in nanoseconds.
        std::int64_t arrivalNanos = 0;

        //  Go.dot's sample at that moment, or -1 before the clock line has had a pair.
        std::int64_t sample = -1;
    };

    //  The rate MTC's rate code counts labels by: 24, 25, 29.97 drop-frame or 30.
    Rate rateOfMtcCode (int code) noexcept;

    class MtcReader
    {
    public:
        /*  A pair of the clock line: Go.dot's sample `sample` (at
            `sampleRate`) was playing when the steady clock read `nanos`. The
            same pair the tick hands the MTC lane, fed before each read. */
        void feedClock (std::int64_t sample, std::int64_t nanos, int sampleRate) noexcept
        {
            estimate.feed (sample, nanos, sampleRate);
        }

        /*  One message as it arrived, in order. What it completes - a whole
            cycle's eight pieces, or a full frame's locate - is appended to
            `out`; anything that is neither a quarter frame nor a full frame is
            ignored and breaks nothing, so a clock, a note or another SysEx
            between two pieces leaves the cycle whole. */
        void read (const std::uint8_t* bytes, std::size_t size, std::int64_t arrivalNanos,
                   std::vector<MtcObservation>& out);

        /*  A cycle part-read forgotten: the port changed, or stopped
            listening, so what comes next does not follow what came before. */
        void forget() noexcept;

        //  Whole cycles read and cycles thrown away for a gap, a duplicate or a turn, so far.
        std::uint64_t cyclesRead() const noexcept    { return whole; }
        std::uint64_t cyclesBroken() const noexcept  { return broken; }

        /*  Go.dot's sample at a moment of the steady clock, on the reader's own
            line, or -1 before it has had a pair: where a piece that completed
            nothing yet arrived, which keeps a code alive for TG.5's watcher
            while its cycle is being put together. */
        std::int64_t sampleAt (std::int64_t nanos) const noexcept { return estimate.sampleAt (nanos); }

        /*  A piece arriving this long after the one before it does not
            continue a cycle: the code stopped and started again between them,
            and a label put together across that pause would be two codes'
            pieces. The MTC lane's own stall (MtcLane::stallNanos), after which
            it re-seats with a full frame for the same reason. */
        static constexpr std::int64_t staleNanos = 250'000'000;

    private:
        void piece (int number, int nibble, std::int64_t arrivalNanos, std::vector<MtcObservation>& out);
        void fullFrame (const std::uint8_t* bytes, std::int64_t arrivalNanos, std::vector<MtcObservation>& out);

        //  The cycle is whole: its label read, its pieces said.
        void complete (std::vector<MtcObservation>& out);

        //  A cycle broken: counted when it had begun, and the reader looks for the next start.
        void drop() noexcept;

        enum class Direction { unknown, forward, reverse };

        ClockEstimate estimate;

        //  The cycle being read: each piece's nibble and arrival, how many, which way.
        std::array<int, 8> nibbles {};
        std::array<std::int64_t, 8> arrivals {};
        int count = 0;
        int firstPiece = -1;
        Direction direction = Direction::unknown;

        //  The piece before this one and when it came, whether or not a cycle was being read.
        int lastPiece = -1;
        std::int64_t lastArrival = 0;

        std::uint64_t whole = 0;
        std::uint64_t broken = 0;
    };
}
