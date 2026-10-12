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

#include <wfg/engine/timecode/MtcSchedule.h>

#include <wfg/engine/timecode/FrameClock.h>

namespace wfg::timecode
{
    namespace
    {
        constexpr std::uint8_t quarterFrameStatus = 0xF1;

        //  Two frames a cycle: eight pieces at four a frame.
        constexpr std::int64_t framesPerCycle = mtcPiecesPerCycle / mtcQuartersPerFrame;

        /*  A piece's nibble for a label already normalised, and the rate's
            code: the one place the layout is written, so the cursor - which
            holds a label `labelOf` made - need not normalise it again a
            quarter frame. */
        std::uint8_t nibbleOf (const Label& named, int code, int piece) noexcept
        {
            int value = 0;

            switch (piece & 0x07)
            {
                case 0:  value = named.frames & 0x0F; break;
                case 1:  value = (named.frames >> 4) & 0x01; break;
                case 2:  value = named.seconds & 0x0F; break;
                case 3:  value = (named.seconds >> 4) & 0x03; break;
                case 4:  value = named.minutes & 0x0F; break;
                case 5:  value = (named.minutes >> 4) & 0x03; break;
                case 6:  value = named.hours & 0x0F; break;
                default: value = ((code & 0x03) << 1) | ((named.hours >> 4) & 0x01); break;
            }

            return static_cast<std::uint8_t> (value);
        }

        std::array<std::uint8_t, mtcQuarterFrameSize> quarterOf (const Label& named, int code, int piece) noexcept
        {
            return { quarterFrameStatus,
                     static_cast<std::uint8_t> (((piece & 0x07) << 4) | nibbleOf (named, code, piece)) };
        }

        std::array<std::uint8_t, mtcFullFrameSize> fullOf (const Label& named, int code) noexcept
        {
            /*  F0, then 7F for a Universal Real Time message, then the device
                - 7F, every device (AHO) - then 01 01 for MIDI Time Code's full
                message, the four numbers, and F7. Each a data byte, so kept to
                seven bits. */
            return { 0xF0, 0x7F, 0x7F, 0x01, 0x01,
                     static_cast<std::uint8_t> (((code & 0x03) << 5) | (named.hours & 0x1F)),
                     static_cast<std::uint8_t> (named.minutes & 0x3F),
                     static_cast<std::uint8_t> (named.seconds & 0x3F),
                     static_cast<std::uint8_t> (named.frames & 0x1F),
                     0xF7 };
        }
    }

    std::array<std::uint8_t, mtcFullFrameSize> fullFrame (const Label& label, Rate rate) noexcept
    {
        return fullOf (normalise (label, rate), mtcCodeOf (rate));
    }

    std::uint8_t quarterNibble (const Label& label, Rate rate, int piece) noexcept
    {
        return nibbleOf (normalise (label, rate), mtcCodeOf (rate), piece);
    }

    std::array<std::uint8_t, mtcQuarterFrameSize> quarterFrame (const Label& label, Rate rate, int piece) noexcept
    {
        return quarterOf (normalise (label, rate), mtcCodeOf (rate), piece);
    }

    std::int64_t quarterDueSample (std::int64_t quarter, std::int64_t sampleRate, Rate rate) noexcept
    {
        return partStartSample (quarter, mtcQuartersPerFrame, sampleRate, rate);
    }

    MtcCursor::MtcCursor() noexcept = default;

    MtcCursor::MtcCursor (Rate rate, std::int64_t sampleRate, std::int64_t startFrame, std::int64_t zeroSample) noexcept
        : runRate (rate), runSampleRate (sampleRate), firstFrame (startFrame), zero (zeroSample)
    {
        seatOnQuarter (0, true);
    }

    MtcMessage MtcCursor::next() const noexcept
    {
        MtcMessage message;

        /*  A full frame is only ever pending at a cycle's start, so it is due
            on the same sample as the piece 0 after it, and goes first. */
        message.dueSample = zero + quarterDueSample (nextQuarter, runSampleRate, runRate);

        if (fullFramePending)
        {
            message.bytes = fullOf (encoded, mtcCodeOf (runRate));
            message.size = mtcFullFrameSize;
            return message;
        }

        const auto piece = static_cast<int> (nextQuarter % mtcPiecesPerCycle);
        const auto quarterBytes = quarterOf (encoded, mtcCodeOf (runRate), piece);
        message.bytes[0] = quarterBytes[0];
        message.bytes[1] = quarterBytes[1];
        message.size = mtcQuarterFrameSize;
        return message;
    }

    void MtcCursor::advance() noexcept
    {
        if (fullFramePending)
        {
            fullFramePending = false;
            return;
        }

        ++nextQuarter;

        //  A new cycle encodes the frame it starts on, two on from the last.
        if (nextQuarter % mtcPiecesPerCycle == 0)
            encoded = labelOf (cycleFrame(), runRate);
    }

    void MtcCursor::seek (std::int64_t sample, bool withFullFrame) noexcept
    {
        /*  THE FIRST CYCLE AT OR AFTER THE SAMPLE: the first frame that begins
            there, rounded up to the even frame a cycle can begin on - cycles
            fall every two frames from the run's first, never in between, so a
            receiver adding two always lands on the frame being drawn. */
        auto frame = firstFrameAtOrAfter (sample - zero, runSampleRate, runRate);

        if (frame < 0)
            frame = 0;

        const auto cycle = (frame + framesPerCycle - 1) / framesPerCycle;
        seatOnQuarter (cycle * mtcPiecesPerCycle, withFullFrame);
    }

    std::int64_t MtcCursor::cycleFrame() const noexcept
    {
        return firstFrame + framesPerCycle * (nextQuarter / mtcPiecesPerCycle);
    }

    void MtcCursor::seatOnQuarter (std::int64_t quarterIndex, bool withFullFrame) noexcept
    {
        nextQuarter = quarterIndex;
        fullFramePending = withFullFrame;
        encoded = labelOf (cycleFrame(), runRate);
    }
}
