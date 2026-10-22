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
    ONE VOICE PLAYING A SLICE OF A LIVE TAKE (namespace draft §105, AZQ-AZT).

    A pad on the take is a media cue whose sound is a sampling channel's take
    rather than a file. A file is a clip in a launcher slot; a take is memory
    the channel's own track is still writing, which no clip can read, so the
    voice reads it here instead: at the head of its chain, ahead of its EQ, its
    plugins and its output stage, which then treat it as any sound.

    THREE THREADS, NONE WAITING. The message thread gives the voice its slice
    at the arm, while it is silent - the take, the in and the out in samples of
    the take, how many passes. The tick thread launches it at a sample of
    Go.dot's count, stops it at one, or kills it - each a store and a serial
    the audio thread reads once a block, so a stop or a kill can only ever
    land on the launch it was meant for. The audio thread plays it.

    WHAT IT PLAYS: `Looper::readLive` - the take and its closed layers, nothing
    past the record head (AZT). It rises over `edgeSamples` at its start and
    falls over them at the last pass's end and at a stop, so a slice cut into a
    waveform starts and ends without a click; a pass that is followed by
    another crossfades into it over `crossfadeSamples`, as the take's own loop
    does. Nought passes is for ever, as a range's `loops` is.

    `isPlaying` is true from the launch until the audio thread has played its
    last sample - the Runner's edge, which is how a run ends.

    Vendor-free and allocation-free: the adaptor Tracktion calls is
    TakeReaderPlugin.
*/

