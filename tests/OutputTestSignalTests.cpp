/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/engine/audio/OutputTestSignal.h>
#include <wfg/engine/audio/AudioCommands.h>
#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/rt/RtCheck.h>
#include <cmath>

using namespace wfg;

TEST_CASE ("output test: WFS signals ramp on one hardware output and stop without touching cues")
{
    for (int type = 1; type <= 4; ++type)
    {
        CAPTURE (type);
        audio::OutputTestSignal signal;
        signal.prepare (48000, 480);
        audio::OutputTestSettings settings { type, 1, 1000, -20, true };
        signal.set (settings);
        juce::AudioBuffer<float> buffer (2, 480);
        float firstPeak = 0, laterPeak = 0;
        rt::resetCounts();
        for (int block = 0; block < 210; ++block)
        {
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.25f, 480);
            {
                rt::ScopedRealtimeCheck check (rt::Region::ours);
                signal.render (buffer.getArrayOfWritePointers(), 2, 480);
            }
            CHECK (buffer.getSample (0, 0) == 0.25f);
            CHECK (buffer.getSample (0, 479) == 0.25f);
            const auto peak = buffer.getMagnitude (1, 0, 480);
            if (block == 0) firstPeak = peak;
            if (block >= 50) laterPeak = std::max (laterPeak, peak);
        }
        CHECK (rt::violations() == 0);
        CHECK (laterPeak > 0.001f);
        CHECK (firstPeak < laterPeak * 0.05f);
        if (type == 2) CHECK (laterPeak == doctest::Approx (0.1f).epsilon (0.001));
        // Switching targets restarts the protective ramp.
        settings.channel = 0; signal.set (settings);
        signal.render (buffer.getArrayOfWritePointers(), 2, 480);
        CHECK (buffer.getMagnitude (0, 0, 480) < laterPeak * 0.05f);
        settings.type = 0; settings.channel = -1; signal.set (settings);
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), 0.25f, 480);
        signal.render (buffer.getArrayOfWritePointers(), 2, 480);
        CHECK (buffer.getSample (0, 250) == 0.25f);
        CHECK (buffer.getSample (1, 250) == 0.25f);
    }
}

TEST_CASE ("output test: frequency and level are audible in rendered samples")
{
    audio::OutputTestSignal signal;
    signal.prepare (48000, 480);
    signal.set ({ 2, 0, 2000, -40, false });
    juce::AudioBuffer<float> buffer (1, 480);
    for (int n = 0; n < 60; ++n) signal.render (buffer.getArrayOfWritePointers(), 1, 480);
    int crossings = 0;
    for (int n = 1; n < 480; ++n)
        if (buffer.getSample (0, n - 1) <= 0 && buffer.getSample (0, n) > 0) ++crossings;
    CHECK (crossings >= 19);
    CHECK (crossings <= 20);
    CHECK (buffer.getMagnitude (0, 0, 480) == doctest::Approx (0.01).epsilon (0.001));
}

TEST_CASE ("output test: commands validate controls and both global stops clear held signals")
{
    Engine engine;
    audio::AudioState state;
    cue::RunTable runs;
    audio::registerAudioCommands (engine.commands(), state);
    cue::registerRunCommands (engine.commands(), runs, [&] { audio::stopOutputTest (state); });
    audio::OutputTestSettings delivered;
    state.sendTest = [&] (const auto& settings) { delivered = settings; };
    using V = osc::Value;
    std::int64_t tick = 0;
    for (const auto* stop : { "audio.testStop", "run.stopAll", "run.killAll" })
    {
        REQUIRE (engine.submit ("test", "audio.testSignal", { V::int32 (2), V::int32 (1), V::int32 (1000), V::float64 (-30), V::boolean (true) }));
        engine.processTick (tick++);
        CHECK (delivered.channel == 1);
        REQUIRE (engine.submit ("test", "audio.testSignal", { V::int32 (2), V::int32 (1), V::int32 (1000), V::float64 (1), V::boolean (true) }));
        engine.processTick (tick++);
        CHECK (delivered.level == -30);
        REQUIRE (engine.submit ("test", stop, {}));
        engine.processTick (tick++);
        CHECK (delivered.type == 0);
        CHECK (delivered.channel == -1);
        CHECK_FALSE (delivered.hold);
    }
}
