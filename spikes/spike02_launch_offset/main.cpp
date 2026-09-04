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
    SPIKE 02 — LAUNCHER START AT AN ARBITRARY IN-FILE OFFSET.
    THROWAWAY CODE (devplan:19).

    PRD §6.1 item 2, verbatim, and this is the pass criterion:

        2. **Launcher start at an arbitrary in-file offset** — load-to-time
           depends on it; if absent, this is the one genuine gap.

    THE PREMISE OF THAT ITEM IS FALSE, AND THAT IS THIS SPIKE'S FIRST FINDING
    ------------------------------------------------------------------------
    It is not absent. EditNodeBuilder honours the clip offset on the launcher
    path explicitly (tracktion_EditNodeBuilder.cpp:550-577):

        if (role == ClipRole::launcher)
            WaveNodeRealTime::BeatConfig config { .offset = clip.getOffsetInBeats(), ... }

    So the question worth spending a spike on is not "can it?" but "how
    accurately, and what does it cost?" - because the answer to the second half
    is what load-to-time (PRD §3.13) actually has to be built on.

    HOW ACCURACY IS MEASURED, AND WHY IT IS MEASURED THIS WAY
    ---------------------------------------------------------
    Spike 04 established that TE's launch INSTANT is not reproducible run to run:
    the sync point is influenced by transport start rather than purely by
    processed blocks, so the same launch lands on a different sample each time.
    Measuring "where did the transient land, in absolute samples" would therefore
    measure that jitter and not the offset.

    So every offset is launched in ONE run, on its own track, routed to its own
    stereo bus, at the SAME MonotonicBeat. Whatever the launch jitter J is, it is
    common to all of them, and it cancels in the DIFFERENCES:

        measured(o) - measured(0)  ==  -o * sampleRate     (exactly)

    A file with a single non-zero sample makes the position unambiguous, and the
    tolerance is 5 samples - the same tolerance TE uses in its own PDC test
    (tracktion_Plugins.test.cpp:60).

    THE COST, WHICH IS THE REAL DELIVERABLE
    ---------------------------------------
    IDs::offset is in Edit::TreeWatcher's restart list (tracktion_Edit.cpp:147-150),
    so setting an offset on a LIVE graph rebuilds it. LaunchHandle::nudge is the
    rebuild-free alternative. Both are measured here, because a load-to-time that
    rebuilds the graph per cue is not a load-to-time anyone can use mid-show.
*/

#include "../SpikeHarness.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace tracktion;
using namespace tracktion::engine;

namespace
{
    constexpr auto criterion =
        "Launcher start at an arbitrary in-file offset - load-to-time depends on it.";

    // No extra flags: the ladder is fixed, and --tracks only has to be big
    // enough to hold one track per offset.
    constexpr auto extraFlags = "";

    /*  Spike mechanics, not product decisions.

        0.1234567 s is the one that earns its place: at 48 kHz it is 5925.9216
        samples and at 96 kHz 11851.8432, so it is not an integer sample count at
        either rate, let alone a whole block or beat. An offset implementation
        that quietly rounds to a block boundary passes a ladder of round numbers
        and fails this one. (1/3 s is NOT such a value - at 48 kHz it is exactly
        16000 samples - which is why it is not used here.)
    */
    const std::vector<double> offsetsSeconds { 0.0, 0.25, 0.1234567, 0.5, 0.75 };

    constexpr double fileLengthSeconds = 4.0;
    constexpr double transientAtSeconds = 2.0;

    //==============================================================================
    /*  First frame on `channel` whose magnitude exceeds the threshold. The file
        is silent apart from one sample, so this is unambiguous.
    */
    std::optional<choc::buffer::FrameCount>
    firstNonZeroOnChannel (const choc::buffer::ChannelArrayBuffer<float>& b,
                           choc::buffer::ChannelCount channel,
                           float threshold = 1.0e-4f)
    {
        if (channel >= b.getNumChannels())
            return {};

        for (choc::buffer::FrameCount f = 0; f < b.getNumFrames(); ++f)
            if (std::abs (b.getSample (channel, f)) > threshold)
                return f;

        return {};
    }

