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

/*
    SPIKE 03b — WHERE A RANGE ENDS AND THE NEXT PASS BEGINS.  THROWAWAY CODE.

    M12 of the Phase 3 plan, and it decides one thing:

        Go.dot places every boundary between ranges as a stop-and-play pair on
        two slots. A range that LOOPS could be done the same way - a placed
        play at each pass - or it could be left to the clip's own wrap. Which
        of the two sounds better?

    Spike 03 answered §6.1's follow-action question and answered it NO: follow
    actions are not sample-accurate enough and there is no crossfade at a
    boundary without a custom clip. This is the sequel, on the mechanism Phase 3
    actually chose, and it is a comparison rather than a verdict on one thing.

    THE THREE JOINS, side by side on one rig at five block sizes:

      1. THE CLIP'S OWN WRAP. One slot, one clip, armed looping over the whole
         file. Nothing in Go.dot touches the boundary: WaveNodeRealTime wraps
         inside the block. This is the mechanism PR 3.8 built, and if it is the
         best of the three then a looping range costs Go.dot nothing at all.

      2. LaunchHandle::setLooping ON A CLIP THAT WAS ARMED NOT LOOPING. The
         obvious alternative, and the reading of the sources says it CANNOT
         work: SlotControlNode captures a stop duration when the graph is built
         - the clip's length in beats when isLooping() is false - and queues
         that stop every block, ahead of the wrap
         (tracktion_SlotControlNode.cpp:134-153,
          tracktion_EditNodeBuilder.cpp:1025-1026). setLooping is a seqlock
         store with no rebuild, so it cannot remove a duration that was already
         captured. Measured anyway, because a spike that only confirms what the
         source says is a spike that stops being run.

      3. THE CROSS-SLOT PLACED BOUNDARY. Two slots holding two halves of the
         file, a stop on the first and a play on the second at the same sample.
         This is what a boundary BETWEEN RANGES is, and it is measured here so
         that a looping join has something to be compared against.

    HOW A JOIN IS MADE MEASURABLE — spike 03's method, unchanged, because the
    two numbers have to be comparable.

    The source is a LINEAR CHIRP and that is load-bearing. Alignment is found by
    searching for the offset that best matches a reference. Against a sine that
    search is ambiguous modulo one period and every join would measure as
    perfect. A chirp is never self-similar, so the alignment is unique.

    The launch instant jitters between runs (spike 04), so nothing absolute is
    reported. What is reported is a DIFFERENCE of two alignments:

        alignPre  = the offset that best matches the reference before the join
        alignPost = the offset that best matches it after the join
        join_error_samples = alignPost - alignPre

    The jitter is common to both and cancels. Zero means the join dropped and
    repeated nothing.

    AND THE DAMAGED SPAN, which is the number the PRD's ear cares about: how
    many consecutive samples around the join differ from the reference by more
    than the floor. A join can land on its sample and still have a click.
*/

#include "../SpikeHarness.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace tracktion;
using namespace tracktion::engine;

namespace
{
    constexpr auto criterion =
        "M12: of the clip's own loop wrap, a setLooping re-trigger, and a placed "
        "cross-slot boundary, which join is cleanest - and is the wrap good enough "
        "to leave a looping range alone?";

    constexpr const char* extraFlags = "";

    constexpr double fileLengthSeconds = 2.0;
    constexpr double chirpStartHz = 200.0;
    constexpr double chirpEndHz = 2000.0;
    constexpr float  amplitude = 0.5f;

    /*  How far either side of a join is inspected for damage. A block is the
        unit the artefact would arrive in, and 1024 is the largest block this
        spike runs, so this covers the worst case with room around it. */
    constexpr long long damageRadius = 4096;

    /*  What counts as damaged. The source is 16-bit-clean float written through
        a WAV, so a sample that agrees is within a quantisation step or two of
        the reference; anything past this is material that is not there. */
    constexpr double damageFloor = 0.02;

