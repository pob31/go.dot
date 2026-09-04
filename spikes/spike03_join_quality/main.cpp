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
    SPIKE 03 — FOLLOW-ACTION JOIN QUALITY.  THROWAWAY CODE (devplan:19).

    PRD §6.1 item 3, verbatim, and this is the pass criterion:

        3. Follow-action join quality: sample-accurate? crossfade at the boundary
           achievable without a custom clip?

    Two questions, so two answers. The first is measured here. The second is
    settled by reading the graph builder and is a categorical NO - stated in the
    report with its citation, because a spike cannot measure the absence of an
    API.

    HOW "SAMPLE-ACCURATE" IS MADE MEASURABLE
    ---------------------------------------
    Clip A and clip B are the two halves of ONE continuous source file: A is
    [0, L/2) of it, B is [L/2, L) reached with Clip::setOffset. A carries a
    follow action to B. If the join is sample-accurate the output reconstructs
    the original file exactly, so the measurement is a comparison against
    material that already exists rather than against a theory of where the join
    should be.

    THE SOURCE IS A CHIRP, NOT A SINE, AND THAT IS LOAD-BEARING.
    Alignment is found by searching for the offset that best matches the
    reference. Against a pure sine that search is ambiguous modulo one period -
    at 1 kHz and 48 kHz, any error above 24 samples aliases into a small one and
    the spike would report near-perfect joins no matter how badly TE performed. A
    linear chirp is never self-similar, so the alignment is unique over the whole
    search range.

    THE MEASUREMENT, WHICH IS DELIBERATELY A DIFFERENCE OF TWO ALIGNMENTS
    --------------------------------------------------------------------
    Spike 04 established that TE's launch instant is not reproducible, so the
    absolute position of anything in this output jitters between runs. So:

      alignPre  = the offset that best matches the reference BEFORE the join
      alignPost = the offset that best matches the reference AFTER the join
      join_error_samples = alignPost - alignPre

    The launch jitter is common to both and cancels. A sample-accurate join gives
    exactly zero; a join that drops or repeats N samples gives N.

    WHAT THIS RIG CAN AND CANNOT RESOLVE
    ------------------------------------
    Both alignments are integer sample offsets, so join_error_samples is an
    integer and this spike can state "sample-accurate" or "off by N samples" and
    nothing finer. It cannot support a sub-sample claim, and it does not make
    one. (Spike 02's report records what happens when that line is crossed.)
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
        "Follow-action join quality: sample-accurate? crossfade at the boundary "
        "achievable without a custom clip?";

    constexpr auto extraFlags = " [--overlap] [--dump=PATH]";

    constexpr double fileLengthSeconds = 4.0;
    constexpr double chirpStartHz = 200.0;
    constexpr double chirpEndHz = 2000.0;
    constexpr float  amplitude = 0.5f;

    //==============================================================================
    /*  A linear chirp. Spike-specific, so it lives here rather than in the
        harness (the harness rule: if only one spike would use it, it belongs in
        that spike).

        Phase is integrated rather than computed per sample from an instantaneous
        frequency, because the naive form sin(2*pi*f(t)*t) is NOT a chirp - its
        instantaneous frequency is f(t) + t*f'(t), which sweeps at twice the
        intended rate and is not continuous across a join. Getting that wrong
        would put a phase discontinuity into the REFERENCE and this spike would
        then measure its own generator.
    */
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
    /*  Sum of squared differences between the output starting at `outStart` and
        the reference starting at `refStart`, over `len` samples.
    */
    double ssd (const choc::buffer::ChannelArrayBuffer<float>& out,
                choc::buffer::ChannelCount channel,
                choc::buffer::FrameCount outStart,
                const std::vector<float>& ref,
                size_t refStart,
                size_t len)
    {
        double total = 0.0;

        for (size_t i = 0; i < len; ++i)
        {
            const auto o = outStart + static_cast<choc::buffer::FrameCount> (i);
            const auto r = refStart + i;

            if (o >= out.getNumFrames() || r >= ref.size())
                return std::numeric_limits<double>::max();

            const auto d = static_cast<double> (out.getSample (channel, o)) - ref[r];
            total += d * d;
        }

        return total;
    }

    /*  The integer output offset at which the reference window starting at
        `refStart` best matches. Returns nullopt if nothing matched well enough to
        be believable - a match is only meaningful if it is clearly better than
        the alternatives, and a flat search surface means the window carried no
        information (silence, for instance).
    */
    struct Alignment
    {
        long long offset = 0;
        double residual = 0.0;
        bool confident = false;
    };

    Alignment alignWindow (const choc::buffer::ChannelArrayBuffer<float>& out,
                           choc::buffer::ChannelCount channel,
                           const std::vector<float>& ref,
                           size_t refStart,
                           size_t len,
                           choc::buffer::FrameCount searchCentre,
                           long long searchRadius)
    {
        Alignment best;
        best.residual = std::numeric_limits<double>::max();

        double secondBest = std::numeric_limits<double>::max();

        for (long long d = -searchRadius; d <= searchRadius; ++d)
        {
            const auto start = static_cast<long long> (searchCentre) + d;

            if (start < 0)
                continue;

            const auto s = ssd (out, channel, static_cast<choc::buffer::FrameCount> (start),
                                ref, refStart, len);

            if (s < best.residual)
            {
                secondBest = best.residual;
                best.residual = s;
                best.offset = start;
            }
            else if (s < secondBest)
            {
                secondBest = s;
            }
        }

        // A believable alignment is one where the winner is decisively better
        // than the runner-up. On silence, or on material with no unique
        // structure, every offset scores about the same and the "winner" is noise.
        best.confident = best.residual < 1.0e-6
                          || (secondBest < std::numeric_limits<double>::max()
                               && best.residual * 4.0 < secondBest);

        return best;
    }

    //==============================================================================
    /*  THE OVERLAP TEST — the precondition for Go.dot building its own crossfade.

        The join measured above is a BUTT join: A stops, B starts, nothing
        overlaps. Since §6.1 #3's crossfade turns out not to exist in Tracktion,
        Go.dot has to build one from two slots playing at once - and that is a
        different risk entirely. Two copies of the SAME file, overlapping, are
        only safe if they are sample-aligned with each other. Misalign them by a
        few samples and the sum is comb-filtered, which is far worse than the butt
        join it was meant to improve on.

        So: two clips on two tracks, summed to one bus, arranged so that during
        the overlap they are reading the SAME source samples.

          A: source [0, 3 s),  launched at beat N
          B: source [2 s, 4 s), launched at beat N+2

        At 60 bpm one beat is one second, so B starts exactly 2 s after A. During
        [N+2, N+3) A is playing source 2-3 s and B is playing source 0-1 s of its
        own view, which IS source 2-3 s. Identical material, in phase.

        If they are aligned the sum is exactly 2x the source. If they are off by
        N samples the sum is source(t) + source(t-N), which is a comb filter, and
        the residual against 2x source measures precisely that error.

        Both launches are derived from ONE reading of the sync point, so the two
        beats are exactly two apart rather than two separate roundings.
    */
    struct OverlapResult
    {
        double maxDevFromDouble = 0.0;   // max |out - 2*ref| inside the overlap
        double peakInOverlap = 0.0;
        long long bestLagSamples = 0;    // lag that best explains the sum, 0 = aligned
        long long clipAOriginFrame = 0;
        bool measured = false;
    };

    OverlapResult runOverlap (spike::HeadlessEngine& harness, const spike::Args& args,
                              const std::vector<float>& chirp)
    {
        OverlapResult result;
        auto& engine = *harness;

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 2;
        params.outputChannels = 2;

        auto edit = test_utilities::createTestEdit (engine, 2, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.size() < 2)
            return result;

        juce::AudioBuffer<float> buf (2, static_cast<int> (chirp.size()));

        for (int c = 0; c < 2; ++c)
            std::copy (chirp.begin(), chirp.end(), buf.getWritePointer (c));

        auto file = spike::writeWav (buf, params.sampleRate);

        if (file == nullptr)
            return result;

        const AudioFile audio (engine, file->getFile());

        Clip::Ptr clips[2];

        for (int t = 0; t < 2; ++t)
        {
            auto& list = tracks[t]->getClipSlotList();
            list.ensureNumberOfSlots (1);

            auto c = insertWaveClip (*list.getClipSlots()[0], {}, file->getFile(),
                                     { { 0_tp, TimeDuration::fromSeconds (t == 0 ? 3.0 : 2.0) } },
                                     DeleteExistingClips::no);

            if (c == nullptr)
                return result;

            // Disable looping BEFORE setting the offset - disableLooping()
            // overwrites it (AudioClipBase.cpp:916).
            c->disableLooping();

            if (t == 1)
                c->setOffset (TimeDuration::fromSeconds (2.0));

            clips[t] = c;
        }

        bool mappedOk = false;
        auto player = spike::createPlayerWithDeadline (*edit, params, { audio }, mappedOk);

        if (player == nullptr || ! mappedOk)
            return result;

        const auto blockSize = params.blockSize;
        player->process (blockSize);

        // Both derived from one sync-point read: exactly two beats apart.
        if (! spike::launchAtBeatOffset (*clips[0], 1.0)) return result;
        if (! spike::launchAtBeatOffset (*clips[1], 3.0)) return result;

        const auto blocks = static_cast<int> ((8.0 * params.sampleRate) / blockSize);

        for (int i = 0; i < blocks; ++i)
            player->process (blockSize);

        const auto out = player->getOutput();

        /*  LOCATE A rather than assuming where it starts.

            A is launched at beat 1, which at 60 bpm "should" be 1 s - but
            assuming that turns any launch offset into an apparent lag, and the
            first version of this test duly reported -2 samples of comb filtering
            that was really -2 samples of my own arithmetic. Align the solo region
            (before B enters) against the reference and derive everything from
            what is measured.
        */
        const auto sr = params.sampleRate;
        const auto win = static_cast<size_t> (sr * 0.05);

        const auto aStart = alignWindow (out, 0, chirp,
                                         static_cast<size_t> (sr * 0.5), win,
                                         static_cast<choc::buffer::FrameCount> (sr * 1.5),
                                         static_cast<long long> (sr * 1.4));

        if (! aStart.confident)
            return result;

        // aStart.offset is where source 0.5 s appears, so A's origin is that
        // minus 0.5 s of source.
        const auto aOrigin = aStart.offset - static_cast<long long> (sr * 0.5);
        result.clipAOriginFrame = aOrigin;

        // The overlap is [aOrigin + 2 s, aOrigin + 3 s); both play source 2-3 s.
        const auto from = static_cast<choc::buffer::FrameCount> (aOrigin + static_cast<long long> (sr * 2.05));
        const auto to   = static_cast<choc::buffer::FrameCount> (aOrigin + static_cast<long long> (sr * 2.95));
        const auto refBase = static_cast<size_t> (sr * 2.05);

        double worst = 0.0, peak = 0.0;

        for (auto f = from; f < std::min (to, out.getNumFrames()); ++f)
        {
            const auto r = refBase + static_cast<size_t> (f - from);

            if (r >= chirp.size())
                break;

            const auto o = static_cast<double> (out.getSample (0, f));
            peak = std::max (peak, std::abs (o));
            worst = std::max (worst, std::abs (o - 2.0 * chirp[r]));
        }

        result.maxDevFromDouble = worst;
        result.peakInOverlap = peak;

        /*  If they were misaligned, out(t) = ref(t) + ref(t-lag). Search the lag
            that best explains the sum; 0 means aligned. This distinguishes "in
            sync" from "in sync by luck of the residual threshold".
        */
        double bestScore = std::numeric_limits<double>::max();

        for (long long lag = -64; lag <= 64; ++lag)
        {
            double score = 0.0;

            for (auto f = from; f < std::min (to, out.getNumFrames()); f += 4)
            {
                const auto r = static_cast<long long> (refBase) + static_cast<long long> (f - from);
                const auto rl = r - lag;

                if (r < 0 || rl < 0 || static_cast<size_t> (r) >= chirp.size()
                     || static_cast<size_t> (rl) >= chirp.size())
                    continue;

                const auto model = static_cast<double> (chirp[static_cast<size_t> (r)])
                                     + chirp[static_cast<size_t> (rl)];
                const auto d = static_cast<double> (out.getSample (0, f)) - model;
                score += d * d;
            }

            if (score < bestScore) { bestScore = score; result.bestLagSamples = lag; }
        }

        result.measured = true;
        return result;
    }

    //==============================================================================
    struct RunResult
    {
        choc::buffer::ChannelArrayBuffer<float> output { choc::buffer::Size::create (0, 0) };
        bool measured = false;
        bool clipALooping = false;
    };

    RunResult runOnce (spike::HeadlessEngine& harness, const spike::Args& args,
                       const std::vector<float>& chirp, bool withFollowAction)
    {
        RunResult result;
        auto& engine = *harness;

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 2;
        params.outputChannels = 2;

        auto edit = test_utilities::createTestEdit (engine, 1, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.isEmpty())
            return result;

        // Write the chirp out as the single source both clips read from.
        juce::AudioBuffer<float> buf (2, static_cast<int> (chirp.size()));

        for (int c = 0; c < 2; ++c)
            std::copy (chirp.begin(), chirp.end(), buf.getWritePointer (c));

        auto file = spike::writeWav (buf, params.sampleRate);

        if (file == nullptr)
            return result;

        const AudioFile audio (engine, file->getFile());

        auto& slots = tracks[0]->getClipSlotList();
        slots.ensureNumberOfSlots (2);
        auto slotArray = slots.getClipSlots();

        const auto half = fileLengthSeconds / 2.0;

        // A = the first half of the file.
        auto clipA = insertWaveClip (*slotArray[0], {}, file->getFile(),
                                     { { 0_tp, TimeDuration::fromSeconds (half) } },
                                     DeleteExistingClips::no);

        // B = the second half of the SAME file, reached with an offset. Spike 02
        // established that the offset is honoured to the nearest sample, which is
        // what makes the reconstruction argument valid.
        auto clipB = insertWaveClip (*slotArray[1], {}, file->getFile(),
                                     { { 0_tp, TimeDuration::fromSeconds (half) } },
                                     DeleteExistingClips::no);

        if (clipA == nullptr || clipB == nullptr)
            return result;

        /*  ORDER MATTERS HERE, AND GETTING IT WRONG IS SILENT.

            disableLooping() OVERWRITES the clip's offset:

                pos.offset = toDuration (loopStart);         // AudioClipBase.cpp:916

            so calling it after setOffset() throws the offset away and the clip
            plays from the start of the file instead. That is exactly what
            happened here first time round: B played source position 0 rather than
            2 s, the reconstruction never matched, and the spike reported "the
            follow action did not fire" - when the follow action had fired
            perfectly and it was the offset that had been wiped. Four seconds of
            continuous audio for two two-second clips is what finally gave it away.

            Launcher clips loop by default (measured: clipA_is_looping was 1
            before this call), and looping matters because it selects which branch
            of the follow-action duration logic applies: the looping branch depends
            on the loop range and followActionNumLoops, while the non-looping
            branch is simply `clipDuration = length - offset`. This spike wants the
            latter - play once, then hand over.

            So: disable looping FIRST, then set the offset.
        */
        clipA->disableLooping();
        clipB->disableLooping();

        clipB->setOffset (TimeDuration::fromSeconds (half));

        if (withFollowAction)
        {
            /*  "Play to the end of the clip, then go to the next slot on this
                track." For a NON-looping clip the graph reads
                clipDuration = length - offset for the loops duration type
                (tracktion_EditNodeBuilder.cpp:987-995), i.e. play once then act.
            */
            clipA->followActionDurationType = Clip::FollowActionDurationType::loops;

            /*  followActionNumLoops MUST be set, and the reason is a branch in
                tracktion_EditNodeBuilder.cpp:987-995:

                    case FollowActionDurationType::loops:
                        if (clip->isLooping())
                        {
                            if (auto afterLoops = followActionNumLoops.get(); afterLoops > 0.0)
                                clipDuration = (loopLength * afterLoops) - offset;
                        }
                        else
                            clipDuration = length - offset;

                For a LOOPING clip with numLoops still at its default of 0, that
                inner `if` never fires, `clipDuration` stays nullopt, and the
                SlotControlNode is built with no duration - so the follow action
                never triggers and the clip loops for ever. Launcher clips loop by
                default, so this is the path a first attempt lands on, and its
                symptom is not "no audio" but "audio that never stops and never
                matches", which reads like a routing or reference bug.
            */
            clipA->followActionNumLoops = 1.0;

            if (auto* fa = clipA->getFollowActions())
            {
                auto& action = fa->addAction();
                action.action = FollowAction::trackNext;
                action.weight = 1.0;
            }
        }

        result.clipALooping = clipA->isLooping();

        bool mappedOk = false;
        auto player = spike::createPlayerWithDeadline (*edit, params, { audio }, mappedOk);

        if (player == nullptr || ! mappedOk)
            return result;

        const auto blockSize = params.blockSize;
        player->process (blockSize);

        if (! spike::launchAtNextWholeBeat (*clipA))
            return result;

        // Long enough for A, the follow action, and all of B.
        const auto blocks = static_cast<int> (((fileLengthSeconds + 3.0) * params.sampleRate) / blockSize);

        for (int i = 0; i < blocks; ++i)
            player->process (blockSize);

        result.output = player->getOutput();
        result.measured = true;
        return result;
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    spike::makeAssertsNonInteractive();

    const auto parsed = spike::parseArgs (argc, argv);

    if (! parsed)
        return spike::usage ("spike03_join_quality", criterion, extraFlags);

    const auto args = *parsed;

    spike::HeadlessEngine engine;
    spike::Report report ("spike03_join_quality", argc, argv);

    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("source", "linear chirp 200-2000 Hz, 4 s, split in half");

    const auto chirp = makeChirp (static_cast<double> (args.sampleRate), fileLengthSeconds);
    const auto sr = static_cast<double> (args.sampleRate);
    const auto halfSamples = static_cast<size_t> (sr * fileLengthSeconds / 2.0);

    if (spike::hasFlag (argc, argv, "--overlap"))
    {
        const auto ov = runOverlap (engine, args, chirp);

        if (! ov.measured)
            return report.cannotMeasure ("could not set up the overlap pair");

        report.value ("overlap.max_dev_from_2x_source", ov.maxDevFromDouble);
        report.value ("overlap.peak_in_overlap", ov.peakInOverlap);
        report.value ("overlap.best_lag_samples", ov.bestLagSamples);
        report.value ("overlap.clipA_origin_frame", ov.clipAOriginFrame);

        // Aligned means the sum IS 2x the source and the best-fit lag is zero.
        // A non-zero lag here is comb filtering, and the lag is its period.
        const bool aligned = ov.bestLagSamples == 0 && ov.maxDevFromDouble < 0.01;

        return report.verdict (aligned,
                               aligned
                                 ? "two overlapping copies of the same file are sample-aligned: the sum is 2x the source, no comb filtering"
                                 : "overlapping copies are NOT aligned - see overlap.best_lag_samples, this is comb filtering");
    }

    const auto run = runOnce (engine, args, chirp, true);

    if (! run.measured)
        return report.cannotMeasure ("could not set up the follow-action pair");

    /*  --dump=<path> writes the raw output and the reference side by side, so a
        stuck measurement can be looked at rather than guessed at. Diagnostic
        only; nothing in the verdict depends on it.
    */
    if (const auto dumpPath = spike::textFor (argc, argv, "--dump="))
    {
        juce::AudioBuffer<float> b (2, static_cast<int> (run.output.getNumFrames()));

        for (choc::buffer::FrameCount f = 0; f < run.output.getNumFrames(); ++f)
        {
            b.setSample (0, static_cast<int> (f), run.output.getSample (0, f));
            b.setSample (1, static_cast<int> (f),
                         static_cast<size_t> (f) < chirp.size() ? chirp[static_cast<size_t> (f)] : 0.0f);
        }

        juce::WavAudioFormat fmt;
        const juce::File out { juce::String (*dumpPath) };   // braces: (...) is a function decl
        out.deleteFile();

        if (auto* os = out.createOutputStream().release())
        {
            if (auto* w = fmt.createWriterFor (os, sr, 2, 24, {}, 0))
            {
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                delete w;
                report.value ("dump_written", out.getFullPathName().toStdString());
            }
        }
    }

    // CONTROL: an identical run must be bit-identical, or nothing below means
    // anything. Same lesson as spike 04.
    const auto control = runOnce (engine, args, chirp, true);

    if (! control.measured)
        return report.cannotMeasure ("control run could not be set up");

    double worst = 0.0;
    const auto frames = std::min (run.output.getNumFrames(), control.output.getNumFrames());

    for (choc::buffer::FrameCount f = 0; f < frames; ++f)
        worst = std::max (worst, std::abs (static_cast<double> (run.output.getSample (0, f))
                                            - control.output.getSample (0, f)));

    report.value ("control_max_abs_diff", worst);

    if (worst > 0.0)
        return report.cannotMeasure (
            "two identical runs differ, so any join measurement would be measuring the "
            "harness rather than the engine");

    // Where in the OUTPUT is the join, roughly? Find where A starts, then step
    // half the file forward. The search then only has to cover the error, not the
    // whole timeline.
    const auto window = static_cast<size_t> (sr * 0.05);          // 50 ms
    const auto radius = static_cast<long long> (sr * 0.05);       // +/- 50 ms

    // Coarse: locate A's start anywhere in the first few seconds.
    const auto coarse = alignWindow (run.output, 0, chirp,
                                     0, window,
                                     static_cast<choc::buffer::FrameCount> (sr * 1.5),
                                     static_cast<long long> (sr * 1.5));

    if (! coarse.confident)
        return report.cannotMeasure ("could not locate the start of clip A in the output");

    report.value ("clipA_start_frame", coarse.offset);
    report.value ("clipA_is_looping", run.clipALooping ? 1 : 0);

    // PRE-join: a window ending just before the boundary.
    const auto preRefStart = halfSamples - window - static_cast<size_t> (sr * 0.01);
    const auto pre = alignWindow (run.output, 0, chirp, preRefStart, window,
                                  static_cast<choc::buffer::FrameCount> (coarse.offset + static_cast<long long> (preRefStart)),
                                  radius);

    // POST-join: a window starting just after the boundary.
    /*  The post-join search is WIDE on purpose - +/- 1.5 s rather than the
        +/- 50 ms used before the join.

        A narrow window assumes B starts where the join is expected. If B is
        instead launched at the next quantised beat, or after any gap at all, a
        narrow window spans silence-then-B and matches nothing - which reads as
        "the follow action never fired" when in fact it fired late. Searching
        wide finds B wherever it is, and the distance to where it was expected
        becomes the measurement rather than a failure.
    */
    const auto postRefStart = halfSamples + static_cast<size_t> (sr * 0.01);
    const auto post = alignWindow (run.output, 0, chirp, postRefStart, window,
                                   static_cast<choc::buffer::FrameCount> (coarse.offset + static_cast<long long> (postRefStart)),
                                   static_cast<long long> (sr * 1.5));

    report.value ("pre_join.confident", pre.confident ? 1 : 0);
    report.value ("post_join.confident", post.confident ? 1 : 0);

    /*  Did B play at all? "Could not align" and "there was nothing to align"
        are different failures and the report must not conflate them.
    */
    auto peakOver = [&] (choc::buffer::FrameCount from, choc::buffer::FrameCount to)
    {
        double m = 0.0;

        for (auto f = from; f < std::min (to, run.output.getNumFrames()); ++f)
            m = std::max (m, std::abs (static_cast<double> (run.output.getSample (0, f))));

        return m;
    };

    const auto expectedJoin = static_cast<choc::buffer::FrameCount> (coarse.offset)
                                + static_cast<choc::buffer::FrameCount> (halfSamples);
    const auto second = static_cast<choc::buffer::FrameCount> (sr);

    report.value ("peak_before_join", peakOver (expectedJoin > second ? expectedJoin - second : 0, expectedJoin));
    report.value ("peak_after_join",  peakOver (expectedJoin, expectedJoin + second));
    report.value ("peak_after_join_plus_1s", peakOver (expectedJoin + second, expectedJoin + 2 * second));
    report.value ("total_frames", static_cast<long long> (run.output.getNumFrames()));

    /*  If the post-join window will not align where it is expected, ask the
        output what it IS playing: scan the whole reference for the source
        position that best matches. "Nothing there" and "the wrong part of the
        file" are different findings and this is what separates them.
    */
    if (! post.confident)
    {
        const auto outStart = expectedJoin + static_cast<choc::buffer::FrameCount> (sr * 0.01);

        size_t bestRef = 0;
        double bestScore = std::numeric_limits<double>::max();

        for (size_t r = 0; r + window < chirp.size(); r += 64)
        {
            const auto sc = ssd (run.output, 0, outStart, chirp, r, window);

            if (sc < bestScore) { bestScore = sc; bestRef = r; }
        }

        for (size_t r = bestRef > 64 ? bestRef - 64 : 0; r < bestRef + 64 && r + window < chirp.size(); ++r)
        {
            const auto sc = ssd (run.output, 0, outStart, chirp, r, window);

            if (sc < bestScore) { bestScore = sc; bestRef = r; }
        }

        report.value ("post_join.best_source_frame", static_cast<long long> (bestRef));
        report.value ("post_join.best_source_seconds", static_cast<double> (bestRef) / sr);
        report.value ("post_join.best_residual", bestScore);
        report.value ("expected_source_frame", static_cast<long long> (halfSamples));
    }

    if (! pre.confident || ! post.confident)
        return report.cannotMeasure (
            "one side of the join could not be aligned against the reference - most likely "
            "the follow action did not fire, so there is no second half to align");

    const auto preAlign  = pre.offset - static_cast<long long> (preRefStart);
    const auto postAlign = post.offset - static_cast<long long> (postRefStart);
    const auto joinError = postAlign - preAlign;

    report.value ("join_error_samples", joinError);
    report.value ("pre_join.residual", pre.residual);
    report.value ("post_join.residual", post.residual);

    /*  The audible artefact, independent of the alignment argument: the largest
        sample-to-sample step near the join, against the largest step elsewhere.
        A chirp has a bounded slew rate, so a discontinuity stands out.
    */
    auto maxStep = [&] (choc::buffer::FrameCount from, choc::buffer::FrameCount to)
    {
        double m = 0.0;

        for (auto f = from + 1; f < std::min (to, run.output.getNumFrames()); ++f)
            m = std::max (m, std::abs (static_cast<double> (run.output.getSample (0, f))
                                        - run.output.getSample (0, f - 1)));
        return m;
    };

    const auto joinFrame = static_cast<choc::buffer::FrameCount> (preAlign + static_cast<long long> (halfSamples));
    const auto guard = static_cast<choc::buffer::FrameCount> (sr * 0.002);

    const auto stepAtJoin  = maxStep (joinFrame > guard ? joinFrame - guard : 0, joinFrame + guard);
    const auto stepBaseline = maxStep (joinFrame > static_cast<choc::buffer::FrameCount> (sr * 0.5)
                                          ? joinFrame - static_cast<choc::buffer::FrameCount> (sr * 0.5) : 0,
                                       joinFrame > guard ? joinFrame - guard : 0);

    /*  THE JOIN REGION ITSELF.

        The two residual windows above sit 10 ms either side of the boundary, so
        they establish that the halves are correctly POSITIONED but say nothing
        about the boundary sample itself. TE inserts click-suppression fades that
        clip fade nodes do not account for (clip fades are skipped entirely for
        launcher clips - tracktion_EditNodeBuilder.cpp:637), so any artefact lives
        exactly in the gap those windows leave.

        This walks the join region sample by sample against the reference and
        reports how far it deviates and for how long. That span IS the fade, and
        its length is the number Phase 3 needs when it decides what a range
        boundary sounds like.
    */
    {
        const auto span = static_cast<choc::buffer::FrameCount> (sr * 0.05);   // +/- 50 ms
        const auto from = joinFrame > span ? joinFrame - span : 0;
        const auto to   = std::min (joinFrame + span, run.output.getNumFrames());

        double maxDev = 0.0;
        long long firstBad = -1, lastBad = -1;
        constexpr double devThreshold = 1.0e-4;

        for (auto f = from; f < to; ++f)
        {
            const auto refIndex = static_cast<long long> (f) - preAlign;

            if (refIndex < 0 || static_cast<size_t> (refIndex) >= chirp.size())
                continue;

            const auto dev = std::abs (static_cast<double> (run.output.getSample (0, f))
                                        - chirp[static_cast<size_t> (refIndex)]);
            maxDev = std::max (maxDev, dev);

            if (dev > devThreshold)
            {
                if (firstBad < 0)
                    firstBad = static_cast<long long> (f);

                lastBad = static_cast<long long> (f);
            }
        }

        report.value ("join_region.max_deviation", maxDev);
        report.value ("join_region.max_deviation_dbfs",
                      maxDev > 0.0 ? 20.0 * std::log10 (maxDev) : -999.0);
        report.value ("join_region.artifact_samples", firstBad < 0 ? 0LL : lastBad - firstBad + 1);
        report.value ("join_region.artifact_ms",
                      firstBad < 0 ? 0.0 : 1000.0 * static_cast<double> (lastBad - firstBad + 1) / sr);
        report.value ("join_region.artifact_start_rel_join",
                      firstBad < 0 ? 0LL : firstBad - static_cast<long long> (joinFrame));
    }

    report.value ("step_at_join", stepAtJoin);
    report.value ("step_baseline", stepBaseline);
    report.value ("step_ratio", stepBaseline > 0.0 ? stepAtJoin / stepBaseline : -1.0);

    const bool sampleAccurate = joinError == 0;

    return report.verdict (sampleAccurate,
                           sampleAccurate
                             ? "the follow-action join is sample-accurate: the two halves reconstruct the source exactly"
                             : "the follow-action join is not sample-accurate - see join_error_samples");
}
