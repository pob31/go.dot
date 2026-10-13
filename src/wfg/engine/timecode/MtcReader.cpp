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

#include <wfg/engine/timecode/MtcReader.h>

#include <wfg/engine/timecode/MtcSchedule.h>

namespace wfg::timecode
{
    namespace
    {
        constexpr std::uint8_t quarterFrameStatus = 0xF1;

        /*  A LABEL THE CODE'S RATE HAS, and nothing else: hours to 23, minutes
            and seconds to 59, frames below what the rate counts to, and at
            drop-frame none of the two names a dropping minute leaves out.
            `normalise` turns any label outside those into the next one there
            is - which is right for a label somebody typed (AHF) and wrong for
            one a machine sent: a sender saying a frame that does not exist is
            a sender to disbelieve, not to round up - so a label it changes is
            no label. The fields come from at most seven bits each, never
            negative, and none is wide enough to carry twice. */
        bool readable (const Label& label, Rate rate) noexcept
        {
            return normalise (label, rate) == label;
        }

        //  A seven-bit data byte, as every byte of a full frame between F0 and F7 must be.
        bool isData (std::uint8_t byte) noexcept { return byte < 0x80; }
    }

    Rate rateOfMtcCode (int code) noexcept
    {
        switch (code & 0x03)
        {
            case 0:  return Rate::fps24;
            case 1:  return Rate::fps25;
            case 2:  return Rate::fps2997df;
            default: return Rate::fps30;
        }
    }

    void MtcReader::read (const std::uint8_t* bytes, std::size_t size, std::int64_t arrivalNanos,
                          std::vector<MtcObservation>& out)
    {
        if (bytes == nullptr || size == 0)
            return;

        if (bytes[0] == quarterFrameStatus)
        {
            if (size == static_cast<std::size_t> (mtcQuarterFrameSize) && isData (bytes[1]))
                piece (bytes[1] >> 4, bytes[1] & 0x0F, arrivalNanos, out);

            return;
        }

        /*  `F0 7F <device> 01 01 hh mm ss ff F7`: a Universal Real Time
            message, MIDI Time Code, a full message. Any device number. Every
            other SysEx - the user bits (01 02), a desk's own - is ignored and
            leaves a cycle being read whole. */
        if (size == static_cast<std::size_t> (mtcFullFrameSize)
              && bytes[0] == 0xF0 && bytes[1] == 0x7F && isData (bytes[2])
              && bytes[3] == 0x01 && bytes[4] == 0x01
              && isData (bytes[5]) && isData (bytes[6]) && isData (bytes[7]) && isData (bytes[8])
              && bytes[9] == 0xF7)
            fullFrame (bytes, arrivalNanos, out);
    }

    void MtcReader::forget() noexcept
    {
        count = 0;
        firstPiece = -1;
        direction = Direction::unknown;
        lastPiece = -1;
        lastArrival = 0;
    }

    void MtcReader::drop() noexcept
    {
        /*  ONLY A CYCLE UNDER WAY IS COUNTED BROKEN. A lone piece 0 or 7 is a
            start that may not have been one - a reversed cycle ends on the
            piece 0 a forward one begins on - and the next piece says which. */
        if (count >= 2)
            ++broken;

        count = 0;
        firstPiece = -1;
        direction = Direction::unknown;
    }