    //==============================================================================
    /*  A linear chirp, copied from spike 03 rather than shared with it, because
        a spike is throwaway and two spikes that share a file are two spikes
        that cannot be deleted separately.

        Phase is integrated rather than computed per sample from an instantaneous
        frequency: the naive sin(2*pi*f(t)*t) is not a chirp, it sweeps at twice
        the intended rate, and its phase is not continuous across a join - which
        would put a discontinuity into the REFERENCE and make this spike measure
        its own generator. */
    std::vector<float> makeChirp (double sampleRate, double lengthSeconds)
    {
        const auto numSamples = static_cast<size_t> (sampleRate * lengthSeconds);
        std::vector<float> out (numSamples);

        double phase = 0.0;

        for (size_t i = 0; i < numSamples; ++i)
        {
            const auto t = static_cast<double> (i) / sampleRate;
            const auto f = chirpStartHz + (chirpEndHz - chirpStartHz) * (t / lengthSeconds);

            out[i] = amplitude * static_cast<float> (std::sin (phase));
            phase += 2.0 * juce::MathConstants<double>::pi * f / sampleRate;
        }

        return out;
    }

    //==============================================================================
    double ssd (const choc::buffer::ChannelArrayBuffer<float>& out,
                choc::buffer::ChannelCount channel,
                long long outStart,
                const std::vector<float>& ref,
                size_t refStart,
                size_t len)
    {
        double total = 0.0;

        for (size_t i = 0; i < len; ++i)
        {
            const auto o = outStart + static_cast<long long> (i);
            const auto r = refStart + i;

            if (o < 0 || static_cast<choc::buffer::FrameCount> (o) >= out.getNumFrames()
                  || r >= ref.size())
                return std::numeric_limits<double>::max();

            const auto d = static_cast<double> (
                               out.getSample (channel, static_cast<choc::buffer::FrameCount> (o)))
                             - ref[r];
            total += d * d;
        }

        return total;
    }

    struct Alignment
    {
        long long offset = 0;
        double residual = 0.0;
        bool confident = false;
    };

    /*  The integer output offset at which the reference window starting at
        `refStart` best matches. `confident` is false when the search surface is
        flat, which is what silence and structureless material look like - a
        winner picked off a flat surface is noise, and reporting it as a join
        error would be inventing a measurement. */
    Alignment alignWindow (const choc::buffer::ChannelArrayBuffer<float>& out,
                           choc::buffer::ChannelCount channel,
                           const std::vector<float>& ref,
                           size_t refStart,
                           size_t len,
                           long long searchCentre,
                           long long searchRadius)
    {
        Alignment best;
        best.residual = std::numeric_limits<double>::max();

        double secondBest = std::numeric_limits<double>::max();

        for (long long d = -searchRadius; d <= searchRadius; ++d)
        {
            const auto s = ssd (out, channel, searchCentre + d, ref, refStart, len);

            if (s < best.residual)
            {
                secondBest = best.residual;
                best.residual = s;
                best.offset = searchCentre + d;
            }
            else if (s < secondBest)
            {
                secondBest = s;
            }
        }

        best.confident = best.residual < 1.0e-6
                          || (secondBest < std::numeric_limits<double>::max()
                               && best.residual * 4.0 < secondBest);

        return best;
    }

    /*  How many samples around `joinAt` differ from what should be there by
        more than the floor. The model is a function of output position, because
        which reference sample belongs where is different for a wrap and for a
        cross-slot boundary and this measurement has to be the same for both. */
    struct Damage
    {
        long long span = 0;        ///< first damaged sample to last, inclusive
        long long count = 0;       ///< how many were damaged inside that span
        double worst = 0.0;
        long long worstAt = 0;

        /*  THE LONGEST UNBROKEN RUN of damaged samples, which is a different
            number from `span` and is the one an ear responds to. `span` counts
            from the first damaged sample to the last, so two isolated
            quantisation stragglers a thousand apart report a span of a thousand
            and describe nothing. */
        long long longestRun = 0;

        /*  How many of the damaged samples were SILENT while the reference was
            not - which is what tells a gap from a fade from wrong material. A
            hole reads as silence; Tracktion's own 40-sample stop decay reads as
            damage that is not silence; a boundary landing on the wrong material
            reads as neither. */
        long long silent = 0;

