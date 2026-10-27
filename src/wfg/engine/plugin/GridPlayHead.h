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
    THE PLAYHEAD A HOSTED PLUGIN READS (namespace draft §123, BEJ): what the
    child answers when a plugin asks its host where it is - the tempo at this
    block, the time signature, the beat and the bar, the time - read from the
    beat clock the parent put in the region's header and the block's first
    sample on the lane. Until this a hosted plugin had no playhead at all; a
    delay set in note lengths had nothing to go by.

    Here rather than in the child alone so a test can build one on a region
    laid out in a vector and ask it, with no process behind it.
*/

#include <wfg/engine/cue/BeatClock.h>
#include <wfg/engine/plugin/SharedRegion.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace wfg::plugin
{
    class GridPlayHead final : public juce::AudioPlayHead
    {
    public:
        GridPlayHead (const region::Header* headerToRead, const region::Lane* laneToRead, double sampleRateToUse) noexcept
            : header (headerToRead), lane (laneToRead), sampleRate (sampleRateToUse)
        {
        }

        /*  THE CLOCK AS THE PARENT LAST LEFT IT, read under its count: odd
            while the parent writes, moved when it wrote between the two reads;
            either way read again, a handful of times, and the last whole copy
            kept where the parent never holds still. */
        cue::BeatClockState clockNow() const noexcept
        {
            if (header == nullptr)
                return last;

            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const auto before = header->gridSeq.load (std::memory_order_acquire);

                if ((before & 1u) != 0)
                    continue;

                cue::BeatClockState read;
                read.anchorSample = static_cast<std::int64_t> (header->gridAnchorSample.load (std::memory_order_relaxed));
                read.clock.anchorBeat = header->gridAnchorBeat.load (std::memory_order_relaxed);
                read.clock.bpmFrom = header->gridBpmFrom.load (std::memory_order_relaxed);
                read.clock.bpmTo = header->gridBpmTo.load (std::memory_order_relaxed);
                read.clock.rampStart = header->gridRampStart.load (std::memory_order_relaxed);
                read.clock.rampLength = header->gridRampLength.load (std::memory_order_relaxed);
                read.clock.shape = header->gridShape.load (std::memory_order_relaxed);
                read.clock.beatsPerBar = static_cast<int> (header->gridBeatsPerBar.load (std::memory_order_relaxed));
                read.playing = header->gridPlaying.load (std::memory_order_relaxed) != 0;

                std::atomic_thread_fence (std::memory_order_acquire);

                if (header->gridSeq.load (std::memory_order_relaxed) == before)
                {
                    if (before == 0)
                        return last;    // never written: the clock as it starts

                    if (read.clock.beatsPerBar < 1)
                        read.clock.beatsPerBar = 4;

                    last = read;
                    return read;
                }
            }

            return last;
        }

        juce::Optional<PositionInfo> getPosition() const override
        {
            const auto state = clockNow();
            const auto blockStart = lane != nullptr
                                      ? static_cast<std::int64_t> (lane->blockStartSample.load (std::memory_order_relaxed))
                                      : std::int64_t (0);
            const auto rate = sampleRate > 0.0 ? sampleRate : 48000.0;
            const auto seconds = static_cast<double> (blockStart - state.anchorSample) / rate;
            const auto beat = state.clock.beatAt (seconds);
            const auto bar = std::max (1, state.clock.beatsPerBar);

            PositionInfo info;
            info.setBpm (state.clock.bpmAt (seconds));
            info.setTimeSignature (TimeSignature { bar, 4 });
            info.setPpqPosition (beat);
            info.setPpqPositionOfLastBarStart (state.clock.lastBarStartBeat (beat));
            info.setBarCount (static_cast<std::int64_t> (std::floor (beat / bar)));
            info.setTimeInSamples (blockStart);
            info.setTimeInSeconds (static_cast<double> (blockStart) / rate);
            info.setIsPlaying (state.playing);
            info.setIsLooping (false);
            info.setIsRecording (false);
            return info;
        }

        bool canControlTransport() override { return false; }

    private:
        const region::Header* header = nullptr;
        const region::Lane* lane = nullptr;
        double sampleRate = 0.0;
        mutable cue::BeatClockState last;
    };
}
