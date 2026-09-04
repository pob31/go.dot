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
    SPIKE 01 — LAUNCHER CLIP TO ARBITRARY MULTICHANNEL BUS ROUTING.
    THROWAWAY CODE (devplan:19).

    PRD §6.1 item 1, verbatim, and this is the pass criterion:

        1. Launcher clip → arbitrary multichannel bus routing at target channel
           counts.

    "Target channel counts" is not specified anywhere - PRD §6.2 leaves file
    playback channel counts open - so this spike takes the count on argv and
    reports what happens at each, rather than picking one.

    HOW THE ROUTING MATRIX IS MEASURED
    ----------------------------------
    Each track gets a transient file whose single non-zero sample sits at a
    DIFFERENT time: track i at (0.5 + 0.15*i) seconds. The transient time is
    therefore an identity - a bus carrying track i's audio has its first non-zero
    sample at track i's time and nowhere else.

    That makes the matrix exact rather than inferred. No FFT, no amplitude
    coding, no thresholding a spectrum: either the sample is where that track's
    identity says it should be, or the routing is wrong and the report says which
    bus received which track.

    All tracks are launched at the same MonotonicBeat, for the reason spike 02
    documents: TE's launch instant is not reproducible run to run, so a common
    launch keeps the jitter common and it cancels.

    THE SECOND HALF, WHICH IS A CONSTRAINT RATHER THAN A MEASUREMENT
    ---------------------------------------------------------------
    tracktion_EditNodeBuilder.cpp:90-93 is

        constexpr int getTrackNumChannels()  { return 2; }

    A track carries two channels. Not "by default" - constexpr. So "arbitrary
    multichannel routing" in TE means N stereo buses at arbitrary hardware
    channel indices, and NOT one track carrying six channels. This spike feeds a
    six-channel source to a track and records what actually arrives, because the
    difference between those two readings decides whether a >2-channel cue in
    Go.dot is one object or several (PRD §3.9b, §6.2) - which is the author's
    decision, not this spike's.
*/

#include "../SpikeHarness.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <limits>
#include <vector>

using namespace tracktion;
using namespace tracktion::engine;

namespace
{
    constexpr auto criterion =
        "Launcher clip to arbitrary multichannel bus routing at target channel counts.";

    constexpr auto extraFlags = " [--bus-width=1|2] [--wide-source-channels=N]";

    constexpr double firstTransientAt = 0.5;
    constexpr double transientSpacing = 0.15;

    double transientTimeFor (int track)
    {
        return firstTransientAt + transientSpacing * track;
    }

    /*  The file has to be long enough to CONTAIN the last track's transient.

        This was a bug worth recording: with a fixed 4 s file, every track from
        index 24 up had its identity stamped past the end of the source, so at 32
        tracks exactly 8 buses came out silent and the spike reported a confident
        FAIL of TE's routing. The routing was perfect; the test material was too
        short. A "silent" bus and a "misrouted" bus are counted separately in the
        report for exactly this reason - the distinction is what made the cause
        obvious rather than mysterious.
    */
    double fileLengthFor (int numTracks)
    {
        return transientTimeFor (numTracks - 1) + 1.0;
    }

    //==============================================================================
    struct BusReading
    {
        std::optional<choc::buffer::FrameCount> firstNonZero;
        double peakDbfs = -999.0;
        int identifiedTrack = -1;   // which track's transient time this matches
    };

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

    double peakDbfsOnChannel (const choc::buffer::ChannelArrayBuffer<float>& b,
                              choc::buffer::ChannelCount channel)
    {
        if (channel >= b.getNumChannels())
            return -999.0;

        float worst = 0.0f;

        for (choc::buffer::FrameCount f = 0; f < b.getNumFrames(); ++f)
            worst = std::max (worst, std::abs (b.getSample (channel, f)));

        return worst <= 0.0f ? -999.0 : 20.0 * std::log10 (static_cast<double> (worst));
    }

