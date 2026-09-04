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
    SPIKE 06 — LIVE-INPUT LATENCY THROUGH A RACK, AND PDC.
    THROWAWAY CODE (devplan:19). FEASIBILITY PROBE (author's decision).

    PRD §6.1 item 6, verbatim, and this is the pass criterion:

        6. Live-input latency through a Rack, with TE's PDC behaviour on live
           tracks understood (it may insert alignment delay exactly where none is
           wanted).

    THE PARENTHESIS IS THE WHOLE SPIKE, AND §3.25 SAYS WHY
    -----------------------------------------------------
        "Watch PDC on live input: TE may insert delay to align a live track with
         the rest of the graph, which is exactly what the rack path must not
         have."

    The fear is not that a plugin has latency - a plugin with latency has
    latency. It is that TE, told one track is 0.25 s late, DELAYS EVERY OTHER
    TRACK by 0.25 s to keep them aligned. In a DAW that is correct and desirable.
    In a show it means putting a quarter of a second between the GO button and
    every cue, to compensate for a reverb on a live microphone.

    So the measured quantity is not the latency of the plugin. It is the
    POSITION OF A DIFFERENT TRACK'S AUDIO, with and without that plugin present:

        file_path_shift_samples = position(with latency) - position(without)

    Zero means TE leaves the file path alone and the §3.25 warning can be
    retired. Non-zero means the warning is real and Go.dot must either disable
    PDC or keep latency-bearing plugins off shared graphs.

    WHAT THIS PROBE DOES AND DOES NOT COVER
    ---------------------------------------
    It measures the graph's PDC behaviour and the rack's channel shape, both of
    which are answerable without hardware. It does NOT measure real round-trip
    latency through a real driver - that is the hardware half, and it needs the
    interface attached. §3.19c's latency alignment work is a later phase anyway.
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
        "Live-input latency through a Rack, with TE's PDC behaviour on live tracks "
        "understood (it may insert alignment delay exactly where none is wanted).";

    constexpr auto extraFlags = " [--latency-ms=N] [--use-rack]";

    constexpr double fileLengthSeconds = 4.0;
    constexpr double transientAtSeconds = 1.0;

    //==============================================================================
    std::optional<choc::buffer::FrameCount>
    firstNonZeroOnBus (const choc::buffer::ChannelArrayBuffer<float>& b,
                       int firstChannel, int width, float threshold = 1.0e-4f)
    {
        for (choc::buffer::FrameCount f = 0; f < b.getNumFrames(); ++f)
            for (int c = firstChannel; c < firstChannel + width; ++c)
                if (static_cast<choc::buffer::ChannelCount> (c) < b.getNumChannels())
                    if (std::abs (b.getSample (static_cast<choc::buffer::ChannelCount> (c), f)) > threshold)
                        return f;

        return {};
    }

    double peakOnChannel (const choc::buffer::ChannelArrayBuffer<float>& b, choc::buffer::ChannelCount c)
    {
        if (c >= b.getNumChannels())
            return 0.0;

        double m = 0.0;

        for (choc::buffer::FrameCount f = 0; f < b.getNumFrames(); ++f)
            m = std::max (m, std::abs (static_cast<double> (b.getSample (c, f))));

        return m;
    }

    //==============================================================================
    struct RunResult
    {
        std::optional<choc::buffer::FrameCount> fileTransientFrame;   // track F, bus 0
        std::optional<choc::buffer::FrameCount> latencyTrackFrame;    // track L, bus 1
        double reportedLatencySamples = -1.0;
        double busPeakL = 0.0, busPeakR = 0.0;   // track L's bus, to see its width
        bool measured = false;
    };

    /*  `latencySeconds` of 0 means "no latency plugin at all", which is the
        reference the shift is measured against.
    */
    RunResult runOnce (spike::HeadlessEngine& harness, const spike::Args& args,
                       double latencySeconds, bool useRack, bool monoSource,
                       bool lowLatencyMonitoring = false)
    {
        RunResult result;
        auto& engine = *harness;

        HostedAudioDeviceInterface::Parameters params;
        params.sampleRate     = static_cast<double> (args.sampleRate);
        params.blockSize      = static_cast<int> (args.buffer);
        params.inputChannels  = 2;
        params.outputChannels = 4;      // bus 0 = track F, bus 1 = track L

        harness.behaviour->describeWaveDevicesFn =
            [] (std::vector<WaveDeviceDescription>& descs, juce::AudioIODevice&, bool isInput)
            {
                descs.clear();

                if (isInput)
                    descs.emplace_back ("spike-in", 0, 1, true);
                else
                {
                    descs.emplace_back ("file-bus",    0, 1, true);
                    descs.emplace_back ("latency-bus", 2, 3, true);
                }
            };

        auto edit = test_utilities::createTestEdit (engine, 2, Edit::EditRole::forEditing);
        auto tracks = getAudioTracks (*edit);

        if (tracks.size() < 2)
            return result;

        // Both tracks play the SAME transient from the timeline. Track F is the
        // witness: nothing is ever done to it. Track L is the one that gets the
        // latency-bearing plugin.
        auto file = spike::makeTransientFile (params.sampleRate, fileLengthSeconds,
                                              transientAtSeconds, 0.5f, monoSource ? 1 : 2);

        if (file == nullptr)
            return result;

        const AudioFile audio (engine, file->getFile());

        for (auto* t : tracks)
            insertWaveClip (*t, {}, file->getFile(),
                            { { 0_tp, TimeDuration::fromSeconds (fileLengthSeconds) } },
                            DeleteExistingClips::no);

        if (latencySeconds > 0.0)
        {
            auto latencyPlugin = insertNewPlugin<LatencyPlugin> (*tracks[1]);

            if (latencyPlugin == nullptr)
                return result;

            latencyPlugin->latencyTimeSeconds = static_cast<float> (latencySeconds);

            if (useRack)
            {
                /*  Wrap it in a Rack, because §6.1 asks about a RACK and a rack
                    is not merely "a plugin": RackInstance is a fixed stereo
                    object (getNumOutputChannelsGivenInputs returns 2, and its
                    Channel enum is {left, right}), so it may behave differently
                    from a bare plugin on the same track.
                */
                auto rack = edit->getRackList().addNewRack();

                if (rack != nullptr)
                {
                    // Take the plugin off the track, put it in the rack, then put
                    // an instance of the rack on the track in its place.
                    latencyPlugin->removeFromParent();
                    rack->addPlugin (latencyPlugin.get(), { 0.5f, 0.5f }, true);

                    // RackInstance::create returns the instance's STATE, not a
                    // plugin; PluginList::insertPlugin(ValueTree, index) is what
                    // turns that into a live plugin on the track.
                    const auto instanceState = RackInstance::create (*rack);

                    if (instanceState.isValid())
                        tracks[1]->pluginList.insertPlugin (instanceState, 0);
                }
            }
        }

        /*  Edit::setLowLatencyMonitoring is the only Edit-level lever TE offers
            against this, and it is worth measuring rather than assuming, because
            its name suggests it solves the problem and its implementation
            suggests it cannot: it shrinks the device buffer AND BYPASSES the
            plugins it is given (tracktion_Edit.cpp:2301-2330). Bypassing the
            plugin removes its latency by removing the plugin.
        */
        if (lowLatencyMonitoring)
        {
            juce::Array<EditItemID> toBypass;

            for (auto* p : tracks[1]->pluginList)
                toBypass.add (p->itemID);

            edit->setLowLatencyDisabledPlugins (toBypass);
            edit->setLowLatencyMonitoring (true, toBypass);
            edit->dispatchPendingUpdatesSynchronously();
        }

        {
            auto& dm = engine.getDeviceManager();

            bool mapped = false;
            auto player = spike::createPlayerWithDeadline (*edit, params, { audio }, mapped);

            if (player == nullptr || ! mapped)
                return result;

            if (dm.getNumWaveOutDevices() >= 2)
            {
                tracks[0]->getOutput().setOutputToDeviceID (dm.getWaveOutDevice (0)->getDeviceID());
                tracks[1]->getOutput().setOutputToDeviceID (dm.getWaveOutDevice (1)->getDeviceID());
                edit->dispatchPendingUpdatesSynchronously();
            }

            if (auto* context = edit->getTransport().getCurrentPlaybackContext())
                result.reportedLatencySamples = static_cast<double> (context->getLatencySamples());

            const auto blocks = static_cast<int> (((fileLengthSeconds + 2.0) * params.sampleRate)
                                                    / params.blockSize);

            for (int i = 0; i < blocks; ++i)
                player->process (params.blockSize);

            const auto out = player->getOutput();

            result.fileTransientFrame  = firstNonZeroOnBus (out, 0, 2);
            result.latencyTrackFrame   = firstNonZeroOnBus (out, 2, 2);
            result.busPeakL = peakOnChannel (out, 2);
            result.busPeakR = peakOnChannel (out, 3);
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
        return spike::usage ("spike06_rack_latency_pdc", criterion, extraFlags);

    const auto args = *parsed;
    const auto latencyMs = static_cast<double> (spike::valueFor (argc, argv, "--latency-ms=").value_or (250));
    const auto useRack   = spike::hasFlag (argc, argv, "--use-rack");

    spike::HeadlessEngine engine;
    engine->getPluginManager().createBuiltInType<LatencyPlugin>();

    spike::Report report ("spike06_rack_latency_pdc", argc, argv);

    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("latency_ms", latencyMs);
    report.value ("wrapped_in_rack", useRack ? 1 : 0);

    const auto sr = static_cast<double> (args.sampleRate);
    const auto latencySeconds = latencyMs / 1000.0;

    // Reference: no latency plugin anywhere.
    const auto ref = runOnce (engine, args, 0.0, false, false);

    if (! ref.measured || ! ref.fileTransientFrame)
        return report.cannotMeasure ("reference run produced no transient on the file bus");

    // With a latency-bearing plugin on the OTHER track.
    const auto withLatency = runOnce (engine, args, latencySeconds, useRack, false);

    if (! withLatency.measured || ! withLatency.fileTransientFrame)
        return report.cannotMeasure ("latency run produced no transient on the file bus");

    const auto refFile  = static_cast<long long> (*ref.fileTransientFrame);
    const auto latFile  = static_cast<long long> (*withLatency.fileTransientFrame);
    const auto fileShift = latFile - refFile;

    report.value ("reference.file_transient_frame", refFile);
    report.value ("with_latency.file_transient_frame", latFile);
    report.value ("file_path_shift_samples", fileShift);
    report.value ("file_path_shift_ms", 1000.0 * static_cast<double> (fileShift) / sr);

    report.value ("reference.reported_latency_samples", ref.reportedLatencySamples);
    report.value ("with_latency.reported_latency_samples", withLatency.reportedLatencySamples);

    if (withLatency.latencyTrackFrame)
    {
        const auto latTrack = static_cast<long long> (*withLatency.latencyTrackFrame);
        report.value ("with_latency.latency_track_frame", latTrack);
        report.value ("latency_track_delay_vs_file_samples", latTrack - latFile);
        report.value ("latency_track_delay_vs_file_ms",
                      1000.0 * static_cast<double> (latTrack - latFile) / sr);
        report.value ("expected_plugin_latency_samples", latencySeconds * sr);
    }
    else
    {
        report.value ("with_latency.latency_track_frame", "not-found");
    }

    // The documented escape hatch, measured rather than assumed.
    {
        const auto lowLat = runOnce (engine, args, latencySeconds, useRack, false, true);

        if (lowLat.measured && lowLat.fileTransientFrame)
        {
            const auto shift = static_cast<long long> (*lowLat.fileTransientFrame) - refFile;
            report.value ("lowlatency.file_path_shift_samples", shift);
            report.value ("lowlatency.reported_latency_samples", lowLat.reportedLatencySamples);
            report.value ("lowlatency.latency_bus_peak", lowLat.busPeakL);
        }
    }

    // The author's question: a MONO source into a stereo rack - what comes out?
    const auto mono = runOnce (engine, args, latencySeconds, useRack, true);

    if (mono.measured)
    {
        report.value ("mono_source.bus_peak_left", mono.busPeakL);
        report.value ("mono_source.bus_peak_right", mono.busPeakR);
        report.value ("mono_source.is_stereo_at_output",
                      (mono.busPeakL > 1.0e-4 && mono.busPeakR > 1.0e-4) ? 1 : 0);
    }

    /*  The gate is the PDC question, not the plugin's own latency. A plugin with
        latency having latency is correct behaviour; the file path moving because
        of it is the failure §3.25 warns about.
    */
    const bool filePathUndisturbed = fileShift == 0;

    return report.verdict (filePathUndisturbed,
                           filePathUndisturbed
                             ? "a latency-bearing plugin on one track did NOT shift the file path on another"
                             : "TE shifted the file path to align with the latency track - the 3.25 warning is real");
}
