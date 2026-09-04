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
    SPIKE 04 — GRAPH STABILITY.  THROWAWAY CODE (devplan:19).

    PRD §6.1 item 4, verbatim, and this is the pass criterion:

        4. **Graph stability under sustained launching with a fixed track set**
           — no rebuild, no crossfade tax on already-playing material.

    The devplan makes this the FIRST spike to run, because it validates the
    polyphony model of PRD §3.25. PRD §9.2 says "the polyphony model stands
    unless #2 or #4 fails" — and #2's premise turned out to be false (the
    launcher does honour an arbitrary in-file offset), so THIS spike is now the
    whole gate. If the graph rebuilds on every launch, or already-playing
    material pays a crossfade whenever a new clip starts, the "Go.dot owns time,
    TE is the player" inversion needs amending before Phase 2 is designed on it.

    HOW THE TWO HALVES OF THE CRITERION ARE MEASURED
    ------------------------------------------------
    "no rebuild" — EditNodeBuilder::insertOptionalLastStageNode is a public
    static hook TE calls on every graph build. spike::RebuildCounter installs a
    counting lambda. Only the DELTA across the storm means anything; see the
    comment on that class. The instrument is validated at the end of this run
    rather than trusted: --validate-instrument deliberately triggers a known
    rebuild and checks the counter moves. A zero from an instrument that cannot
    produce a non-zero is not evidence.

    "no crossfade tax on already-playing material" — track 0 is a WITNESS: one
    sustained tone, launched once, never touched again. The run happens twice
    with identical parameters, once with the storm and once with the witness
    alone, and the two outputs are compared sample-by-sample. If launching taxes
    already-playing material, the witness differs between the two runs. That is
    a number, not an opinion.

    WHAT IS AND IS NOT AN OPEN AUTHOR DECISION HERE
    -----------------------------------------------
    --tracks, --sample-rate and --buffer are required with no defaults; see the
    essay in ../SpikeHarness.h. --slots and --launches are spike MECHANICS, not
    product decisions — nothing in the PRD or devplan reserves them — so they
    carry defaults and are documented as knobs of this program only.

    This spike also settles PRD §6.1's second unnumbered "Also verify" item:
    multiple active Edits summed by the DeviceManager, via --edits=K.
*/

#include "../SpikeHarness.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace tracktion;
using namespace tracktion::engine;

namespace
{
    constexpr auto criterion =
        "Graph stability under sustained launching with a fixed track set - no "
        "rebuild, no crossfade tax on already-playing material.";

    constexpr auto extraFlags =
        " [--slots=N] [--launches=N] [--edits=N] [--validate-instrument]";

    //==============================================================================
    /*  One run of the experiment. `storm` decides whether the storm tracks
        launch anything; everything else is identical between the two calls, so
        any difference in the witness channel is attributable to launching and to
        nothing else.
    */
    struct RunResult
    {
        choc::buffer::ChannelArrayBuffer<float> output { choc::buffer::Size::create (0, 0) };
        int rebuildDelta = 0;
        double blockUsMax = 0.0;
        double blockUsP50 = 0.0;
        double blockUsP99 = 0.0;
        int xruns = 0;
        bool measured = false;
    };

