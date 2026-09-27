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

#include <wfg/engine/rt/RtCheck.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/*
    THE RECORDER OF A SAMPLING CHANNEL (Phase 9c, stage 9c.1, namespace draft
    §19; decisions CA, CD and CR): one take, the layers laid on it, and the
    loop it plays between an in and an out point that move while it plays.

    WHY THIS IS A SEPARATE CLASS FROM THE PLUGIN THAT WILL HOLD IT - CueEq's
    reason, exactly. It names no Tracktion type and no JUCE type, so a take
    closed to the sample, an Undo bit for bit and the step at a wrap on a sine
    are tested in microseconds with no engine; and it is owned by the audio
    host beside the Edit rather than by a plugin, because every media arm
    rebuilds the graph and a take must outlive that (§19.2).

    THE MEMORY IS SET ASIDE AND TOUCHED IN prepare(), on the message thread
    when the show opens: (1 + layers) passes of the longest take, both
    channels, every page written once, so the first Rec never allocates and
    never faults a page in on the audio thread (PRD §4.1 and §4.2).

    LAYERS ARE KEPT APART. The take is slot nought and each pass laid on it a
    slot of its own, summed in slot order as the loop plays: Undo takes the top
    slot out of the sum, and what is left sums to what it summed to before the
    layer was laid - bit for bit, because the same numbers are added in the
    same order. Nothing folds a layer into the take (decision CA's cost).

    A SLOT IS WIPED WITHOUT BEING WIPED. Zeroing a minute of two channels is
    twenty megabytes, far too much for one block. So the take is cut into
    chunks of `peakSamples`, each stamped with the generation of its slot when
    it was last written; a slot is emptied by moving its generation on, which
    is one store, and a chunk whose stamp is behind reads as silence and is
    zeroed the first time it is written again - 256 samples at a time, by the
    audio thread, as it gets there. The chunk is also the unit of the take's
    picture: its peak is kept beside its stamp.

    EVERY PRESS IS PLACED AT A SAMPLE. `post` takes a command from one thread
    - the tick thread, which places a press at `now + the launch latency` as it
    places a launch (§19.6) - into a queue of sixty-four with no lock, and the
    audio thread applies it at that sample inside the block. What the audio
    thread decides on its own - a take that filled its memory and closed
    itself, the length a pressed take closed at - comes back through `nextEvent`
    for the engine to log (`take.closed`), so a replay knows it.

    NOTHING CLICKS (decision CD). A wrap or a jump is an equal-power crossfade
    of ten milliseconds between where the loop was going and where it goes,
    both read inside the take: a closed take goes on recording for one
    crossfade past its end, so even a loop of the whole take has somewhere to
    fade out from. The loop starts and stops over the same ten milliseconds; a
    layer taken off by Undo fades out before it goes; and an action that fades
    holds the next command back until it is done, which is at most ten
    milliseconds late and never a step. A loop is at least two crossfades long.

    A LAYER HAS SOFT EDGES. A pass rises over its first ten milliseconds and
    goes on for ten more after it is closed, falling; and at every wrap or jump
    under it, the input is laid twice - at the in point, rising, and where the
    loop was going, falling - so the loop's own crossfade puts the layer back
    together at unity (the sine squared and the cosine squared make one), and
    a layer is never an edge from its sound to silence in one sample, wherever
    the points are later moved. A take needs none of it: it is recorded whole,
    and its end goes on recording for one crossfade.

    WHAT IT SOUNDS. The loop, and the input as well when `through` is on
    (§19.3) - off, a voice being sampled is not doubled by the channel that
    samples it. While a layer is laid the loop plays it too, read before the
    input is added, so a pass is heard from its second time round.
*/
namespace wfg::audio
{
    /** What a take is doing, in a word: the order `slot/<id>/take` says them in. */
    enum class TakeState : int { empty, recording, looping, overdubbing, held };

    class Looper
    {
    public:
        /** What prepare() sets aside for. */
        struct Shape
        {
            double sampleRate = 48000.0;
            int channels = 2;
            double takeSeconds = 0.0;   ///< the longest take; nought is no recorder
            int layers = 4;             ///< passes kept on top of the take
        };

        static constexpr int maxChannels = 2;          ///< a rack track is two wide
        static constexpr int maxLayers = 16;
        static constexpr int peakSamples = 256;        ///< a chunk: one stamp, one peak
        static constexpr double crossfadeSeconds = 0.010;
        static constexpr int inboxSize = 64;
        static constexpr int outboxSize = 64;

        Looper() = default;
        Looper (const Looper&) = delete;
        Looper& operator= (const Looper&) = delete;

        //======================================================================
        /*  Sets aside and touches the memory for that shape and empties the
            channel. Message thread, with the audio thread not in process(). A
            shape with no take seconds holds nothing, and process() then leaves
            every block as it found it. */
        void prepare (const Shape&);

        const Shape& shape() const noexcept             { return current; }

        /** What prepare() set aside, in bytes - said in words in the Rack tab. */
        std::size_t bytes() const noexcept;

        /** The longest take, in samples; nought for no recorder. */
        std::int64_t capacity() const noexcept          { return takeSamples; }

        /** A wrap's crossfade, in samples: ten milliseconds at the rate. */
        int crossfadeSamples() const noexcept           { return fade; }

        //======================================================================
        enum class Verb : std::uint8_t { record, loop, overdub, undo, clear, hold, points };

        /*  A press, at a sample of Go.dot's count; one in the past lands at
            the start of the next block. `in` and `out` are for `points`, in
            samples into the take. */
        struct Command
        {
            Verb verb = Verb::record;
            std::int64_t at = -1;
            std::int64_t in = 0;
            std::int64_t out = 0;
        };

        /*  From one thread, the tick thread. False when sixty-four are already
            waiting, which a tick of presses cannot fill. */
        bool post (const Command&) noexcept;

        /** Whether the channel sounds its input as well as its loop. Any thread. */
        void setThrough (bool shouldHearInput) noexcept;
        bool isThrough() const noexcept;

        //======================================================================
        /*  The audio thread. Records the block from `channelData` and writes
            what the channel sounds over it, in place; `blockStart` is where the
            block begins in Go.dot's count, against which every press is placed. */
        void process (float* const* channelData, int numChannels, int numSamples,
                      std::int64_t blockStart) noexcept WFG_AUDIO_THREAD;

        //======================================================================
        /** What the audio thread did by itself, or the length it closed a take at. */
        struct Event
        {
            enum class Kind : std::uint8_t
            {
                closed,         ///< a take closed by a press, at `length` samples
                full,           ///< a take that filled its memory and closed itself
                layersFull,     ///< a pass refused: every layer is in use
            };

            Kind kind = Kind::closed;
            std::int64_t at = 0;
            std::int64_t length = 0;
        };

        /** The next event, oldest first; one consumer. */
        bool nextEvent (Event&) noexcept;

        //======================================================================
        /*  WHERE IT IS, published at the end of every block. Any thread. The
            playhead and the points are samples into the take. */
        TakeState state() const noexcept;
        std::int64_t length() const noexcept;
        int layerCount() const noexcept;
        std::int64_t playhead() const noexcept;
        std::int64_t loopIn() const noexcept;
        std::int64_t loopOut() const noexcept;

        /*  THE TAKE'S PICTURE: how many chunks the take covers, and the
            largest sample of either channel in a chunk of a slot - nought is
            the take, one on its layers, and the one being laid after those.
            A chunk not written since its slot was emptied reads nought. Any
            thread; the audio thread writes them as it records. */
        int peakCount() const noexcept;
        float peak (int slot, int chunk) const noexcept;

    private:
        //======================================================================
        float* samplesOf (int slot, int channel) noexcept;
        const float* samplesOf (int slot, int channel) const noexcept;
        std::size_t chunkIndex (int slot, std::int64_t chunk) const noexcept;
        std::uint32_t generationNow (int slot) const noexcept;
        void moveOn (int slot) noexcept;
        bool isFresh (int slot, std::int64_t at) const noexcept;

        float mixAt (int channel, std::int64_t at) const noexcept;
        void write (int slot, std::int64_t at, const float* input) noexcept;
        void layInto (int slot, float gain, const float* input) noexcept;

        void apply (const Command&, std::int64_t sample) noexcept;
        void closeTake (std::int64_t sample, bool full, TakeState into) noexcept;
        bool startLayer (std::int64_t sample) noexcept;
        void closeLayer() noexcept;
        void startLoop() noexcept;
        void stopLoop (TakeState then) noexcept;
        void empty() noexcept;
        void setPoints (std::int64_t in, std::int64_t out) noexcept;
        bool isBusy() const noexcept;
        void tell (Event::Kind, std::int64_t sample, std::int64_t samples) noexcept;
        void publish() noexcept;

        //======================================================================
        Shape current;
        int channelsHeld = 0;
        int slots = 0;
        std::int64_t takeSamples = 0;
        std::int64_t room = 0;              ///< what one slot holds per channel, whole chunks
        std::int64_t chunks = 0;
        int fade = 1;

        std::vector<float> memory;          ///< slot, then channel, then sample
        std::unique_ptr<std::atomic<std::uint32_t>[]> stamps;
        std::unique_ptr<std::atomic<float>[]> peaks;
        std::array<std::atomic<std::uint32_t>, maxLayers + 1> generation {};
        std::vector<float> fadeUp, fadeDown;    ///< the equal-power ramp, `fade` long

        std::array<Command, inboxSize> inbox {};
        std::atomic<std::uint32_t> inboxHead { 0 }, inboxTail { 0 };
        std::array<Event, outboxSize> outbox {};
        std::atomic<std::uint32_t> outboxHead { 0 }, outboxTail { 0 };

        std::atomic<bool> through { false };

        std::atomic<int> publishedState { 0 };
        std::atomic<std::int64_t> publishedLength { 0 };
        std::atomic<int> publishedLayers { 0 };
        std::atomic<std::int64_t> publishedPlayhead { 0 };
        std::atomic<std::int64_t> publishedIn { 0 };
        std::atomic<std::int64_t> publishedOut { 0 };
        std::atomic<std::int64_t> publishedChunks { 0 };

        //======================================================================
        /*  THE AUDIO THREAD'S OWN, from prepare() on. */
        TakeState now = TakeState::empty;
        std::int64_t written = 0;           ///< the take so far, while it records
        std::int64_t takeLength = 0;
        bool closeWanted = false;           ///< a close pressed before the take is two crossfades long
        TakeState closeInto = TakeState::looping;
        int closedLayers = 0;               ///< layers on the take, the one being laid not counted
        int laying = 0;                     ///< the slot being laid, nought for none
        int passAt = 0;                     ///< how far into its rise the pass being laid is
        int tailing = 0;                    ///< a pass closed and still falling, nought for none
        int tailAt = 0;
        std::int64_t postRoll = 0;          ///< samples still to record past a closed take
        std::int64_t postRollAt = 0;

        std::int64_t position = 0;          ///< the playhead
        std::int64_t in = 0, out = 0;
        int crossfade = -1;                 ///< how far into a wrap's crossfade, -1 for none
        std::int64_t fadingFrom = 0;        ///< where the loop it fades out of had got to

        bool playing = false;               ///< the loop sounding, or fading to silence
        int rampAt = 0;                     ///< how far along the loop's start or stop
        bool rampingUp = false, rampingDown = false;
        bool clearAfterStop = false;

        int dropping = 0;                   ///< a layer fading out before it goes
        int dropAt = 0;
    };
}