#include <wfg/engine/audio/Looper.h>
#include <wfg/engine/rt/RtCheck.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace wfg::audio
{
    class TakeVoice
    {
    public:
        static constexpr int edgeSamples = 96;          ///< two milliseconds at 48 kHz
        static constexpr int crossfadeSamples = 480;    ///< ten, the take's own wrap
        static constexpr int chunk = 256;

        TakeVoice() = default;
        TakeVoice (const TakeVoice&) = delete;
        TakeVoice& operator= (const TakeVoice&) = delete;

        //======================================================================
        /*  MESSAGE THREAD, at the arm, the voice silent: the take and the slice,
            in samples of the take. An out not after the in is the whole take as
            far as it goes. */
        void setSource (const Looper* take, std::int64_t in, std::int64_t out, int loops) noexcept
        {
            sliceIn.store (std::max<std::int64_t> (0, in), std::memory_order_relaxed);
            sliceOut.store (out > in ? out : std::numeric_limits<std::int64_t>::max(), std::memory_order_relaxed);
            passes.store (std::max (0, loops), std::memory_order_relaxed);
            source.store (take, std::memory_order_release);
        }

        /*  And none: the voice plays a file again. */
        void clearSource() noexcept                     { source.store (nullptr, std::memory_order_release); }

        /** Whether the voice plays a take rather than its clips. Any thread. */
        bool hasSource() const noexcept                 { return source.load (std::memory_order_acquire) != nullptr; }

        //======================================================================
        /*  TICK THREAD. A launch at a sample of Go.dot's count; a stop at one,
            or at the next block for -1; a kill, silence at the next block. */
        void launchAt (std::int64_t sample) noexcept
        {
            launchSample.store (sample, std::memory_order_relaxed);
            stopSample.store (std::numeric_limits<std::int64_t>::max(), std::memory_order_relaxed);
            launchSerial.fetch_add (1, std::memory_order_release);
        }

        void stopAt (std::int64_t sample) noexcept
        {
            stopSample.store (sample < 0 ? std::numeric_limits<std::int64_t>::min() : sample,
                              std::memory_order_release);
        }

        void kill() noexcept
        {
            killSerial.store (launchSerial.load (std::memory_order_relaxed), std::memory_order_release);
        }

        /** From the launch until its last sample has been played. Any thread. */
        bool isPlaying() const noexcept
        {
            return launchSerial.load (std::memory_order_acquire) != finishedSerial.load (std::memory_order_acquire);
        }

        //======================================================================
        /*  THE AUDIO THREAD: the slice added into `left` and `right` for the
            block that starts at `blockStart` - or, on a voice one channel wide,
            into `left` alone with a null `right`, the two sides at half each
            (namespace draft §42's fold). */
        void process (float* left, float* right, int frames, std::int64_t blockStart) noexcept WFG_AUDIO_THREAD
        {
            const auto serial = launchSerial.load (std::memory_order_acquire);

            if (serial != seen)
            {
                seen = serial;
                state = State::waiting;
                startSample = launchSample.load (std::memory_order_relaxed);
                stopping = false;
                passesDone = 0;
            }

            if (state == State::idle)
                return;

            const auto* take = source.load (std::memory_order_acquire);

            if (take == nullptr || killSerial.load (std::memory_order_acquire) == seen)
            {
                finish();
                return;
            }

            const auto in = sliceIn.load (std::memory_order_relaxed);
            const auto loops = passes.load (std::memory_order_relaxed);
            const auto stop = stopSample.load (std::memory_order_acquire);

            auto n = 0;

            while (n < frames && state != State::idle)
            {
                const auto sample = blockStart + n;

                if (state == State::waiting)
                {
                    //  Stopped before it began: nothing sounded, and nothing will.
                    if (sample >= stop)
                    {
                        finish();
                        break;
                    }

                    if (startSample > sample)
                    {
                        n += static_cast<int> (std::min ({ static_cast<std::int64_t> (frames - n),
                                                           startSample - sample, stop - sample }));
                        continue;
                    }

                    /*  THE WHOLE TAKE, AS FAR AS IT WENT AT THE LAUNCH, for a
                        slice with no out: a pad does not chase a take still
                        recording. */
                    const auto asked = sliceOut.load (std::memory_order_relaxed);
                    endAt = asked != std::numeric_limits<std::int64_t>::max()
                              ? asked : std::max (in + 1, take->readLive (0, 0, nullptr, nullptr));

                    state = State::playing;
                    position = in;
                    rise = 0;
                }

                const auto sliceLength = endAt - in;

                if (! stopping && sample >= stop)
                {
                    stopping = true;
                    fall = edgeSamples;
                }

                //  THE PASS'S END: another pass from the in point, past the crossfade it began with; or done.
                if (position >= endAt)
                {
                    ++passesDone;

                    if (loops != 0 && passesDone >= loops)
                    {
                        finish();
                        break;
                    }

                    position = in + (sliceLength > 2 * crossfadeSamples ? crossfadeSamples : 0);
                    continue;
                }

                const auto last = loops != 0 && passesDone + 1 >= loops;
                const auto wraps = ! last && sliceLength > 2 * crossfadeSamples;
                const auto fadeFrom = endAt - (wraps ? crossfadeSamples : last ? edgeSamples : 0);

                //  As far as the next thing that changes: the block's end, the pass's end, a fade's start, a chunk.
                auto span = std::min<std::int64_t> (frames - n, chunk);
                span = std::min (span, endAt - position);

                if (position < fadeFrom)
                    span = std::min (span, fadeFrom - position);

                if (! stopping && stop > sample)
                    span = std::min (span, stop - sample);

                const auto count = static_cast<int> (std::max<std::int64_t> (1, span));
                take->readLive (position, count, readLeft.data(), readRight.data());

                if (wraps && position >= fadeFrom)
                    take->readLive (in + (position - fadeFrom), count, nextLeft.data(), nextRight.data());

                for (int i = 0; i < count; ++i)
                {
                    auto a = readLeft[static_cast<std::size_t> (i)];
                    auto b = readRight[static_cast<std::size_t> (i)];
                    const auto at = position + i;

                    /*  INTO THE NEXT PASS, equal-power, inside the slice: the end
                        falls as the in point's first samples rise. */
                    if (wraps && at >= fadeFrom)
                    {
                        const auto t = (static_cast<double> (at - fadeFrom) + 0.5) / crossfadeSamples;
                        const auto down = static_cast<float> (std::cos (halfPi * t));
                        const auto up = static_cast<float> (std::sin (halfPi * t));
                        a = a * down + nextLeft[static_cast<std::size_t> (i)] * up;
                        b = b * down + nextRight[static_cast<std::size_t> (i)] * up;
                    }

                    auto gain = 1.0f;

                    if (rise < edgeSamples)
                        gain *= static_cast<float> (++rise) / static_cast<float> (edgeSamples);

                    if (last && at >= endAt - edgeSamples)
                        gain *= static_cast<float> (endAt - at) / static_cast<float> (edgeSamples);

                    if (stopping)
                    {
                        if (fall <= 0)
                        {
                            finish();
                            break;
                        }

                        gain *= static_cast<float> (fall--) / static_cast<float> (edgeSamples);
                    }

                    if (right != nullptr)
                    {
                        left[n + i] += a * gain;
                        right[n + i] += b * gain;
                    }
                    else
                    {
                        left[n + i] += (a + b) * 0.5f * gain;
                    }
                }

                if (state == State::idle)
                    break;

                position += count;
                n += count;
            }
        }

    private:
        enum class State { idle, waiting, playing };

        static constexpr double halfPi = 1.57079632679489661923;

        void finish() noexcept
        {
            state = State::idle;
            finishedSerial.store (seen, std::memory_order_release);
        }

        //  The slice, the message thread's.
        std::atomic<const Looper*> source { nullptr };
        std::atomic<std::int64_t> sliceIn { 0 }, sliceOut { 0 };
        std::atomic<int> passes { 1 };

        //  The tick thread's.
        std::atomic<std::int64_t> launchSample { 0 };
        std::atomic<std::int64_t> stopSample { std::numeric_limits<std::int64_t>::max() };
        std::atomic<std::uint32_t> launchSerial { 0 };
        std::atomic<std::uint32_t> killSerial { 0 };

        //  The audio thread's, and what it tells the others.
        std::atomic<std::uint32_t> finishedSerial { 0 };
        std::uint32_t seen = 0;
        State state = State::idle;
        std::int64_t startSample = 0;
        std::int64_t position = 0;
        std::int64_t endAt = 0;         ///< the slice's out, fixed at the launch
        int passesDone = 0;
        int rise = 0;
        int fall = 0;
        bool stopping = false;

        std::array<float, chunk> readLeft {}, readRight {}, nextLeft {}, nextRight {};
    };
}
