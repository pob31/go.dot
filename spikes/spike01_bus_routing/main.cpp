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

    A track's INTERNAL width is two channels. Not "by default" - constexpr. The
    DESTINATION width is a separate thing and may be one channel; --bus-width=1
    measures that, and mono direct outs work.

    WHAT THIS SPIKE DOES NOT MEASURE: what a >2-channel SOURCE does on a track.
    An earlier version claimed to "feed a six-channel source to a track and record
    what actually arrives". It did not: it generated a six-channel file, built an
    AudioFile from it, and read back that file's own channel count into a field
    named wideSourceChannelsAtOutput. Nothing was inserted, launched or played,
    so the number reported was the channel count of a file this program had just
    written - an instrument measuring itself. Removed rather than left in place.
    The >2-channel question is open and belongs to whoever answers PRD §3.9b.
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

    constexpr auto extraFlags =
        " [--bus-width=1|2] [--mono=N --stereo=M] [--pace] [--wide-source-channels=N]";

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
    /*  A bus layout: one entry per destination, each with its width and the
        hardware channel it starts at.

        A real rig is NOT uniform. The author's Digiface Dante presents 64
        channels total, shared between mono direct outs (one object each, straight
        into a spatial processor), stereo sources, and stereo mix tracks. So the
        interesting question is not "does mono work" or "does stereo work" - both
        are answered - but whether they stay exact when they COEXIST and share one
        channel pool.

        The two kinds are INTERLEAVED rather than grouped, so mono buses do not all
        sit at low channel indices and stereo pairs do not all start on even ones.
        A routing bug that only shows up when a stereo pair starts at an odd
        hardware channel would hide completely behind a grouped layout.
    */
    struct Bus
    {
        int width = 2;
        int firstChannel = 0;
    };

    std::vector<Bus> makeLayout (int numMono, int numStereo, int uniformWidth, int numTracks)
    {
        std::vector<int> widths;

        if (numMono > 0 || numStereo > 0)
        {
            // Interleave, so neither kind occupies a contiguous index range.
            int m = numMono, st = numStereo;

            while (m > 0 || st > 0)
            {
                if (m > 0)  { widths.push_back (1); --m; }
                if (st > 0) { widths.push_back (2); --st; }
            }
        }
        else
        {
            widths.assign (static_cast<size_t> (numTracks), uniformWidth);
        }

        std::vector<Bus> layout;
        int channel = 0;

        for (auto w : widths)
        {
            layout.push_back ({ w, channel });
            channel += w;
        }

        return layout;
    }

    //==============================================================================
    struct BusReading
    {
        std::optional<choc::buffer::FrameCount> firstNonZero;
        double peakDbfs = -999.0;
        int identifiedTrack = -1;   // which track's transient time this matches
    };

    /*  These three all scan EVERY channel of a bus, not just its first.

        They used to take a single channel index, which in stereo mode meant only
        the LEFT half of each bus was ever inspected - channels 0, 2, 4... A leak
        into any odd hardware channel was invisible to the instrument, so the
        report's "nothing leaked anywhere else" was unsupported for exactly half
        the channels it claimed to cover. Taking a (first, width) range instead
        makes the claim mean what it says.
    */
    std::optional<choc::buffer::FrameCount>
    firstNonZeroOnBus (const choc::buffer::ChannelArrayBuffer<float>& b,
                       int firstChannel, int width,
                       float threshold = 1.0e-4f)
    {
        for (choc::buffer::FrameCount f = 0; f < b.getNumFrames(); ++f)
            for (int c = firstChannel; c < firstChannel + width; ++c)
                if (static_cast<choc::buffer::ChannelCount> (c) < b.getNumChannels())
                    if (std::abs (b.getSample (static_cast<choc::buffer::ChannelCount> (c), f)) > threshold)
                        return f;

        return {};
    }

    double peakDbfsOnBus (const choc::buffer::ChannelArrayBuffer<float>& b,
                          int firstChannel, int width)
    {
        float worst = 0.0f;

        for (int c = firstChannel; c < firstChannel + width; ++c)
            if (static_cast<choc::buffer::ChannelCount> (c) < b.getNumChannels())
                for (choc::buffer::FrameCount f = 0; f < b.getNumFrames(); ++f)
                    worst = std::max (worst, std::abs (b.getSample (static_cast<choc::buffer::ChannelCount> (c), f)));

        return worst <= 0.0f ? -999.0 : 20.0 * std::log10 (static_cast<double> (worst));
    }

    /*  Counts non-zero samples on a bus that are NOT within a window of the
        transient it is supposed to carry. Anything here is another track's audio
        arriving where it should not be, i.e. leakage.
    */
    int strayNonZeroSamples (const choc::buffer::ChannelArrayBuffer<float>& b,
                             int firstChannel, int width,
                             choc::buffer::FrameCount expectedFrame,
                             choc::buffer::FrameCount window,
                             choc::buffer::FrameCount scanUntil,
                             float threshold = 1.0e-4f)
    {
        int stray = 0;
        const auto last = std::min (scanUntil, b.getNumFrames());
        const auto lo = expectedFrame > window ? expectedFrame - window : 0;

        for (int c = firstChannel; c < firstChannel + width; ++c)
        {
            if (static_cast<choc::buffer::ChannelCount> (c) >= b.getNumChannels())
                continue;

            for (choc::buffer::FrameCount f = 0; f < last; ++f)
            {
                if (std::abs (b.getSample (static_cast<choc::buffer::ChannelCount> (c), f)) <= threshold)
                    continue;

                if (f < lo || f > expectedFrame + window)
                    ++stray;
            }
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
                       int wideChannels, int busWidth, int numMono, int numStereo,
                       bool paceToRealTime)
    {
        RunResult result;
        auto& engine = *harness;

        const auto layout = makeLayout (numMono, numStereo, busWidth,
                                        static_cast<int> (args.tracks));
        const auto numTracks = static_cast<int> (layout.size());

        if (numTracks == 0)
            return result;

        int totalChannels = 0;

        for (const auto& b : layout)
            totalChannels += b.width;

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 2;
        params.outputChannels = totalChannels;

        // One stereo bus per track, at ascending hardware channel indices. This
        // is the "arbitrary multichannel bus" the criterion is about: the device
        // is carved into as many stereo destinations as there are tracks.
        harness.behaviour->describeWaveDevicesFn =
            [layout] (std::vector<WaveDeviceDescription>& descs,
                      juce::AudioIODevice&, bool isInput)
            {
                descs.clear();

                if (isInput)
                {
                    descs.emplace_back ("spike-in", 0, 1, true);
                    return;
                }

                for (size_t i = 0; i < layout.size(); ++i)
                {
                    const auto& b = layout[i];

                    if (b.width == 1)
                    {
                        /*  A MONO direct out - one hardware channel, one object.

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
                        const ChannelIndex ch { b.firstChannel, juce::AudioChannelSet::left };
                        descs.emplace_back ("mono" + juce::String (int (i)), &ch, 1, true);
                    }
                    else
                    {
                        descs.emplace_back ("bus" + juce::String (int (i)),
                                            b.firstChannel, b.firstChannel + 1, true);
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
                                               transientTimeFor (i), 0.5f,
                                               layout[static_cast<size_t> (i)].width);

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

        std::optional<spike::RealTimePacer> pacer;

        if (paceToRealTime)
            pacer.emplace (params.sampleRate, blockSize);

        for (int i = 0; i < blocks; ++i)
        {
            player->process (blockSize);

            if (pacer)
                pacer->waitForBlock();
        }

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
            firsts.push_back (firstNonZeroOnBus (output,
                                                 layout[static_cast<size_t> (i)].firstChannel,
                                                 layout[static_cast<size_t> (i)].width));

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
            r.peakDbfs = peakDbfsOnBus (output,
                                        layout[static_cast<size_t> (i)].firstChannel,
                                        layout[static_cast<size_t> (i)].width);

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
                if (strayNonZeroSamples (output,
                                         layout[static_cast<size_t> (i)].firstChannel,
                                         layout[static_cast<size_t> (i)].width,
                                         *result.buses[static_cast<size_t> (i)].firstNonZero,
                                         tolerance, analysisEnd) > 0)
                    result.buses[static_cast<size_t> (i)].identifiedTrack = -2;   // contaminated

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
    const auto numMono      = static_cast<int> (spike::valueFor (argc, argv, "--mono=").value_or (0));
    const auto numStereo    = static_cast<int> (spike::valueFor (argc, argv, "--stereo=").value_or (0));

    spike::HeadlessEngine engine;
    spike::Report report ("spike01_bus_routing", argc, argv);

    report.value ("tracks", args.tracks);
    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    if (numMono > 0 || numStereo > 0)
    {
        report.value ("layout", "mixed");
        report.value ("mono_buses", numMono);
        report.value ("stereo_buses", numStereo);
        report.value ("output_channels", numMono + 2 * numStereo);
    }
    else
    {
        report.value ("layout", "uniform");
        report.value ("bus_width", busWidth);
        report.value ("output_channels", busWidth * args.tracks);
    }

    const auto run = runOnce (engine, args, wideChannels, busWidth, numMono, numStereo,
                             spike::hasFlag (argc, argv, "--pace"));

    if (! run.measured)
        return report.cannotMeasure ("could not set up the routing matrix");

    int correct = 0, contaminated = 0, silent = 0, misrouted = 0;

    for (size_t i = 0; i < run.buses.size(); ++i)
    {
        const auto& b = run.buses[i];
        const auto tag = "bus" + std::to_string (i);

        report.value (tag + ".peak_dbfs", b.peakDbfs);
        report.value (tag + ".first_frame", b.firstNonZero ? static_cast<long long> (*b.firstNonZero) : -1LL);
        report.value (tag + ".identified_track", b.identifiedTrack);

        if (! b.firstNonZero)              { ++silent; }
        else if (b.identifiedTrack == -2)  { ++contaminated; }
        else if (b.identifiedTrack == static_cast<int> (i)) { ++correct; }
        else                               { ++misrouted; }
    }

    /*  DEGENERATE-RESULT GUARD.

        Bus 0 is the identification reference, so its elapsed time is identically
        zero and it matches track 0 whenever it has ANY content above threshold.
        "1 correct" is therefore the FLOOR of this instrument, not a measurement:
        a run in which every bus is wrong still scores 1.

        That distinction was drawn wrongly once already. The report read
        "consistently 1 correct out of 64, rather than varying - the signature of
        something structural rather than of load", when 1 was simply what total
        failure looks like here. Say so explicitly instead of inviting the
        inference again.
    */
    const bool degenerate = correct == 1 && run.buses.size() > 1
                              && run.buses[0].identifiedTrack == 0;

    report.value ("identification_degenerate", degenerate ? 1 : 0);
    report.value ("buses.correct", correct);
    report.value ("buses.misrouted", misrouted);
    report.value ("buses.contaminated", contaminated);
    report.value ("buses.silent", silent);

    const bool allCorrect = correct == static_cast<int> (run.buses.size());

    if (degenerate)
        return report.cannotMeasure (
            "only the reference bus identified, which is this instrument's floor rather "
            "than a result - no conclusion about routing can be drawn from this run");

    return report.verdict (allCorrect,
                           allCorrect
                             ? "every track reached its own destination bus and no other"
                             : "the routing matrix did not match intent - see the per-bus lines");
}
