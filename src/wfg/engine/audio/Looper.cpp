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

#include <wfg/engine/audio/Looper.h>

#include <algorithm>
#include <cmath>

namespace wfg::audio
{
    namespace
    {
        constexpr double halfPi = 1.57079632679489661923;
    }

    //==============================================================================
    void Looper::prepare (const Shape& wanted)
    {
        current = wanted;
        const auto rate = wanted.sampleRate > 0.0 ? wanted.sampleRate : 48000.0;

        channelsHeld = std::clamp (wanted.channels, 1, maxChannels);
        slots = 1 + std::clamp (wanted.layers, 0, maxLayers);
        fade = std::max (1, static_cast<int> (std::lround (crossfadeSeconds * rate)));

        /*  A TAKE IS AT LEAST A LOOP - two crossfades - and its slot holds one
            crossfade more than the longest take, for what is recorded past its
            end (the post-roll a wrap fades out through), in whole chunks. */
        const auto asked = static_cast<std::int64_t> (std::llround (std::max (0.0, wanted.takeSeconds) * rate));
        takeSamples = asked > 0 ? std::max<std::int64_t> (asked, 2 * static_cast<std::int64_t> (fade)) : 0;
        chunks = takeSamples > 0 ? (takeSamples + fade + peakSamples - 1) / peakSamples : 0;
        room = chunks * peakSamples;

        /*  SET ASIDE AND TOUCHED: `assign` writes every sample, so every page is
            in memory before the audio thread writes its first one. */
        memory.assign (static_cast<std::size_t> (slots) * static_cast<std::size_t> (channelsHeld)
                           * static_cast<std::size_t> (room),
                       0.0f);

        const auto stampCount = static_cast<std::size_t> (slots) * static_cast<std::size_t> (chunks);
        stamps = std::make_unique<std::atomic<std::uint32_t>[]> (std::max<std::size_t> (1, stampCount));
        peaks = std::make_unique<std::atomic<float>[]> (std::max<std::size_t> (1, stampCount));

        for (std::size_t i = 0; i < std::max<std::size_t> (1, stampCount); ++i)
        {
            stamps[i].store (0, std::memory_order_relaxed);
            peaks[i].store (0.0f, std::memory_order_relaxed);
        }

        //  Every chunk stamped nought, every slot at generation one: all silence.
        for (auto& each : generation)
            each.store (1, std::memory_order_relaxed);

        fadeUp.resize (static_cast<std::size_t> (fade));
        fadeDown.resize (static_cast<std::size_t> (fade));

        for (int t = 0; t < fade; ++t)
        {
            const auto angle = halfPi * (static_cast<double> (t) + 0.5) / static_cast<double> (fade);
            fadeUp.data()[t] = static_cast<float> (std::sin (angle));
            fadeDown.data()[t] = static_cast<float> (std::cos (angle));
        }

        inboxHead.store (0, std::memory_order_relaxed);
        inboxTail.store (0, std::memory_order_relaxed);
        outboxHead.store (0, std::memory_order_relaxed);
        outboxTail.store (0, std::memory_order_relaxed);

        now = TakeState::empty;
        written = takeLength = 0;
        closeWanted = false;
        closeInto = TakeState::looping;
        closedLayers = laying = passAt = tailing = tailAt = 0;
        postRoll = postRollAt = 0;
        position = in = out = 0;
        crossfade = -1;
        fadingFrom = 0;
        playing = rampingUp = rampingDown = clearAfterStop = false;
        rampAt = 0;
        dropping = dropAt = 0;
        publish();
    }

    std::size_t Looper::bytes() const noexcept
    {
        return memory.size() * sizeof (float)
                 + static_cast<std::size_t> (slots) * static_cast<std::size_t> (chunks)
                       * (sizeof (std::uint32_t) + sizeof (float));
    }

    //==============================================================================
    bool Looper::post (const Command& command) noexcept
    {
        const auto tail = inboxTail.load (std::memory_order_relaxed);

        if (tail - inboxHead.load (std::memory_order_acquire) >= static_cast<std::uint32_t> (inboxSize))
            return false;

        inbox[tail % static_cast<std::uint32_t> (inboxSize)] = command;
        inboxTail.store (tail + 1, std::memory_order_release);
        return true;
    }

    void Looper::setThrough (bool shouldHearInput) noexcept
    {
        through.store (shouldHearInput, std::memory_order_relaxed);
    }