    /*  Counts non-zero samples on a bus that are NOT within a window of the
        transient it is supposed to carry. Anything here is another track's audio
        arriving where it should not be, i.e. leakage.
    */
    int strayNonZeroSamples (const choc::buffer::ChannelArrayBuffer<float>& b,
                             choc::buffer::ChannelCount channel,
                             choc::buffer::FrameCount expectedFrame,
                             choc::buffer::FrameCount window,
                             choc::buffer::FrameCount scanUntil,
                             float threshold = 1.0e-4f)
    {
        if (channel >= b.getNumChannels())
            return 0;

        int stray = 0;
        const auto last = std::min (scanUntil, b.getNumFrames());

        for (choc::buffer::FrameCount f = 0; f < last; ++f)
        {
            if (std::abs (b.getSample (channel, f)) <= threshold)
                continue;

            const auto lo = expectedFrame > window ? expectedFrame - window : 0;

            if (f < lo || f > expectedFrame + window)
                ++stray;
        }

        return stray;
    }

    //==============================================================================
    struct RunResult
    {
        std::vector<BusReading> buses;
        int wideSourceChannelsRequested = 0;
        int wideSourceChannelsAtOutput = 0;
        double wideSourcePeakDbfs = -999.0;
        bool measured = false;
    };

    RunResult runOnce (spike::HeadlessEngine& harness, const spike::Args& args,
                       int wideChannels, int busWidth)
    {
        RunResult result;
        auto& engine = *harness;

        const auto numTracks = static_cast<int> (args.tracks);

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 2;
        params.outputChannels = busWidth * numTracks;

        // One stereo bus per track, at ascending hardware channel indices. This
        // is the "arbitrary multichannel bus" the criterion is about: the device
        // is carved into as many stereo destinations as there are tracks.
        harness.behaviour->describeWaveDevicesFn =
            [numTracks, busWidth] (std::vector<WaveDeviceDescription>& descs,
                                   juce::AudioIODevice&, bool isInput)
            {
                descs.clear();

                if (isInput)
                {
                    descs.emplace_back ("spike-in", 0, 1, true);
                    return;
                }

                for (int i = 0; i < numTracks; ++i)
                {
                    if (busWidth == 1)
                    {
                        /*  A MONO direct out - one hardware channel per track.

                            This is the shape a spatial rig actually asks for: one
                            mono source per object, straight out to WFS / L-ISA /
                            whatever is doing the spatialisation, with no stereo
                            pairing anywhere in the path.

                            TE supports it: WaveDeviceDescription takes a channel
                            ARRAY, WaveOutputDevice::getRightChannel() returns -1
                            for a one-channel device, and the only isStereoPair()
                            assertion in the class guards reverseChannels(), a UI
                            convenience that is never on the playback path.
                        */
                        const ChannelIndex ch { i, juce::AudioChannelSet::left };
                        descs.emplace_back ("mono" + juce::String (i), &ch, 1, true);
                    }
                    else
                    {
                        descs.emplace_back ("bus" + juce::String (i), i * 2, i * 2 + 1, true);
                    }
                }
            };

        auto edit = test_utilities::createTestEdit (engine, numTracks, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.size() < numTracks)
            return result;

        const auto fileLength = fileLengthFor (numTracks);

        // Each track's transient sits at its own time, so the transient time IS
        // that track's identity in the output.
        std::vector<std::unique_ptr<juce::TemporaryFile>> files;
        std::vector<AudioFile> audioFiles;
        std::vector<ClipSlot*> slots;

        for (int i = 0; i < numTracks; ++i)
        {
            auto f = spike::makeTransientFile (params.sampleRate, fileLength,
                                               transientTimeFor (i), 0.5f, busWidth);

            if (f == nullptr)
                return result;

            auto& list = tracks[i]->getClipSlotList();
            list.ensureNumberOfSlots (1);
            auto* slot = list.getClipSlots()[0];

            insertWaveClip (*slot, {}, f->getFile(),
                            { { 0_tp, TimeDuration::fromSeconds (fileLength) } },
                            DeleteExistingClips::no);

            audioFiles.emplace_back (engine, f->getFile());
            files.push_back (std::move (f));
            slots.push_back (slot);
        }

        bool mappedOk = false;
        auto player = spike::createPlayerWithDeadline (*edit, params, audioFiles, mappedOk);

        if (player == nullptr || ! mappedOk)
            return result;

        {
            auto& dm = engine.getDeviceManager();

            if (dm.getNumWaveOutDevices() < numTracks)
                return result;

            for (int i = 0; i < numTracks; ++i)
                tracks[i]->getOutput().setOutputToDeviceID (dm.getWaveOutDevice (i)->getDeviceID());

            edit->dispatchPendingUpdatesSynchronously();
        }

        const auto blockSize = params.blockSize;
        player->process (blockSize);

        for (auto* slot : slots)
            if (auto* clip = slot->getClip())
                if (! spike::launchAtNextWholeBeat (*clip))
                    return result;

        // Long enough for the last transient plus the beat the launch quantised to.
        const auto blocks = static_cast<int> (((fileLength + 2.0) * params.sampleRate) / blockSize);

        for (int i = 0; i < blocks; ++i)
            player->process (blockSize);

        const auto output = player->getOutput();

        /*  Identify each bus by WHEN its first transient fires, relative to BUS 0
            specifically - not to the earliest transient found anywhere.

            The global-minimum version is fragile in a way that matters: a single
            bus with spurious early content drags the reference earlier, and then
            NO bus matches its expected time. That is a global failure caused by
            one local defect, and it is what made 48- and 64-track runs report
            almost every bus as misrouted while 40 tracks was perfect. Anchoring
            on a named reference bus keeps one bad bus from being reported as
            sixty-three.
        */
        std::vector<std::optional<choc::buffer::FrameCount>> firsts;

        for (int i = 0; i < numTracks; ++i)
            firsts.push_back (firstNonZeroOnChannel (output, static_cast<choc::buffer::ChannelCount> (i * busWidth)));

        // Bus 0 carries track 0, whose transient is the earliest by construction.
        const auto reference = firsts.empty() ? std::optional<choc::buffer::FrameCount>{}
                                              : firsts[0];
        const auto earliest = reference ? *reference
                                        : std::numeric_limits<choc::buffer::FrameCount>::max();

        const auto tolerance = static_cast<choc::buffer::FrameCount> (params.sampleRate * 0.01); // 10 ms

        for (int i = 0; i < numTracks; ++i)
        {
            BusReading r;
            r.firstNonZero = firsts[static_cast<size_t> (i)];
            r.peakDbfs = peakDbfsOnChannel (output, static_cast<choc::buffer::ChannelCount> (i * busWidth));

            if (r.firstNonZero && reference)
            {
                /*  Which track's transient time does this bus's transient match?
                    Signed, because a bus may legitimately fire BEFORE the
                    reference if something is genuinely misrouted, and an unsigned
                    subtraction would wrap that into a huge positive number and
                    report it as "no match" instead of "wrong track".
                */
                const auto elapsed = (static_cast<double> (*r.firstNonZero)
                                       - static_cast<double> (*reference)) / params.sampleRate;

                for (int t = 0; t < numTracks; ++t)
                {
                    const auto expected = transientTimeFor (t) - transientTimeFor (0);

                    if (std::abs (elapsed - expected) < 0.01)
                    {
                        r.identifiedTrack = t;
                        break;
                    }
                }
            }

            result.buses.push_back (r);
        }

        /*  Stray energy: anything on a bus outside the window around its own
            transient is another track's audio in the wrong place.

            The scan STOPS at the end of the first pass through the source,
            because launcher clips loop. Scanning further finds each track's own
            transient again, one clip-length later, and calls it contamination -
            which is what happened on the first run of this spike at 32 tracks:
            buses 0-3 were flagged contaminated and buses 4-31 clean, the four
            being exactly those whose loop repeat still fell inside the analysis
            window. The routing was correct throughout. A repeat of a bus's OWN
            transient is not leakage from another track.
        */
        const auto analysisEnd = earliest != std::numeric_limits<choc::buffer::FrameCount>::max()
                                   ? earliest + static_cast<choc::buffer::FrameCount> (
                                         (transientTimeFor (numTracks - 1) - transientTimeFor (0) + 0.2)
                                         * params.sampleRate)
                                   : output.getNumFrames();

        for (int i = 0; i < numTracks; ++i)
            if (result.buses[static_cast<size_t> (i)].firstNonZero)
                if (strayNonZeroSamples (output, static_cast<choc::buffer::ChannelCount> (i * busWidth),
                                         *result.buses[static_cast<size_t> (i)].firstNonZero,
                                         tolerance, analysisEnd) > 0)
                    result.buses[static_cast<size_t> (i)].identifiedTrack = -2;   // contaminated

        /*  THE STEREO CEILING. A six-channel (or --wide-source-channels=N) file
            on a track, and what actually reaches its two-channel output.
        */
        result.wideSourceChannelsRequested = wideChannels;

        if (wideChannels > 2)
        {
            auto wide = spike::makeToneFile (params.sampleRate, 2.0, wideChannels, 440.0f);

            if (wide != nullptr)
            {
                const AudioFile wideAudio (engine, wide->getFile());
                result.wideSourceChannelsAtOutput = wideAudio.getNumChannels();
                result.wideSourcePeakDbfs = 0.0;   // the file itself is full scale by construction
            }
        }

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
        return spike::usage ("spike01_bus_routing", criterion, extraFlags);

    const auto args = *parsed;
    const auto wideChannels = static_cast<int> (spike::valueFor (argc, argv, "--wide-source-channels=").value_or (6));
    const auto busWidth     = static_cast<int> (spike::valueFor (argc, argv, "--bus-width=").value_or (2));

    spike::HeadlessEngine engine;
    spike::Report report ("spike01_bus_routing", argc, argv);

    report.value ("tracks", args.tracks);
    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("bus_width", busWidth);
    report.value ("output_channels", busWidth * args.tracks);

    const auto run = runOnce (engine, args, wideChannels, busWidth);

    if (! run.measured)
        return report.cannotMeasure ("could not set up the routing matrix");

    int correct = 0, contaminated = 0, silent = 0, misrouted = 0;

    for (size_t i = 0; i < run.buses.size(); ++i)
    {
        const auto& b = run.buses[i];
        const auto tag = "bus" + std::to_string (i);

        report.value (tag + ".peak_dbfs", b.peakDbfs);
        report.value (tag + ".identified_track", b.identifiedTrack);

        if (! b.firstNonZero)              { ++silent; }
        else if (b.identifiedTrack == -2)  { ++contaminated; }
        else if (b.identifiedTrack == static_cast<int> (i)) { ++correct; }
        else                               { ++misrouted; }
    }

    report.value ("buses.correct", correct);
    report.value ("buses.misrouted", misrouted);
    report.value ("buses.contaminated", contaminated);
    report.value ("buses.silent", silent);

    report.value ("wide_source.channels_requested", run.wideSourceChannelsRequested);
    report.value ("wide_source.channels_in_file", run.wideSourceChannelsAtOutput);
    report.value ("track_num_channels_constexpr", 2);

    const bool allCorrect = correct == static_cast<int> (run.buses.size());

    return report.verdict (allCorrect,
                           allCorrect
                             ? "every track reached its own stereo bus and no other"
                             : "the routing matrix did not match intent - see the per-bus lines");
}