        /*  THE SUM OF SQUARED DEVIATIONS over the whole inspected window, and
            it is what the verdict compares.

            Comparing on the longest damaged RUN alone gets the answer wrong,
            and this rig demonstrated it: at 48 kHz with 512-frame blocks the
            wrap's run is 55 samples and a placed boundary's is 33, so by run
            length the placed boundary wins - while the wrap's worst deviation
            is 0.027 and the placed boundary's is 0.49, eighteen times louder.
            A longer, quieter blemish is not worse than a shorter, louder one,
            and length alone cannot say so.

            Energy integrates both, which is roughly what an ear does over a
            millisecond. Reported beside the length and the depth rather than
            instead of them, because a single number nobody can decompose is a
            number nobody can argue with. */
        double energy = 0.0;
    };

    template <typename ReferenceAt>
    Damage damageAround (const choc::buffer::ChannelArrayBuffer<float>& out,
                         choc::buffer::ChannelCount channel,
                         long long joinAt,
                         ReferenceAt referenceAt)
    {
        Damage damage;

        long long first = -1;
        long long last = -1;
        long long run = 0;

        for (long long n = joinAt - damageRadius; n <= joinAt + damageRadius; ++n)
        {
            if (n < 0 || static_cast<choc::buffer::FrameCount> (n) >= out.getNumFrames())
                continue;

            const auto expected = referenceAt (n);

            if (! expected.has_value())
                continue;

            const auto actual = static_cast<double> (
                out.getSample (channel, static_cast<choc::buffer::FrameCount> (n)));

            const auto difference = std::abs (actual - *expected);

            damage.energy += difference * difference;

            if (difference > damage.worst)
            {
                damage.worst = difference;
                damage.worstAt = n - joinAt;
            }

            if (difference > damageFloor)
            {
                if (first < 0)
                    first = n;

                last = n;
                ++damage.count;

                ++run;
                damage.longestRun = std::max (damage.longestRun, run);

                if (std::abs (actual) < damageFloor && std::abs (*expected) >= damageFloor)
                    ++damage.silent;
            }
            else
            {
                run = 0;
            }
        }

        damage.span = first < 0 ? 0 : last - first + 1;
        return damage;
    }

    //==============================================================================
    /*  What one of the three joins produced. `reached` says the rig got as far
        as making the join happen at all - which is a real outcome for variant 2
        and not a harness failure. */
    struct JoinResult
    {
        bool reached = false;
        std::string note;

        long long joinErrorSamples = 0;
        bool confident = false;

        Damage damage;

        /** Where the join was expected, in output samples. */
        long long joinAt = 0;
    };

    struct Rig
    {
        std::unique_ptr<Edit> edit;
        std::unique_ptr<juce::TemporaryFile> file;
        std::unique_ptr<test_utilities::EnginePlayer> player;
        juce::Array<ClipSlot*> slots;
        double sampleRate = 0.0;
        int blockSize = 0;
        bool ok = false;
    };

    /*  One track, `numSlots` slots, the chirp written out once and used as the
        source of every clip. Nothing is armed here: what goes in the slots is
        what distinguishes the three variants. */
    Rig makeRig (spike::HeadlessEngine& harness, const spike::Args& args,
                 const std::vector<float>& chirp, int numSlots)
    {
        Rig rig;
        auto& engine = *harness;

        rig.sampleRate = static_cast<double> (args.sampleRate);
        rig.blockSize = static_cast<int> (args.buffer);

        rig.edit = test_utilities::createTestEdit (engine, 1, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*rig.edit);

        if (tracks.isEmpty())
            return rig;

        /*  60 bpm, so one beat is one second and every instant below is the
            same number in beats and in seconds. Go.dot's own Edit does this for
            exactly the same reason. */
        if (auto* tempo = rig.edit->tempoSequence.getTempo (0))
            tempo->setBpm (60.0);

        juce::AudioBuffer<float> buffer (2, static_cast<int> (chirp.size()));

        for (int channel = 0; channel < 2; ++channel)
            std::copy (chirp.begin(), chirp.end(), buffer.getWritePointer (channel));

        rig.file = spike::writeWav (buffer, rig.sampleRate);

        if (rig.file == nullptr)
            return rig;

        auto& slotList = tracks[0]->getClipSlotList();
        slotList.ensureNumberOfSlots (numSlots);
        rig.slots = slotList.getClipSlots();

        rig.ok = rig.slots.size() >= numSlots;
        return rig;
    }