    //==============================================================================
    struct OffsetResult
    {
        double offsetSeconds = 0.0;
        std::optional<choc::buffer::FrameCount> measured;
        double errorSamples = 0.0;
    };

    struct RunResult
    {
        std::vector<OffsetResult> offsets;
        int rebuildsForSetOffset = -1;
        int rebuildsForNudge = -1;
        bool measured = false;
    };

    RunResult runOnce (spike::HeadlessEngine& harness, const spike::Args& args)
    {
        RunResult result;
        auto& engine = *harness;

        const auto numOffsets = static_cast<int> (offsetsSeconds.size());

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 2;
        params.outputChannels = 2 * numOffsets;

        // One stereo bus per offset, so each launch is measured in isolation and
        // the common launch jitter cancels between them.
        harness.behaviour->describeWaveDevicesFn =
            [numOffsets] (std::vector<WaveDeviceDescription>& descs, juce::AudioIODevice&, bool isInput)
            {
                descs.clear();

                if (isInput)
                {
                    descs.emplace_back ("spike-in", 0, 1, true);
                    return;
                }

                for (int i = 0; i < numOffsets; ++i)
                    descs.emplace_back ("bus" + juce::String (i), i * 2, i * 2 + 1, true);
            };

        auto edit = test_utilities::createTestEdit (engine, numOffsets, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.size() < numOffsets)
            return result;

        auto transientFile = spike::makeTransientFile (params.sampleRate, fileLengthSeconds,
                                                       transientAtSeconds, 0.5f, 2);

        if (transientFile == nullptr)
            return result;

        const AudioFile transientAudio (engine, transientFile->getFile());

        std::vector<ClipSlot*> slots;

        for (int i = 0; i < numOffsets; ++i)
        {
            auto& list = tracks[i]->getClipSlotList();
            list.ensureNumberOfSlots (1);
            auto* slot = list.getClipSlots()[0];

            insertWaveClip (*slot, {}, transientFile->getFile(),
                            { { 0_tp, TimeDuration::fromSeconds (fileLengthSeconds) } },
                            DeleteExistingClips::no);

            /*  The offset is set BEFORE the graph exists, which is the cheap
                path: no playback context, nothing to rebuild. Setting it on a
                live graph is measured separately below, because that is the case
                load-to-time actually faces.
            */
            if (auto* clip = slot->getClip())
                clip->setOffset (TimeDuration::fromSeconds (offsetsSeconds[static_cast<size_t> (i)]));

            slots.push_back (slot);
        }

        spike::RebuildCounter rebuilds;

        bool mappedOk = false;
        auto player = spike::createPlayerWithDeadline (*edit, params, { transientAudio }, mappedOk);

        if (player == nullptr || ! mappedOk)
            return result;

        // Route each track to its own bus. Structural, so it happens before any
        // rebuild measurement that matters.
        {
            auto& dm = engine.getDeviceManager();

            if (dm.getNumWaveOutDevices() < numOffsets)
                return result;

            for (int i = 0; i < numOffsets; ++i)
                tracks[i]->getOutput().setOutputToDeviceID (dm.getWaveOutDevice (i)->getDeviceID());

            edit->dispatchPendingUpdatesSynchronously();
        }

        const auto blockSize = params.blockSize;

        // A sync point must exist before anything can be launched.
        player->process (blockSize);

        // ALL of them at the same beat. This is what makes the jitter common.
        for (auto* slot : slots)
            if (auto* clip = slot->getClip())
                if (! spike::launchAtNextWholeBeat (*clip))
                    return result;

        // Long enough for the latest transient (offset 0 -> 2 s after launch)
        // plus the beat the launch was quantised to.
        const auto blocks = static_cast<int> ((5.0 * params.sampleRate) / blockSize);

        for (int i = 0; i < blocks; ++i)
            player->process (blockSize);

        const auto output = player->getOutput();

        for (int i = 0; i < numOffsets; ++i)
        {
            OffsetResult r;
            r.offsetSeconds = offsetsSeconds[static_cast<size_t> (i)];
            r.measured = firstNonZeroOnChannel (output, static_cast<choc::buffer::ChannelCount> (i * 2));
            result.offsets.push_back (r);
        }

        // Errors are relative to the zero-offset bus, so the common launch jitter
        // cancels. If bus 0 found nothing there is no reference and no measurement.
        if (! result.offsets.empty() && result.offsets[0].measured)
        {
            const auto reference = static_cast<double> (*result.offsets[0].measured);

            for (auto& r : result.offsets)
            {
                if (! r.measured)
                    continue;

                const auto expectedDelta = -r.offsetSeconds * params.sampleRate;
                const auto actualDelta   = static_cast<double> (*r.measured) - reference;
                r.errorSamples = actualDelta - expectedDelta;
            }
        }

        /*  THE COST MEASUREMENT.

            setOffset on a live graph: IDs::offset is in Edit::TreeWatcher's
            restart list, so this is expected to be non-zero. nudge: expected to
            be zero. Both are what load-to-time has to choose between.
        */
        rebuilds.mark();

        if (auto* clip = slots[0]->getClip())
            clip->setOffset (TimeDuration::fromSeconds (0.1));

        edit->dispatchPendingUpdatesSynchronously();
        player->process (blockSize);
        result.rebuildsForSetOffset = rebuilds.delta();

        rebuilds.mark();

        if (auto* clip = slots[0]->getClip())
            if (auto handle = clip->getLaunchHandle())
                handle->nudge (BeatDuration::fromBeats (0.25));

        edit->dispatchPendingUpdatesSynchronously();
        player->process (blockSize);
        result.rebuildsForNudge = rebuilds.delta();

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
        return spike::usage ("spike02_launch_offset", criterion, extraFlags);

    const auto args = *parsed;
    const auto needed = static_cast<long> (offsetsSeconds.size());

    if (args.tracks < needed)
    {
        std::cerr << "spike02_launch_offset: --tracks must be at least " << needed << "\n"
                  << "This spike puts one offset on each track and launches them together,\n"
                     "so that the launch jitter is common to all of them and cancels in the\n"
                     "differences. Fewer tracks than offsets is not a smaller experiment,\n"
                     "it is a different one.\n";

        return spike::usageError;
    }

    spike::HeadlessEngine engine;
    spike::Report report ("spike02_launch_offset", argc, argv);

    report.value ("tracks_requested", args.tracks);
    report.value ("tracks_used", needed);

    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("offsets", offsetsSeconds.size());
    report.value ("transient_at_s", transientAtSeconds);

    const auto run = runOnce (engine, args);

    if (! run.measured)
        return report.cannotMeasure ("could not set up the offset ladder");

    double worstError = 0.0;
    bool allFound = true;

    for (const auto& r : run.offsets)
    {
        const auto tag = "offset_" + std::to_string (r.offsetSeconds).substr (0, 5);

        if (r.measured)
        {
            report.value (tag + ".measured_sample", *r.measured);
            report.value (tag + ".error_samples", r.errorSamples);
            worstError = std::max (worstError, std::abs (r.errorSamples));
        }
        else
        {
            report.value (tag + ".measured_sample", "not-found");
            allFound = false;
        }
    }

    report.value ("max_abs_error_samples", worstError);
    report.value ("rebuilds.setOffset_live", run.rebuildsForSetOffset);
    report.value ("rebuilds.nudge", run.rebuildsForNudge);

    if (! allFound)
        return report.cannotMeasure ("a transient was not found on every bus, so the ladder is incomplete");

    // 5 samples: the tolerance TE holds itself to in tracktion_Plugins.test.cpp:60.
    const bool accurate = worstError <= 5.0;

    return report.verdict (accurate,
                           accurate
                             ? "the launcher honours an arbitrary in-file offset to within 5 samples"
                             : "the launcher did not place at least one offset within 5 samples");
}