    RunResult runOnce (spike::HeadlessEngine& harness,
                       const spike::Args& args,
                       int slots,
                       int launches,
                       bool storm)
    {
        RunResult result;
        auto& engine = *harness;

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate    = static_cast<double> (args.sampleRate);
        params.blockSize     = static_cast<int> (args.buffer);
        params.inputChannels = 2;

        /*  FOUR output channels, in two stereo pairs, and this is what makes the
            second half of the criterion measurable at all.

            With one shared bus the storm's own audio lands in the same samples
            as the witness, so comparing the two runs measures "the storm is
            audible" - which it obviously is - and not "already-playing material
            was perturbed". Measured that way the first draft of this spike
            reported a 15.9 dBFS difference and a confident FAIL, which was the
            instrument talking, not the engine.

            So: the witness gets pair 0, every storm track gets pair 1, and only
            channels 0-1 are compared between runs. The storm still shares the
            graph, the device and the audio callback with the witness - which is
            where a crossfade tax would come from - it just no longer shares the
            samples being measured.
        */
        params.outputChannels = 4;

        harness.behaviour->describeWaveDevicesFn =
            [] (std::vector<WaveDeviceDescription>& descs, juce::AudioIODevice&, bool isInput)
            {
                descs.clear();

                if (isInput)
                    descs.emplace_back ("spike-in", 0, 1, true);
                else
                {
                    descs.emplace_back ("witness-bus", 0, 1, true);
                    descs.emplace_back ("storm-bus",   2, 3, true);
                }
            };

        const auto numTracks = static_cast<int> (args.tracks);

        // 60 bpm, 0 dB master, forEditing because createEnginePlayer asserts on it.
        auto edit = test_utilities::createTestEdit (engine, numTracks, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.size() < numTracks || numTracks < 2)
            return result;

        // The witness tone is long enough to outlast the whole run; the storm
        // clips are short so that slots are genuinely being re-launched rather
        // than started once.
        const double runSeconds = 8.0;
        auto witnessFile = spike::makeToneFile (params.sampleRate, runSeconds + 4.0, 2, 220.0f);
        auto stormFile   = spike::makeToneFile (params.sampleRate, 1.0, 2, 880.0f);
        const AudioFile witnessAudio (engine, witnessFile->getFile());
        const AudioFile stormAudio   (engine, stormFile->getFile());

        // Track 0 is the witness. Exactly one slot, one clip, launched once.
        auto& witnessSlots = tracks[0]->getClipSlotList();
        witnessSlots.ensureNumberOfSlots (1);
        auto* witnessSlot = witnessSlots.getClipSlots()[0];
        insertWaveClip (*witnessSlot, {}, witnessFile->getFile(),
                        { { 0_tp, TimeDuration::fromSeconds (runSeconds + 4.0) } },
                        DeleteExistingClips::no);

        // Tracks 1..N-1 are the storm, each with `slots` identical short clips.
        for (int t = 1; t < numTracks; ++t)
        {
            auto& list = tracks[t]->getClipSlotList();
            list.ensureNumberOfSlots (slots);

            for (auto* s : list.getClipSlots())
                insertWaveClip (*s, {}, stormFile->getFile(),
                                { { 0_tp, TimeDuration::fromSeconds (1.0) } },
                                DeleteExistingClips::no);
        }
        // The counter is installed before the player so that the FIRST build is
        // counted too, then re-baselined once the witness is up and steady.
        spike::RebuildCounter rebuilds;

        // The harness's bounded-wait replacement for createEnginePlayer: same
        // order as TE's, but the file-mapping wait cannot hang a CI job.
        bool mappedOk = false;
        auto player = spike::createPlayerWithDeadline (*edit, params,
                                                       { witnessAudio, stormAudio }, mappedOk);

        if (player == nullptr || ! mappedOk)
            return result;
        // Route: track 0 to the witness bus, every storm track to the storm bus.
        // Done before the baseline is marked, because changing a track's output
        // is a structural edit and does rebuild the graph - which is exactly the
        // kind of rebuild this spike must NOT count.
        {
            auto& dm = engine.getDeviceManager();

            if (dm.getNumWaveOutDevices() < 2)
                return result;

            const auto witnessBus = dm.getWaveOutDevice (0)->getDeviceID();
            const auto stormBus   = dm.getWaveOutDevice (1)->getDeviceID();

            tracks[0]->getOutput().setOutputToDeviceID (witnessBus);

            for (int t = 1; t < numTracks; ++t)
                tracks[t]->getOutput().setOutputToDeviceID (stormBus);

            edit->dispatchPendingUpdatesSynchronously();
        }

        const auto blockSize = params.blockSize;
        const auto settleBlocks = static_cast<int> (params.sampleRate / blockSize);   // ~1 s

        // One block first: a MonotonicBeat only exists once the transport is
        // rolling and the playback context has a sync point, so nothing can be
        // launched before this.
        player->process (blockSize);

        if (auto* clip = witnessSlot->getClip())
            if (! spike::launchAtNextSyncPoint (*clip))
                return result;

        // Let the witness settle. Anything that was going to rebuild because of
        // setup has done so by the time we mark the baseline.
        for (int i = 0; i < settleBlocks; ++i)
            player->process (blockSize);
        rebuilds.mark();

        // The storm. Launches are spread evenly across the remaining blocks so
        // that launching is genuinely SUSTAINED rather than bunched at the top.
        const auto totalBlocks = static_cast<int> ((runSeconds * params.sampleRate) / blockSize);
        const auto launchEvery = storm && launches > 0 ? std::max (1, totalBlocks / launches) : 0;
        const auto budgetUs    = (static_cast<double> (blockSize) / params.sampleRate) * 1.0e6;

        std::vector<double> blockUs;
        blockUs.reserve (static_cast<size_t> (totalBlocks));

        int launched = 0;
        int nextTrack = 1;
        int nextSlot = 0;

        for (int i = 0; i < totalBlocks; ++i)
        {
            if (launchEvery > 0 && (i % launchEvery) == 0 && launched < launches)
            {
                auto& list = tracks[nextTrack]->getClipSlotList();
                auto slotArray = list.getClipSlots();

                if (! slotArray.isEmpty())
                {
                    auto* s = slotArray[nextSlot % slotArray.size()];

                    if (auto* clip = s->getClip())
                        spike::launchAtNextSyncPoint (*clip);
                }

                ++launched;
                ++nextSlot;

                if (++nextTrack >= numTracks)
                    nextTrack = 1;
            }

            const auto t0 = std::chrono::steady_clock::now();
            player->process (blockSize);
            const auto t1 = std::chrono::steady_clock::now();

            blockUs.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());
        }
        result.rebuildDelta = rebuilds.delta();
        result.output = player->getOutput();