    /*  Makes a clip play at its own rate, which under auto-tempo it does not do
        by default: the launcher schedules in beats, so a clip whose beat count
        is not its length in seconds is stretched. At 60 bpm, numBeats = seconds
        is 1:1. Go.dot's AudioHost does the identical thing and says so at
        greater length. */
    void playAtItsOwnRate (WaveAudioClip& clip)
    {
        const auto seconds = clip.getSourceLength().inSeconds();

        if (seconds <= 0.0)
            return;

        auto info = clip.getLoopInfo();
        info.setNumBeats (seconds);
        clip.setLoopInfo (info);

        clip.setAutoPitch (false);
        clip.setSpeedRatio (1.0);
    }

    bool startPlaying (Rig& rig, const spike::Args& args)
    {
        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = rig.sampleRate;
        params.blockSize      = rig.blockSize;
        params.inputChannels  = 2;
        params.outputChannels = 2;

        const AudioFile audio (rig.edit->engine, rig.file->getFile());

        bool mappedOk = false;
        rig.player = spike::createPlayerWithDeadline (*rig.edit, params, { audio }, mappedOk);

        juce::ignoreUnused (args);

        if (rig.player == nullptr || ! mappedOk)
            return false;

        rig.player->process (rig.blockSize);
        return true;
    }

    //==============================================================================
    /*  VARIANT 1 — THE CLIP'S OWN WRAP.

        One clip, armed looping over the whole file, launched and left alone for
        two passes. The join is at one file length after the sound starts, and
        the material either side of it is the reference twice over.
    */
    JoinResult measureOwnWrap (spike::HeadlessEngine& harness, const spike::Args& args,
                               const std::vector<float>& chirp)
    {
        JoinResult result;

        auto rig = makeRig (harness, args, chirp, 1);

        if (! rig.ok)
        {
            result.note = "no rig";
            return result;
        }

        auto clip = insertWaveClip (*rig.slots[0], {}, rig.file->getFile(),
                                    { { 0_tp, TimeDuration::fromSeconds (fileLengthSeconds) } },
                                    DeleteExistingClips::no);

        if (clip == nullptr)
        {
            result.note = "no clip";
            return result;
        }

        playAtItsOwnRate (*clip);

        /*  ARMED LOOPING, which is the whole variant. A clip whose isLooping()
            is true is built with NO stop duration, so its SlotControlNode never
            queues one and WaveNodeRealTime wraps inside the block. */
        clip->setLoopRangeBeats ({ BeatPosition::fromBeats (0.0),
                                   BeatPosition::fromBeats (fileLengthSeconds) });

        if (! clip->isLooping())
        {
            result.note = "the clip refused to loop";
            return result;
        }

        if (! startPlaying (rig, args))
        {
            result.note = "no player";
            return result;
        }

        if (! spike::launchAtNextWholeBeat (*clip))
        {
            result.note = "could not launch";
            return result;
        }

        /*  Three file lengths, so there is a whole pass either side of the
            first wrap and room for the launch to be placed. */
        const auto blocks = static_cast<int> ((3.5 * fileLengthSeconds * rig.sampleRate)
                                                / rig.blockSize);

        for (int i = 0; i < blocks; ++i)
            rig.player->process (rig.blockSize);

        const auto out = rig.player->getOutput();
        const auto length = static_cast<long long> (chirp.size());

        /*  WHERE THE SOUND STARTS, found rather than assumed: the launch is
            placed at a beat and the jitter is spike 04's. A coarse alignment of
            the first half-second answers it. */
        const auto window = static_cast<size_t> (rig.sampleRate / 4.0);

        const auto pre = alignWindow (out, 0, chirp, 0, window,
                                      static_cast<long long> (rig.sampleRate), length);

        if (! pre.confident)
        {
            result.note = "could not find the first pass";
            return result;
        }

        /*  AND WHERE THE SECOND PASS STARTS, searched near where the first pass
            says it should be. A window from a little way into the reference,
            because the very start of a chirp is its lowest frequency and its
            least distinctive quarter-second. */
        const auto expectedSecond = pre.offset + length;

        const auto post = alignWindow (out, 0, chirp, window, window,
                                       expectedSecond + static_cast<long long> (window),
                                       static_cast<long long> (rig.sampleRate / 20.0));

        result.reached = true;
        result.confident = post.confident;
        result.joinAt = expectedSecond;
        result.joinErrorSamples = (post.offset - static_cast<long long> (window)) - expectedSecond;

        /*  The model around the wrap: before it, the reference read from where
            the first pass put it; after it, the reference read from where the
            second pass did. */
        const auto start = pre.offset;

        result.damage = damageAround (out, 0, expectedSecond,
            [&chirp, start, length] (long long n) -> std::optional<double>
            {
                auto index = n - start;

                while (index >= length)
                    index -= length;

                if (index < 0 || index >= length)
                    return {};

                return static_cast<double> (chirp[static_cast<size_t> (index)]);
            });

        return result;
    }

