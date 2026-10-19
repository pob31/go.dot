/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*  THE AUXES (namespace draft §93): a shared channel fed by the sends of the
    tracks, through the real host - Tracktion's graph, the aux taps, the silent
    aux sends that order the graph, the aux inputs - with an impulse on a rack
    channel's live input as the source, so where it lands says how late it is.

    What the spike had to settle (A.1), and these keep settled:

    - A send down the list is heard IN THE SAME BLOCK as the direct out: a rack
      channel into the first aux, and that aux into the one after it.
    - A send back up the list - a loop - is heard EXACTLY ONE BLOCK later, and
      never at an offset inside it.
    - A send is below the level of the track it leaves, as every send is.
    - A double Esc's sweep silences an aux, and the next GO opens it again.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/audio/CueMatrix.h>

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    struct ScopedStorage
    {
        ScopedStorage()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-aux-test-" + juce::Uuid().toDashedString()))
        {
        }

        ~ScopedStorage() { folder.deleteRecursively(); }

        std::string path() const { return folder.getFullPathName().toStdString(); }

        juce::File folder;
    };

    struct HostRig
    {
        ScopedStorage storage;
        audio::AudioHost host { storage.path() };
    };

    /*  Every sample of every output, kept for the case to read. Sized before
        the blocks run, so the audio side writes into memory it already has. */
    struct RecordingSink final : audio::BlockSink
    {
        RecordingSink (int channels, int samples)
            : recorded (static_cast<std::size_t> (channels), std::vector<float> (static_cast<std::size_t> (samples), 0.0f))
        {
        }

        void blockProduced (const float* const* channels, int numChannels, int numSamples) noexcept override
        {
            for (int channel = 0; channel < numChannels && channel < static_cast<int> (recorded.size()); ++channel)
                for (int n = 0; n < numSamples; ++n)
                    if (const auto at = written + static_cast<std::size_t> (n); at < recorded[static_cast<std::size_t> (channel)].size())
                        recorded[static_cast<std::size_t> (channel)][at] = channels[channel][n];

            written += static_cast<std::size_t> (numSamples);
        }

        /** The first sample of a channel above `threshold`, or -1. */
        int firstAbove (int channel, float threshold = 0.01f) const
        {
            const auto& samples = recorded[static_cast<std::size_t> (channel)];

            for (std::size_t n = 0; n < samples.size() && n < written; ++n)
                if (std::abs (samples[n]) > threshold)
                    return static_cast<int> (n);

            return -1;
        }

        float at (int channel, int sample) const
        {
            return recorded[static_cast<std::size_t> (channel)][static_cast<std::size_t> (sample)];
        }

        float peak (int channel) const
        {
            auto loudest = 0.0f;

            for (const auto sample : recorded[static_cast<std::size_t> (channel)])
                loudest = std::max (loudest, std::abs (sample));

            return loudest;
        }

        std::vector<std::vector<float>> recorded;
        std::size_t written = 0;
    };

    constexpr int rate = 48000;
    constexpr int block = 128;

    /*  A rig of one voice, one rack channel `MIC` on input 0, and the auxes
        named - each stereo - and six outputs: the mic's direct out on 0 and
        each aux's own out on the pair after it. */
    void buildRig (HostRig& rig, const std::vector<std::string>& auxes, int outputs = 6)
    {
        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = block;
        settings.outputChannels = outputs;
        settings.inputChannels = 2;
        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 1;
        spec.channelsPerTrack = 2;

        audio::RackChannelSpec mic;
        mic.id = "MC000001";
        mic.name = "Mic";
        spec.rack.push_back (mic);

        for (const auto& id : auxes)
        {
            audio::RackChannelSpec aux;
            aux.id = id;
            aux.name = "Aux " + id;
            aux.aux = true;
            aux.auxWidth = 2;
            spec.rack.push_back (aux);
        }

        INFO ("build error: " << rig.host.lastError());
        REQUIRE (rig.host.buildEdit (spec));
    }

    /*  Runs `blocks` blocks of input, an impulse of `height` on input 0 at
        `impulseAt` and silence everywhere else. */
    void pump (HostRig& rig, int blocks, int impulseAt, float height = 1.0f)
    {
        std::vector<float> first (block, 0.0f), second (block, 0.0f);
        const float* inputs[] { first.data(), second.data() };

        for (int b = 0; b < blocks; ++b)
        {
            std::fill (first.begin(), first.end(), 0.0f);
            const auto start = static_cast<int> (rig.host.clock().samplesElapsed());

            if (impulseAt >= start && impulseAt < start + block)
                first[static_cast<std::size_t> (impulseAt - start)] = height;

            rig.host.processBlock (inputs, 2);
        }
    }

    using Coefficients = std::vector<std::array<double, 3>>;
}