    bool Looper::isThrough() const noexcept
    {
        return through.load (std::memory_order_relaxed);
    }

    bool Looper::nextEvent (Event& event) noexcept
    {
        const auto head = outboxHead.load (std::memory_order_relaxed);

        if (head == outboxTail.load (std::memory_order_acquire))
            return false;

        event = outbox[head % static_cast<std::uint32_t> (outboxSize)];
        outboxHead.store (head + 1, std::memory_order_release);
        return true;
    }

    void Looper::tell (Event::Kind kind, std::int64_t sample, std::int64_t samples, TakeState into) noexcept
    {
        const auto tail = outboxTail.load (std::memory_order_relaxed);

        //  Nobody reading, sixty-four behind: dropped rather than waited for.
        if (tail - outboxHead.load (std::memory_order_acquire) >= static_cast<std::uint32_t> (outboxSize))
            return;

        outbox[tail % static_cast<std::uint32_t> (outboxSize)] = Event { kind, sample, samples, into };
        outboxTail.store (tail + 1, std::memory_order_release);
    }

    //==============================================================================
    float* Looper::samplesOf (int slot, int channel) noexcept
    {
        return memory.data()
                 + (static_cast<std::size_t> (slot) * static_cast<std::size_t> (channelsHeld)
                    + static_cast<std::size_t> (channel)) * static_cast<std::size_t> (room);
    }

    const float* Looper::samplesOf (int slot, int channel) const noexcept
    {
        return memory.data()
                 + (static_cast<std::size_t> (slot) * static_cast<std::size_t> (channelsHeld)
                    + static_cast<std::size_t> (channel)) * static_cast<std::size_t> (room);
    }

    std::size_t Looper::chunkIndex (int slot, std::int64_t chunk) const noexcept
    {
        return static_cast<std::size_t> (slot) * static_cast<std::size_t> (chunks) + static_cast<std::size_t> (chunk);
    }

    std::uint32_t Looper::generationNow (int slot) const noexcept
    {
        return generation[static_cast<std::size_t> (slot)].load (std::memory_order_acquire);
    }