    //==============================================================================
    /*  VARIANT 2 — setLooping ON A CLIP ARMED NOT LOOPING.

        The sources say this cannot work. Measured because a spike that only
        repeats what the source says is one nobody re-runs after a version bump,
        and this is exactly the kind of thing a version bump changes.
    */
    JoinResult measureSetLooping (spike::HeadlessEngine& harness, const spike::Args& args,
                                  const std::vector<float>& chirp)
    {
        JoinResult result;

        auto rig = makeRig (harness, args, chirp, 1);

        if (! rig.ok)
        {
            result.note = "no rig";
            return result;
        }

        auto clip = insertWaveClip (*rig.slots[0], {}, rig.file->getFile(),
                                    { { 0_tp, TimeDuration::fromSeconds (fileLengthSeconds) } },
                                    DeleteExistingClips::no);

        if (clip == nullptr)
        {
            result.note = "no clip";
            return result;
        }

        playAtItsOwnRate (*clip);

        /*  NOT LOOPING at graph-build time, which is what puts a stop duration
            into the SlotControlNode. */
        clip->disableLooping();

        if (! startPlaying (rig, args))
        {
            result.note = "no player";
            return result;
        }

        auto handle = clip->getLaunchHandle();

        if (handle == nullptr)
        {
            result.note = "no launch handle";
            return result;
        }

        /*  THE STORE THIS VARIANT IS ABOUT, made before the launch so that it
            has every chance: a seqlock write with no rebuild behind it. */
        handle->setLooping (BeatDuration::fromBeats (fileLengthSeconds));

        if (! spike::launchAtNextWholeBeat (*clip))
        {
            result.note = "could not launch";
            return result;
        }

        const auto blocks = static_cast<int> ((3.5 * fileLengthSeconds * rig.sampleRate)
                                                / rig.blockSize);

        for (int i = 0; i < blocks; ++i)
            rig.player->process (rig.blockSize);

        const auto out = rig.player->getOutput();
        const auto length = static_cast<long long> (chirp.size());
        const auto window = static_cast<size_t> (rig.sampleRate / 4.0);

        const auto pre = alignWindow (out, 0, chirp, 0, window,
                                      static_cast<long long> (rig.sampleRate), length);

        if (! pre.confident)
        {
            result.note = "could not find the first pass";
            return result;
        }

        /*  DID IT WRAP AT ALL. Everything after the join instant is examined
            for sound, because the failure this variant expects is not a bad
            join but silence: the queued stop fires and the clip never comes
            back. */
        const auto expectedSecond = pre.offset + length;

        double peakAfter = 0.0;

        for (auto n = expectedSecond + static_cast<long long> (window);
             n < expectedSecond + 2 * static_cast<long long> (window); ++n)
        {
            if (n < 0 || static_cast<choc::buffer::FrameCount> (n) >= out.getNumFrames())
                break;

            peakAfter = std::max (peakAfter,
                                  std::abs (static_cast<double> (
                                      out.getSample (0, static_cast<choc::buffer::FrameCount> (n)))));
        }

        result.joinAt = expectedSecond;

        if (peakAfter < damageFloor)
        {
            result.reached = false;
            result.note = "it stopped at the end of the first pass and did not come back";
            return result;
        }

        const auto post = alignWindow (out, 0, chirp, window, window,
                                       expectedSecond + static_cast<long long> (window),
                                       static_cast<long long> (rig.sampleRate / 20.0));

        result.reached = true;
        result.confident = post.confident;
        result.joinErrorSamples = (post.offset - static_cast<long long> (window)) - expectedSecond;

        const auto start = pre.offset;

        result.damage = damageAround (out, 0, expectedSecond,
            [&chirp, start, length] (long long n) -> std::optional<double>
            {
                auto index = n - start;

                while (index >= length)
                    index -= length;

                if (index < 0 || index >= length)
                    return {};

                return static_cast<double> (chirp[static_cast<size_t> (index)]);
            });

        return result;
    }