TEST_CASE ("aux: a send into the first aux is heard in the same block as the direct out")
{
    HostRig rig;
    buildRig (rig, { "AX000001" });

    const auto mic = rig.host.rackTrackOf ("MC000001");
    const auto aux = rig.host.rackTrackOf ("AX000001");
    REQUIRE (mic == 1);
    REQUIRE (aux == 2);
    CHECK (rig.host.isRackTrack (aux));
    CHECK (rig.host.isAuxTrack (aux));
    CHECK_FALSE (rig.host.isAuxTrack (mic));
    CHECK (rig.host.auxColumnCount() == 2);

    //  Past the rig's six outputs.
    const auto column = static_cast<double> (rig.host.auxColumnOf ("AX000001"));
    REQUIRE (juce::exactlyEqual (column, 6.0));
    CHECK (rig.host.auxColumnOf ("MC000001") == -1);
    REQUIRE (rig.host.trackAuxMatrix (mic) != nullptr);

    //  The mic: direct out on 0, and into the aux's left at half.
    rig.host.setTrackRouting (mic, 0.0, Coefficients { { 0, 0, 1.0 }, { 0, column, 0.5 } });
    //  The aux: its left out on 2.
    rig.host.setTrackRouting (aux, 0.0, Coefficients { { 0, 2, 1.0 } });

    rig.host.setRackSource (mic, 0, 1);
    rig.host.openRackGate (mic, -1);

    RecordingSink sink { 6, 40 * block };
    rig.host.setBlockSink (&sink);

    //  Mid-block, so a block's delay and an offset inside one are told apart.
    const auto impulseAt = 20 * block + 37;
    pump (rig, 40, impulseAt);
    rig.host.setBlockSink (nullptr);

    REQUIRE (sink.firstAbove (0) == impulseAt);
    CHECK (sink.at (0, impulseAt) == doctest::Approx (1.0f));

    //  THE SAME SAMPLE, at the send's half.
    CHECK (sink.firstAbove (2) == impulseAt);
    CHECK (sink.at (2, impulseAt) == doctest::Approx (0.5f));

    //  Nowhere else.
    CHECK (sink.peak (1) == 0.0f);
    CHECK (sink.peak (3) == 0.0f);
}