    void MtcReader::piece (int number, int nibble, std::int64_t arrivalNanos, std::vector<MtcObservation>& out)
    {
        const auto index = static_cast<std::size_t> (number);

        /*  WHICH WAY FROM THE PIECE BEFORE: one on is forwards, one back (seven
            on, round the eight) is backwards, anything else - a repeat, a gap -
            continues nothing. And a piece long after the one before it does
            not continue a cycle whatever its number (`staleNanos`). */
        const auto stale = lastPiece >= 0 && arrivalNanos - lastArrival > staleNanos;
        const auto step = lastPiece >= 0 ? (number - lastPiece + 8) % 8 : -1;

        lastPiece = number;
        lastArrival = arrivalNanos;

        if (count > 0)
        {
            auto continues = false;

            if (! stale)
            {
                switch (direction)
                {
                    case Direction::forward:
                        continues = step == 1;
                        break;

                    case Direction::reverse:
                        continues = step == 7;
                        break;

                    case Direction::unknown:
                        /*  THE SECOND PIECE SAYS WHICH WAY: 0 then 1 is a cycle
                            forwards, 7 then 6 one backwards. 0 then 7 was the
                            end of a reversed cycle and the start of the next,
                            and 7 then 0 the end of a forward one and the start
                            of the next - each begun again below from this
                            piece. */
                        if (firstPiece == 0 && step == 1)
                        {
                            direction = Direction::forward;
                            continues = true;
                        }
                        else if (firstPiece == 7 && step == 7)
                        {
                            direction = Direction::reverse;
                            continues = true;
                        }
                        break;
                }
            }

            if (continues)
            {
                nibbles[index] = nibble;
                arrivals[index] = arrivalNanos;

                if (++count == mtcPiecesPerCycle)
                    complete (out);

                return;
            }

            drop();
        }

        /*  A CYCLE BEGINS ON PIECE 0 FORWARDS AND ON PIECE 7 BACKWARDS, and on
            nothing else: a code joined part way through a cycle is ignored
            until the next one begins, and a cycle a gap broke is never
            finished with the pieces after the gap. */
        if (number == 0 || number == mtcPiecesPerCycle - 1)
        {
            count = 1;
            firstPiece = number;
            direction = Direction::unknown;
            nibbles[index] = nibble;
            arrivals[index] = arrivalNanos;
        }
    }

    void MtcReader::complete (std::vector<MtcObservation>& out)
    {
        const auto reverse = direction == Direction::reverse;

        /*  THE LABEL, low nibble first: frames, seconds, minutes, hours, the
            rate's code beside the hours' top bit in piece 7. */
        Label label;
        label.frames  = nibbles[0] | ((nibbles[1] & 0x01) << 4);
        label.seconds = nibbles[2] | ((nibbles[3] & 0x03) << 4);
        label.minutes = nibbles[4] | ((nibbles[5] & 0x03) << 4);
        label.hours   = nibbles[6] | ((nibbles[7] & 0x01) << 4);

        const auto code = (nibbles[7] >> 1) & 0x03;
        const auto rate = rateOfMtcCode (code);

        count = 0;
        firstPiece = -1;
        direction = Direction::unknown;

        if (! readable (label, rate))
        {
            ++broken;
            return;
        }

        ++whole;

        const auto named = frameOf (label, rate);
        const auto day = framesPerDay (rate);

        /*  EACH PIECE WHERE IT WAS SENT: piece `p` a quarter frame `p` into
            the cycle's frame, in the order the pieces came - 0 to 7 forwards,
            7 to 0 backwards - so the observations run the way the code did. */
        for (int n = 0; n < mtcPiecesPerCycle; ++n)
        {
            const auto p = reverse ? mtcPiecesPerCycle - 1 - n : n;
            const auto at = static_cast<std::size_t> (p);

            MtcObservation seen;
            seen.frame = (named + p / mtcQuartersPerFrame) % day;
            seen.quarter = p % mtcQuartersPerFrame;
            seen.label = labelOf (seen.frame, rate);
            seen.rateCode = code;
            seen.reverse = reverse;
            seen.locate = false;
            seen.arrivalNanos = arrivals[at];
            seen.sample = sampleAt (arrivals[at]);
            out.push_back (seen);
        }
    }

    void MtcReader::fullFrame (const std::uint8_t* bytes, std::int64_t arrivalNanos, std::vector<MtcObservation>& out)
    {
        const auto code = (bytes[5] >> 5) & 0x03;
        const auto rate = rateOfMtcCode (code);

        Label label;
        label.hours = bytes[5] & 0x1F;
        label.minutes = bytes[6];
        label.seconds = bytes[7];
        label.frames = bytes[8];

        if (! readable (label, rate))
            return;

        /*  A LOCATE ENDS WHATEVER CYCLE WAS BEING READ: its pieces belong to
            before the jump, and pieces from after it begin a cycle of their
            own. Not counted broken - a locate is the code saying where it is,
            not a code failing to. */
        count = 0;
        firstPiece = -1;
        direction = Direction::unknown;
        lastPiece = -1;
        lastArrival = arrivalNanos;

        MtcObservation seen;
        seen.frame = frameOf (label, rate);
        seen.quarter = 0;
        seen.label = label;
        seen.rateCode = code;
        seen.reverse = false;
        seen.locate = true;
        seen.arrivalNanos = arrivalNanos;
        seen.sample = sampleAt (arrivalNanos);
        out.push_back (seen);
    }
}