    //==============================================================================
    /*  VARIANT 3 — THE CROSS-SLOT PLACED BOUNDARY.

        Two slots, two halves of the same file, and Go.dot's own arrangement: a
        stop queued on the first at a beat and a play queued on the second at
        the SAME beat. If the pair lands the output reconstructs the chirp.

        This is the join PR 3.9 will place between two ranges, and it is here so
        that the wrap above has something to be better or worse than.
    */
    JoinResult measurePlacedBoundary (spike::HeadlessEngine& harness, const spike::Args& args,
                                      const std::vector<float>& chirp)
    {
        JoinResult result;

        auto rig = makeRig (harness, args, chirp, 2);

        if (! rig.ok)
        {
            result.note = "no rig";
            return result;
        }

        const auto half = fileLengthSeconds / 2.0;

        auto first = insertWaveClip (*rig.slots[0], {}, rig.file->getFile(),
                                     { { 0_tp, TimeDuration::fromSeconds (fileLengthSeconds) } },
                                     DeleteExistingClips::no);

        auto second = insertWaveClip (*rig.slots[1], {}, rig.file->getFile(),
                                      { { 0_tp, TimeDuration::fromSeconds (fileLengthSeconds) } },
                                      DeleteExistingClips::no);

        if (first == nullptr || second == nullptr)
        {
            result.note = "no clips";
            return result;
        }

        playAtItsOwnRate (*first);
        playAtItsOwnRate (*second);

        /*  EACH SLOT HOLDS A RANGE, armed exactly as AudioHost::setTrackRanges
            arms one: a loop range over the region, so the clip loops it for
            ever and Go.dot decides when it stops. */
        first->setLoopRangeBeats ({ BeatPosition::fromBeats (0.0),
                                    BeatPosition::fromBeats (half) });

        second->setLoopRangeBeats ({ BeatPosition::fromBeats (half),
                                     BeatPosition::fromBeats (fileLengthSeconds) });

        if (! first->isLooping() || ! second->isLooping())
        {
            result.note = "a clip refused to loop";
            return result;
        }

        if (! startPlaying (rig, args))
        {
            result.note = "no player";
            return result;
        }

        auto firstHandle = first->getLaunchHandle();
        auto secondHandle = second->getLaunchHandle();

        auto* context = rig.edit->getTransport().getCurrentPlaybackContext();

        if (firstHandle == nullptr || secondHandle == nullptr || context == nullptr)
        {
            result.note = "no handles";
            return result;
        }

        const auto syncPoint = context->getSyncPoint();

        if (! syncPoint.has_value())
        {
            result.note = "no sync point";
            return result;
        }

        /*  ONE READING OF THE SYNC POINT, both instants derived from it. Two
            readings would be two roundings, and the spacing between the launch
            and the boundary is the whole measurement. */
        /*  A QUARTER OF A BEAT AHEAD, not a whole one, and the two numbers have
            to be read together: the play has to LAND before the boundary can be
            queued behind it, and the boundary is `half` after the play. A whole
            beat ahead leaves a quarter of a second for the wait, which is what
            the first version of this spent reporting "the first range never
            started". At 60 bpm a quarter beat is 250 ms, which is six times the
            launch latency Go.dot itself budgets. */
        const auto launchAt = syncPoint->monotonicBeat.v.inBeats() + 0.25;
        const auto boundaryAt = launchAt + half;

        firstHandle->play (MonotonicBeat { BeatPosition::fromBeats (launchAt) });

        /*  THE PLAY IS LET LAND BEFORE THE PAIR IS QUEUED, and finding that out
            is what this variant cost. LaunchHandle keeps ONE queued state: a
            stop queued while a play is still queued CANCELS the play rather
            than following it (tracktion_LaunchHandle.cpp), so the first version
            of this queued a boundary onto a clip that was never going to start
            and then reported that it could not find the first half.

            Go.dot places a boundary the same way for the same reason: the range
            is sounding when its end is placed. */
        const auto blocksToWait = static_cast<int> ((0.75 * half * rig.sampleRate)
                                                     / rig.blockSize);

        bool started = false;

        for (int i = 0; i < blocksToWait && ! started; ++i)
        {
            rig.player->process (rig.blockSize);
            started = firstHandle->getPlayingStatus() == LaunchHandle::PlayState::playing;
        }

        if (! started)
        {
            result.note = "the first range never started";
            return result;
        }

        /*  THE PAIR. Both at the same instant, both placed ahead of it, which
            is what makes them land rather than arrive. */
        firstHandle->stop (MonotonicBeat { BeatPosition::fromBeats (boundaryAt) });
        secondHandle->play (MonotonicBeat { BeatPosition::fromBeats (boundaryAt) });

        const auto blocks = static_cast<int> ((3.5 * fileLengthSeconds * rig.sampleRate)
                                                / rig.blockSize);

        for (int i = 0; i < blocks; ++i)
            rig.player->process (rig.blockSize);

        const auto out = rig.player->getOutput();
        const auto halfSamples = static_cast<long long> (chirp.size()) / 2;
        const auto window = static_cast<size_t> (rig.sampleRate / 8.0);

        /*  SEARCHED AS WIDELY AS THE FIRST VARIANT IS, because where the sound
            starts is the launch jitter spike 04 measured and is not something
            this rig gets to assume. */
        const auto pre = alignWindow (out, 0, chirp, 0, window,
                                      static_cast<long long> (rig.sampleRate),
                                      2 * static_cast<long long> (rig.sampleRate));

        if (! pre.confident)
        {
            result.note = "could not find the first half";
            return result;
        }

        const auto expectedJoin = pre.offset + halfSamples;

        /*  The second half, matched against the reference's second half. If the
            pair landed on its sample the two alignments agree exactly. */
        const auto post = alignWindow (out, 0, chirp,
                                       static_cast<size_t> (halfSamples) + window, window,
                                       expectedJoin + static_cast<long long> (window),
                                       static_cast<long long> (rig.sampleRate / 20.0));

        result.reached = true;
        result.confident = post.confident;
        result.joinAt = expectedJoin;
        result.joinErrorSamples = (post.offset - static_cast<long long> (window))
                                    - (pre.offset + halfSamples);

        const auto start = pre.offset;
        const auto length = static_cast<long long> (chirp.size());

        result.damage = damageAround (out, 0, expectedJoin,
            [&chirp, start, length] (long long n) -> std::optional<double>
            {
                const auto index = n - start;

                if (index < 0 || index >= length)
                    return {};

                return static_cast<double> (chirp[static_cast<size_t> (index)]);
            });

        return result;
    }