TEST_CASE ("aux: down the list in the same block, back up the list one block late")
{
    HostRig rig;
    buildRig (rig, { "AX000001", "AX000002" });

    const auto mic = rig.host.rackTrackOf ("MC000001");
    const auto first = rig.host.rackTrackOf ("AX000001");
    const auto second = rig.host.rackTrackOf ("AX000002");
    const auto firstColumn = static_cast<double> (rig.host.auxColumnOf ("AX000001"));
    const auto secondColumn = static_cast<double> (rig.host.auxColumnOf ("AX000002"));
    REQUIRE (juce::exactlyEqual (firstColumn, 6.0));
    REQUIRE (juce::exactlyEqual (secondColumn, 8.0));

    rig.host.setRackSource (mic, 0, 1);
    rig.host.openRackGate (mic, -1);

    SUBCASE ("the mic into the first aux, the first into the second: the same block")
    {
        rig.host.setTrackRouting (mic, 0.0, Coefficients { { 0, 0, 1.0 }, { 0, firstColumn, 1.0 } });
        rig.host.setTrackRouting (first, 0.0, Coefficients { { 0, secondColumn, 0.5 } });
        rig.host.setTrackRouting (second, 0.0, Coefficients { { 0, 4, 1.0 } });

        RecordingSink sink { 6, 40 * block };
        rig.host.setBlockSink (&sink);
        const auto impulseAt = 20 * block + 61;
        pump (rig, 40, impulseAt);
        rig.host.setBlockSink (nullptr);

        REQUIRE (sink.firstAbove (0) == impulseAt);
        CHECK (sink.firstAbove (4) == impulseAt);
        CHECK (sink.at (4, impulseAt) == doctest::Approx (0.5f));
    }

    SUBCASE ("the mic into the second aux, the second back into the first: one block late")
    {
        rig.host.setTrackRouting (mic, 0.0, Coefficients { { 0, 0, 1.0 }, { 0, secondColumn, 1.0 } });
        rig.host.setTrackRouting (second, 0.0, Coefficients { { 0, firstColumn, 0.5 } });
        rig.host.setTrackRouting (first, 0.0, Coefficients { { 0, 2, 1.0 } });

        RecordingSink sink { 6, 40 * block };
        rig.host.setBlockSink (&sink);
        const auto impulseAt = 20 * block + 61;
        pump (rig, 40, impulseAt);
        rig.host.setBlockSink (nullptr);

        REQUIRE (sink.firstAbove (0) == impulseAt);
        CHECK (sink.firstAbove (2) == impulseAt + block);
        CHECK (sink.at (2, impulseAt + block) == doctest::Approx (0.5f));
    }

    SUBCASE ("an aux into itself: one block late, each pass")
    {
        rig.host.setTrackRouting (mic, 0.0, Coefficients { { 0, 0, 1.0 }, { 0, firstColumn, 1.0 } });
        rig.host.setTrackRouting (first, 0.0, Coefficients { { 0, 2, 1.0 }, { 0, firstColumn, 0.5 } });

        RecordingSink sink { 6, 40 * block };
        rig.host.setBlockSink (&sink);
        const auto impulseAt = 20 * block + 61;
        pump (rig, 40, impulseAt);
        rig.host.setBlockSink (nullptr);

        CHECK (sink.at (2, impulseAt) == doctest::Approx (1.0f));
        CHECK (sink.at (2, impulseAt + block) == doctest::Approx (0.5f));
        CHECK (sink.at (2, impulseAt + 2 * block) == doctest::Approx (0.25f));
    }
}

TEST_CASE ("aux: a send is below the level of the track it leaves")
{
    HostRig rig;
    buildRig (rig, { "AX000001" });

    const auto mic = rig.host.rackTrackOf ("MC000001");
    const auto aux = rig.host.rackTrackOf ("AX000001");
    const auto column = static_cast<double> (rig.host.auxColumnOf ("AX000001"));

    //  The mic at -6 dB: its direct out and its send both a half lower.
    rig.host.setTrackRouting (mic, -6.0206, Coefficients { { 0, 0, 1.0 }, { 0, column, 1.0 } });
    rig.host.setTrackRouting (aux, 0.0, Coefficients { { 0, 2, 1.0 } });
    rig.host.setRackSource (mic, 0, 1);
    rig.host.openRackGate (mic, -1);

    RecordingSink sink { 6, 40 * block };
    rig.host.setBlockSink (&sink);
    const auto impulseAt = 20 * block + 5;
    pump (rig, 40, impulseAt);
    rig.host.setBlockSink (nullptr);

    CHECK (sink.at (0, impulseAt) == doctest::Approx (0.5f).epsilon (0.001));
    CHECK (sink.at (2, impulseAt) == doctest::Approx (0.5f).epsilon (0.001));
}

TEST_CASE ("aux: a double Esc's sweep shuts an aux, and opening it again lets its sends through")
{
    HostRig rig;
    buildRig (rig, { "AX000001" });

    const auto mic = rig.host.rackTrackOf ("MC000001");
    const auto aux = rig.host.rackTrackOf ("AX000001");
    const auto column = static_cast<double> (rig.host.auxColumnOf ("AX000001"));

    rig.host.setTrackRouting (mic, 0.0, Coefficients { { 0, column, 1.0 } });
    rig.host.setTrackRouting (aux, 0.0, Coefficients { { 0, 2, 1.0 } });
    rig.host.setRackSource (mic, 0, 1);
    rig.host.openRackGate (mic, -1);

    CHECK (rig.host.isAuxOpen (aux));

    rig.host.resetEffects ({});
    CHECK (rig.host.trackMatrix (aux)->levelDb() <= audio::CueMatrix::silenceDb);

    {
        RecordingSink sink { 6, 20 * block };
        rig.host.setBlockSink (&sink);
        pump (rig, 20, 10 * block + 3);
        rig.host.setBlockSink (nullptr);

        CHECK_FALSE (rig.host.isAuxOpen (aux));
        CHECK (sink.peak (2) == 0.0f);
    }

    rig.host.openAux (aux);
    rig.host.trackMatrix (aux)->setLevelDb (0.0f);
    CHECK (rig.host.isAuxOpen (aux));

    {
        RecordingSink sink { 6, 20 * block };
        rig.host.setBlockSink (&sink);
        const auto start = static_cast<int> (rig.host.clock().samplesElapsed());
        pump (rig, 20, start + 10 * block + 3);
        rig.host.setBlockSink (nullptr);

        CHECK (sink.peak (2) == doctest::Approx (1.0f));
    }
}