    void Looper::moveOn (int slot) noexcept
    {
        //  EMPTIED IN ONE STORE: every chunk of the slot is now behind.
        auto& each = generation[static_cast<std::size_t> (slot)];
        each.store (each.load (std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    bool Looper::isFresh (int slot, std::int64_t at) const noexcept
    {
        return stamps[chunkIndex (slot, at / peakSamples)].load (std::memory_order_relaxed) == generationNow (slot);
    }

    //==============================================================================
    float Looper::mixAt (int channel, std::int64_t at) const noexcept
    {
        if (at < 0 || at >= room)
            return 0.0f;

        /*  IN SLOT ORDER, so a sum without its top layer is the sum it was
            before that layer was laid, bit for bit. */
        auto sum = 0.0f;
        const auto top = std::max (closedLayers, laying);

        for (int slot = 0; slot <= top; ++slot)
            if (isFresh (slot, at))
                sum += samplesOf (slot, channel)[at];

        if (dropping > 0 && isFresh (dropping, at))
            sum += fadeDown.data()[dropAt] * samplesOf (dropping, channel)[at];

        return sum;
    }

    void Looper::write (int slot, std::int64_t at, const float* input) noexcept
    {
        if (at < 0 || at >= room)
            return;

        const auto chunk = at / peakSamples;
        const auto index = chunkIndex (slot, chunk);
        const auto wanted = generationNow (slot);

        //  A CHUNK BEHIND ITS SLOT is zeroed the first time it is written again.
        if (stamps[index].load (std::memory_order_relaxed) != wanted)
        {
            for (int channel = 0; channel < channelsHeld; ++channel)
                std::fill_n (samplesOf (slot, channel) + chunk * peakSamples, peakSamples, 0.0f);

            peaks[index].store (0.0f, std::memory_order_relaxed);
            stamps[index].store (wanted, std::memory_order_release);
        }

        auto loudest = peaks[index].load (std::memory_order_relaxed);

        for (int channel = 0; channel < channelsHeld; ++channel)
        {
            auto& sample = samplesOf (slot, channel)[at];
            sample += input[channel];
            loudest = std::max (loudest, std::abs (sample));
        }

        peaks[index].store (loudest, std::memory_order_relaxed);
    }

    void Looper::layInto (int slot, float gain, const float* input) noexcept
    {
        float scaled[maxChannels] {};

        if (crossfade >= 0)
        {
            /*  UNDER A WRAP OR A JUMP: laid rising at the playhead and falling
                where the loop was going, so the next crossfade over this place
                sums the two back to the pass itself. */
            const auto rising = gain * fadeUp.data()[crossfade];
            const auto falling = gain * fadeDown.data()[crossfade];

            for (int channel = 0; channel < channelsHeld; ++channel)
                scaled[channel] = rising * input[channel];

            write (slot, position, scaled);

            for (int channel = 0; channel < channelsHeld; ++channel)
                scaled[channel] = falling * input[channel];

            write (slot, fadingFrom + crossfade, scaled);
            return;
        }

        for (int channel = 0; channel < channelsHeld; ++channel)
            scaled[channel] = gain * input[channel];

        write (slot, position, scaled);
    }

    //==============================================================================
    void Looper::apply (const Command& command, std::int64_t sample) noexcept
    {
        switch (command.verb)
        {
            case Verb::record:
                if (now == TakeState::empty)
                {
                    now = TakeState::recording;
                    written = 0;
                    closeWanted = false;
                }
                else if (now == TakeState::recording)    closeTake (sample, false, TakeState::looping);
                else if (now == TakeState::looping)      startLayer (sample);
                else if (now == TakeState::overdubbing)  closeLayer();
                else if (now == TakeState::held)
                {
                    //  A TAKE HELD SILENT: looped from the in point, a layer laid from there.
                    position = in;
                    startLoop();
                    startLayer (sample);
                }
                break;

            case Verb::loop:
                if (now == TakeState::recording)         closeTake (sample, false, TakeState::looping);
                else if (now == TakeState::overdubbing)  closeLayer();
                else if (now == TakeState::held)
                {
                    position = in;
                    startLoop();
                }
                break;

            case Verb::overdub:
                if (now == TakeState::recording)         closeTake (sample, false, TakeState::looping);
                else if (now == TakeState::looping)      startLayer (sample);
                else if (now == TakeState::overdubbing)  closeLayer();
                else if (now == TakeState::held)
                {
                    position = in;
                    startLoop();
                    startLayer (sample);
                }
                break;

            case Verb::undo:
                if (now == TakeState::recording)
                {
                    //  A take never looped is abandoned whole: nothing of it was heard.
                    empty();
                }
                else if (now == TakeState::overdubbing)
                {
                    //  The pass being laid, faded out and gone.
                    dropping = laying;
                    dropAt = 0;
                    laying = 0;
                    now = TakeState::looping;
                }
                else if ((now == TakeState::looping || now == TakeState::held) && closedLayers > 0)
                {
                    //  The top layer - never the take itself (decision CS).
                    dropping = closedLayers;
                    dropAt = 0;
                    --closedLayers;

                    if (tailing == dropping)
                        tailing = tailAt = 0;
                }
                break;

            case Verb::clear:
                if (playing)
                {
                    stopLoop (TakeState::empty);
                    clearAfterStop = true;
                }
                else
                {
                    empty();
                }
                break;

            case Verb::hold:
                if (now == TakeState::recording)
                {
                    //  Its cue ended while it recorded: the take is kept, held silent.
                    closeTake (sample, false, TakeState::held);
                }
                else if (now == TakeState::overdubbing)
                {
                    closeLayer();
                    stopLoop (TakeState::held);
                }
                else if (now == TakeState::looping)
                {
                    stopLoop (TakeState::held);
                }
                break;

            case Verb::points:
                setPoints (command.in, command.out);
                break;
        }
    }

    void Looper::closeTake (std::int64_t sample, bool full, TakeState into) noexcept
    {
        /*  SHORTER THAN A LOOP MAY BE: it goes on recording until it is two
            crossfades long, and closes then. */
        if (written < 2 * static_cast<std::int64_t> (fade))
        {
            closeWanted = true;
            closeInto = into;
            return;
        }

        takeLength = written;
        closeWanted = false;
        in = 0;
        out = takeLength;

        /*  THE POST-ROLL: one crossfade more of the input, past the take's end,
            which the loop's first wrap fades out through. */
        postRoll = fade;
        postRollAt = takeLength;

        tell (full ? Event::Kind::full : Event::Kind::closed, sample, takeLength, into);

        if (into == TakeState::looping)
        {
            position = in;
            startLoop();
        }
        else
        {
            now = into;
        }
    }

    bool Looper::startLayer (std::int64_t sample) noexcept
    {
        if (closedLayers + 1 >= slots)
        {
            tell (Event::Kind::layersFull, sample, takeLength);
            return false;
        }

        laying = closedLayers + 1;
        passAt = 0;
        moveOn (laying);
        now = TakeState::overdubbing;
        return true;
    }

    void Looper::closeLayer() noexcept
    {
        if (laying > 0)
        {
            //  Closed, and still laid for one crossfade more, falling.
            closedLayers = laying;
            tailing = laying;
            tailAt = 0;
        }

        laying = 0;
        now = TakeState::looping;
    }

    void Looper::startLoop() noexcept
    {
        now = TakeState::looping;
        crossfade = -1;
        playing = true;
        rampingUp = true;
        rampingDown = false;
        rampAt = 0;
        clearAfterStop = false;
    }

    void Looper::stopLoop (TakeState then) noexcept
    {
        now = then;

        if (! playing)
            return;

        /*  A STOP DURING A START turns round where the start had got to: the
            equal-power ramp read backwards from the same gain. */
        rampAt = rampingUp ? fade - 1 - rampAt : 0;
        rampingUp = false;
        rampingDown = true;
    }

    void Looper::empty() noexcept
    {
        for (int slot = 0; slot < slots; ++slot)
            moveOn (slot);

        now = TakeState::empty;
        written = takeLength = 0;
        closeWanted = false;
        closedLayers = laying = passAt = tailing = tailAt = dropping = dropAt = 0;
        postRoll = 0;
        position = in = out = 0;
        crossfade = -1;
        playing = rampingUp = rampingDown = clearAfterStop = false;
        rampAt = 0;
    }

    void Looper::setPoints (std::int64_t inAsked, std::int64_t outAsked) noexcept
    {
        if (takeLength <= 0 || now == TakeState::empty || now == TakeState::recording)
            return;

        /*  IN THE TAKE, AND AT LEAST TWO CROSSFADES APART. A playhead the move
            leaves outside the loop is sent to the in point by process(). */
        const auto shortest = 2 * static_cast<std::int64_t> (fade);
        in = std::clamp<std::int64_t> (inAsked, 0, takeLength - shortest);
        out = std::clamp<std::int64_t> (outAsked, in + shortest, takeLength);
    }

    bool Looper::isBusy() const noexcept
    {
        return rampingDown || dropping > 0;
    }

    void Looper::publish() noexcept
    {
        const auto recorded = now == TakeState::recording ? written : takeLength;

        publishedState.store (static_cast<int> (now), std::memory_order_relaxed);
        publishedLength.store (recorded, std::memory_order_relaxed);
        publishedLayers.store (closedLayers, std::memory_order_relaxed);
        publishedPlayhead.store (now == TakeState::recording ? written : position, std::memory_order_relaxed);
        publishedIn.store (in, std::memory_order_relaxed);
        publishedOut.store (out, std::memory_order_relaxed);
        publishedChunks.store ((recorded + peakSamples - 1) / peakSamples, std::memory_order_relaxed);
    }

    //==============================================================================
    void Looper::process (float* const* channelData, int numChannels, int numSamples,
                          std::int64_t blockStart) noexcept
    {
        if (takeSamples <= 0 || channelData == nullptr || numSamples <= 0)
            return;

        const auto width = std::min (numChannels, channelsHeld);
        const auto hearInput = through.load (std::memory_order_relaxed);

        for (int n = 0; n < numSamples; ++n)
        {
            const auto sample = blockStart + n;

            //  THE PRESSES DUE BY THIS SAMPLE - held back while an action fades out.
            while (! isBusy())
            {
                const auto head = inboxHead.load (std::memory_order_relaxed);

                if (head == inboxTail.load (std::memory_order_acquire))
                    break;

                const auto command = inbox[head % static_cast<std::uint32_t> (inboxSize)];

                if (command.at > sample)
                    break;

                inboxHead.store (head + 1, std::memory_order_release);
                apply (command, sample);
            }

            float input[maxChannels] {};

            for (int channel = 0; channel < width; ++channel)
                input[channel] = channelData[channel][n];

            /*  WHAT THE LOOP SOUNDS, read before anything is written at this
                sample - and only what sounded moves on after it: a take that
                closes at the end of this sample starts its loop at the next. */
            const auto sounded = playing;
            float heard[maxChannels] {};

            /*  A WRAP, OR A MOVE THAT LEFT THE PLAYHEAD OUTSIDE THE LOOP: to
                the in point from this sample, crossfaded from where it was
                going - once a crossfade already under way is done. */
            if (sounded && crossfade < 0 && (position >= out || position < in))
            {
                fadingFrom = position;
                position = in;
                crossfade = 0;
            }

            if (sounded)
            {
                const auto gain = rampingUp ? fadeUp.data()[rampAt]
                                            : rampingDown ? fadeDown.data()[rampAt] : 1.0f;

                for (int channel = 0; channel < width; ++channel)
                {
                    auto value = mixAt (channel, position);

                    if (crossfade >= 0)
                        value = fadeUp.data()[crossfade] * value
                                  + fadeDown.data()[crossfade] * mixAt (channel, fadingFrom + crossfade);

                    heard[channel] = gain * value;
                }
            }

            //  WHAT IS RECORDED: the take as it grows, a layer at the playhead, the post-roll.
            if (now == TakeState::recording)
            {
                write (0, written, input);
                ++written;
            }
            else if (now == TakeState::overdubbing && laying > 0)
            {
                layInto (laying, passAt < fade ? fadeUp.data()[passAt] : 1.0f, input);
                passAt = std::min (passAt + 1, fade);
            }

            if (tailing > 0)
            {
                if (sounded)
                    layInto (tailing, fadeDown.data()[tailAt], input);

                if (++tailAt >= fade)
                    tailing = tailAt = 0;
            }

            if (postRoll > 0)
            {
                write (0, postRollAt, input);
                ++postRollAt;
                --postRoll;
            }

            for (int channel = 0; channel < width; ++channel)
                channelData[channel][n] = heard[channel] + (hearInput ? input[channel] : 0.0f);

            //  A TAKE THAT FILLED ITS MEMORY, or was asked to close too soon, closes from the next sample.
            if (now == TakeState::recording)
            {
                if (written >= takeSamples)
                    closeTake (sample + 1, true, TakeState::looping);
                else if (closeWanted && written >= 2 * static_cast<std::int64_t> (fade))
                    closeTake (sample + 1, false, closeInto);
            }

            if (sounded)
            {
                ++position;

                if (crossfade >= 0 && ++crossfade >= fade)
                    crossfade = -1;
            }

            if (sounded && (rampingUp || rampingDown) && ++rampAt >= fade)
            {
                if (rampingDown)
                {
                    playing = false;

                    if (clearAfterStop)
                        empty();
                }

                rampingUp = rampingDown = false;
                rampAt = 0;
            }

            if (dropping > 0 && ++dropAt >= fade)
            {
                moveOn (dropping);
                dropping = dropAt = 0;
            }
        }

        publish();
    }

    //==============================================================================
    TakeState Looper::state() const noexcept
    {
        return static_cast<TakeState> (publishedState.load (std::memory_order_relaxed));
    }

    std::int64_t Looper::length() const noexcept     { return publishedLength.load (std::memory_order_relaxed); }
    int Looper::layerCount() const noexcept          { return publishedLayers.load (std::memory_order_relaxed); }
    std::int64_t Looper::playhead() const noexcept   { return publishedPlayhead.load (std::memory_order_relaxed); }
    std::int64_t Looper::loopIn() const noexcept     { return publishedIn.load (std::memory_order_relaxed); }
    std::int64_t Looper::loopOut() const noexcept    { return publishedOut.load (std::memory_order_relaxed); }

    int Looper::peakCount() const noexcept
    {
        return static_cast<int> (publishedChunks.load (std::memory_order_relaxed));
    }

    float Looper::peak (int slot, int chunk) const noexcept
    {
        if (slot < 0 || slot >= slots || chunk < 0 || chunk >= chunks || stamps == nullptr)
            return 0.0f;

        const auto index = chunkIndex (slot, chunk);

        return stamps[index].load (std::memory_order_acquire) == generationNow (slot)
                   ? peaks[index].load (std::memory_order_relaxed)
                   : 0.0f;
    }
}