    //==============================================================================
    void reportJoin (spike::Report& report, const std::string& prefix,
                     const JoinResult& join, int blockSize)
    {
        const auto key = [&prefix, blockSize] (const char* name)
        {
            return prefix + "_" + std::to_string (blockSize) + "_" + name;
        };

        report.value (key ("reached"), join.reached ? 1 : 0);

        if (! join.note.empty())
            report.value (key ("note"), join.note);

        if (! join.reached)
            return;

        report.value (key ("join_error_samples"), join.joinErrorSamples);
        report.value (key ("confident"), join.confident ? 1 : 0);
        report.value (key ("damaged_span_samples"), join.damage.span);
        report.value (key ("damaged_samples"), join.damage.count);
        report.value (key ("longest_damaged_run"), join.damage.longestRun);
        report.value (key ("silent_samples"), join.damage.silent);
        report.value (key ("damage_energy"), join.damage.energy);
        report.value (key ("worst_deviation"), join.damage.worst);
        report.value (key ("worst_at_offset"), join.damage.worstAt);
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    spike::makeAssertsNonInteractive();

    const auto parsed = spike::parseArgs (argc, argv);

    if (! parsed)
        return spike::usage ("spike03b_loop_joins", criterion, extraFlags);

    auto args = *parsed;

    spike::HeadlessEngine engine;
    spike::Report report ("spike03b_loop_joins", argc, argv);

    report.value ("sample_rate", args.sampleRate);
    report.value ("source", "linear chirp 200-2000 Hz, 2 s");

    const auto chirp = makeChirp (static_cast<double> (args.sampleRate), fileLengthSeconds);

    /*  FIVE BLOCK SIZES, because the artefact this is looking for arrives in
        units of a block: a stop that misses the block it shares with a wrap
        lands at the next block start, and how bad that is depends on how long a
        block is. 64 is what a show runs at its tightest and 1024 is what a
        laptop running a rehearsal is set to. */
    bool anyWrap = false;
    bool anyPlaced = false;
    long long worstWrapRun = 0;
    long long worstPlacedRun = 0;
    double worstWrapEnergy = 0.0;
    double worstPlacedEnergy = 0.0;

    for (const int blockSize : { 64, 128, 256, 512, 1024 })
    {
        args.buffer = static_cast<unsigned> (blockSize);

        const auto wrap = measureOwnWrap (engine, args, chirp);
        const auto relooped = measureSetLooping (engine, args, chirp);
        const auto placed = measurePlacedBoundary (engine, args, chirp);

        reportJoin (report, "wrap", wrap, blockSize);
        reportJoin (report, "setlooping", relooped, blockSize);
        reportJoin (report, "placed", placed, blockSize);

        /*  The worst configuration of each, on both measures. Not the span -
            first-to-last is inflated by isolated quantisation stragglers either
            side, and comparing two inflated numbers compares two amounts of
            inflation. */
        if (wrap.reached)
        {
            anyWrap = true;
            worstWrapRun = std::max (worstWrapRun, wrap.damage.longestRun);
            worstWrapEnergy = std::max (worstWrapEnergy, wrap.damage.energy);
        }

        if (placed.reached)
        {
            anyPlaced = true;
            worstPlacedRun = std::max (worstPlacedRun, placed.damage.longestRun);
            worstPlacedEnergy = std::max (worstPlacedEnergy, placed.damage.energy);
        }
    }

    /*  BOTH OR NOTHING. The question is which of two joins is cleaner, so one
        of them missing is not a result with a caveat - it is no result. The
        first version of this spike reported PASS with the placed boundary
        unmeasured, which is exactly the shape of a measurement that flatters
        whatever it did manage to run. */
    if (! anyWrap || ! anyPlaced)
        return report.cannotMeasure (anyWrap
                                       ? "the placed cross-slot boundary could not be brought"
                                         " about, so the wrap has nothing to be compared with"
                                       : "the clip's own wrap could not be brought about, so"
                                         " there is nothing to compare");

    report.value ("worst_wrap_damaged_run", worstWrapRun);
    report.value ("worst_placed_damaged_run", worstPlacedRun);
    report.value ("worst_wrap_energy", worstWrapEnergy);
    report.value ("worst_placed_energy", worstPlacedEnergy);

    /*  THE VERDICT IS A COMPARISON, not a threshold. What Phase 3 has to decide
        is whether a looping range can be left to wrap on its own or has to have
        every pass placed - and that is answered by which of the two is worse,
        at every block size, not by either one clearing a number. */
    const auto wrapIsFine = worstWrapEnergy <= worstPlacedEnergy;

    return report.verdict (wrapIsFine,
                           wrapIsFine
                             ? "the clip's own wrap is no worse than a boundary Go.dot places,"
                               " so a looping range is left alone"
                             : "the clip's own wrap is worse than a placed boundary, so every"
                               " pass of a looping range is placed instead");
}