TEST_CASE ("aux: a show with no aux builds no tap, no aux send and no column")
{
    HostRig rig;

    audio::HostSettings settings;
    settings.sampleRate = rate;
    settings.blockSize = block;
    settings.outputChannels = 2;
    REQUIRE (rig.host.start (settings));

    audio::EditSpec spec;
    spec.tracks = 2;
    REQUIRE (rig.host.buildEdit (spec));

    CHECK (rig.host.auxColumnCount() == 0);
    CHECK (rig.host.trackAuxMatrix (0) == nullptr);
    CHECK (rig.host.auxColumnOf ("AX000001") == -1);
    CHECK (rig.host.inspectNodeIds().ok());
}

TEST_CASE ("M83: what four stereo auxes cost a block of a 64-voice graph")
{
    /*  Namespace draft §93, A.1. The same graph built twice - 64 voices and
        four rack channels, then the same with four stereo auxes - and a
        thousand blocks of 128 timed through each. Said, not asserted: the
        figure is Release's on a quiet machine, and a Debug or a loaded runner
        says something else. Every voice sends nothing, which is a show's
        usual state and the one the silent slots are for; the auxes carry an
        impulse a block from a rack channel. */
    const auto timeBlocks = [] (bool withAuxes)
    {
        HostRig rig;

        audio::HostSettings settings;
        settings.sampleRate = rate;
        settings.blockSize = block;
        settings.outputChannels = 16;
        settings.inputChannels = 2;
        REQUIRE (rig.host.start (settings));

        audio::EditSpec spec;
        spec.tracks = 64;
        spec.channelsPerTrack = 2;

        for (int i = 0; i < 4; ++i)
        {
            audio::RackChannelSpec channel;
            channel.id = "MC00000" + std::to_string (i + 1);
            spec.rack.push_back (channel);
        }

        if (withAuxes)
            for (int i = 0; i < 4; ++i)
            {
                audio::RackChannelSpec aux;
                aux.id = "AX00000" + std::to_string (i + 1);
                aux.aux = true;
                spec.rack.push_back (aux);
            }

        REQUIRE (rig.host.buildEdit (spec));

        if (withAuxes)
        {
            const auto mic = rig.host.rackTrackOf ("MC000001");
            rig.host.setRackSource (mic, 0, 1);
            rig.host.openRackGate (mic, -1);
            rig.host.setTrackRouting (mic, 0.0, Coefficients { { 0, 0, 1.0 },
                                                               { 0, static_cast<double> (rig.host.auxColumnOf ("AX000001")), 1.0 } });

            for (int i = 0; i < 4; ++i)
                rig.host.setTrackRouting (rig.host.rackTrackOf ("AX00000" + std::to_string (i + 1)), 0.0,
                                          Coefficients { { 0, static_cast<double> (2 * i), 1.0 } });
        }

        pump (rig, 50, 0);

        const auto started = juce::Time::getHighResolutionTicks();
        pump (rig, 1000, 0);
        const auto ticks = juce::Time::getHighResolutionTicks() - started;

        return juce::Time::highResolutionTicksToSeconds (ticks) * 1.0e6 / 1000.0;
    };

    const auto without = timeBlocks (false);
    const auto with = timeBlocks (true);

    MESSAGE ("M83: a block of 128 at 48 kHz, 64 voices and 4 rack channels: "
             << without << " us without auxes, " << with << " us with four stereo auxes (budget "
             << 1.0e6 * block / rate << " us)");

    CHECK (with > 0.0);
}