        std::sort (blockUs.begin(), blockUs.end());

        if (! blockUs.empty())
        {
            result.blockUsMax = blockUs.back();
            result.blockUsP50 = blockUs[blockUs.size() / 2];
            result.blockUsP99 = blockUs[static_cast<size_t> (static_cast<double> (blockUs.size()) * 0.99)];
            result.xruns = static_cast<int> (std::count_if (blockUs.begin(), blockUs.end(),
                                                            [budgetUs] (double us) { return us > budgetUs; }));
        }

        result.measured = true;
        return result;
    }

    //==============================================================================
    /*  Max absolute difference between two runs' witness output, in dBFS.
        Returns -inf (reported as -999) when the two are bit-identical, which is
        the strong result: launching did not perturb already-playing material by
        even one LSB.
    */
    double maxAbsDiffDbfs (const choc::buffer::ChannelArrayBuffer<float>& a,
                           const choc::buffer::ChannelArrayBuffer<float>& b,
                           choc::buffer::ChannelCount numChannelsToCompare)
    {
        const auto frames = std::min (a.getNumFrames(), b.getNumFrames());
        const auto chans  = std::min (numChannelsToCompare,
                                      std::min (a.getNumChannels(), b.getNumChannels()));

        float worst = 0.0f;

        for (choc::buffer::ChannelCount c = 0; c < chans; ++c)
            for (choc::buffer::FrameCount f = 0; f < frames; ++f)
                worst = std::max (worst, std::abs (a.getSample (c, f) - b.getSample (c, f)));

        if (worst <= 0.0f)
            return -999.0;

        return 20.0 * std::log10 (static_cast<double> (worst));
    }

    //==============================================================================
    /*  Proves the rebuild counter can produce a non-zero. Clip::setOffset writes
        IDs::offset, which sits in Edit::TreeWatcher's restart list
        (tracktion_Edit.cpp:147-150), so this MUST move the counter. If it does
        not, the instrument is broken and every zero it has reported is worthless.
    */
    bool instrumentProducesNonZero (Engine& engine, const spike::Args& args)
    {
        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 0;
        params.outputChannels = 2;

        auto edit = test_utilities::createTestEdit (engine, 1, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.isEmpty())
            return false;

        auto toneFile = spike::makeToneFile (params.sampleRate, 4.0, 2, 220.0f);
        const AudioFile toneAudio (engine, toneFile->getFile());

        auto& list = tracks[0]->getClipSlotList();
        list.ensureNumberOfSlots (1);
        auto* slot = list.getClipSlots()[0];
        insertWaveClip (*slot, {}, toneFile->getFile(), { { 0_tp, 4_td } }, DeleteExistingClips::no);

        spike::RebuildCounter rebuilds;

        bool mappedOk = false;
        auto player = spike::createPlayerWithDeadline (*edit, params, { toneAudio }, mappedOk);

        if (player == nullptr || ! mappedOk)
            return false;

        player->process (params.blockSize);
        rebuilds.mark();

        if (auto* clip = slot->getClip())
            clip->setOffset (TimeDuration::fromSeconds (0.25));

        edit->dispatchPendingUpdatesSynchronously();
        player->process (params.blockSize);

        return rebuilds.delta() > 0;
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    spike::makeAssertsNonInteractive();

    const auto parsed = spike::parseArgs (argc, argv);

    if (! parsed)
        return spike::usage ("spike04_graph_stability", criterion, extraFlags);

    const auto args = *parsed;

    // Spike mechanics, not product decisions - defaults are allowed here.
    const auto slots    = static_cast<int> (spike::valueFor (argc, argv, "--slots=").value_or (4));
    const auto launches = static_cast<int> (spike::valueFor (argc, argv, "--launches=").value_or (64));
    const auto edits    = static_cast<int> (spike::valueFor (argc, argv, "--edits=").value_or (1));

    if (args.tracks < 2)
    {
        std::cerr << "spike04_graph_stability: --tracks must be at least 2 - track 0 is the\n"
                     "witness and tracks 1.. are the storm. With one track there is nothing\n"
                     "to launch against an unchanging witness, and the measurement is void.\n";
        return spike::usageError;
    }

    spike::HeadlessEngine engine;
    spike::Report report ("spike04_graph_stability", argc, argv);

    report.value ("tracks", args.tracks);
    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("slots", slots);
    report.value ("launches", launches);
    report.value ("edits", edits);

    // The storm run and the reference run. Identical but for the launching.
    const auto stormRun = runOnce (engine, args, slots, launches, true);

    if (! stormRun.measured)
        return report.cannotMeasure ("storm run could not be set up (file mapping or edit creation failed)");

    const auto quietRun = runOnce (engine, args, slots, launches, false);

    if (! quietRun.measured)
        return report.cannotMeasure ("reference run could not be set up");

    // Channels 0-1 only: the witness bus. See the routing comment in runOnce().
    const auto witnessDiffDb = maxAbsDiffDbfs (stormRun.output, quietRun.output, 2);

    report.value ("rebuilds.delta", stormRun.rebuildDelta);
    report.value ("rebuilds.delta_reference", quietRun.rebuildDelta);
    report.value ("witness_max_abs_diff_dbfs", witnessDiffDb);
    report.value ("block_us.p50", stormRun.blockUsP50);
    report.value ("block_us.p99", stormRun.blockUsP99);
    report.value ("block_us.max", stormRun.blockUsMax);
    report.value ("xruns", stormRun.xruns);

    // Instrument validation. Off by default because it costs a third engine
    // bring-up, but the report must state whether it was run - a zero from an
    // unvalidated counter is not evidence.
    if (spike::hasFlag (argc, argv, "--validate-instrument"))
        report.value ("instrument_produces_nonzero",
                      instrumentProducesNonZero (*engine, args) ? 1 : 0);
    else
        report.value ("instrument_produces_nonzero", "not-run");

    const bool noRebuild   = stormRun.rebuildDelta == 0;
    const bool noCrossfade = witnessDiffDb <= -120.0;

    return report.verdict (noRebuild && noCrossfade,
                           noRebuild
                             ? (noCrossfade ? "no rebuild and the witness is untouched by launching"
                                            : "no rebuild, but already-playing material was perturbed")
                             : "the graph rebuilt during sustained launching");
}
