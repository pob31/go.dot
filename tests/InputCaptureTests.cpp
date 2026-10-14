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
    WHAT THE INPUTS HEARD, ON THE ENGINE'S COUNT (namespace draft §58, RD.9b;
    audio/InputCapture.h; `wfg serve --capture`).

    What is pinned here, under the hosted pump - the same block source as a
    render, its inputs fed from a file whose every frame says which frame and
    which channel it is: a capture taken up while the pump runs writes a
    sidecar whose first sample is the clock's when it began - a block's first
    sample, between the counts read either side of the moment it was handed
    over - and every frame of its WAV is the input the graph was handed on
    `firstSample + n`, channel for channel, for the inputs asked for and no
    others. A ring made a block deep, with the writer held, drops what does
    not fit, counts it, and writes the hole as silence, so the frames after it
    still sit on their own samples. And without a pump: inputs the interface
    does not have are refused, saying which; a clock that goes back ends the
    capture there, and the sidecar says why.

    Every case runs under fr_FR as well as C: the sidecar is JSON, whose
    numbers are never a locale's.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/audio/HostedAudioDriver.h>
#include <wfg/engine/audio/InputCapture.h>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    namespace audio = wfg::audio;

    struct ScopedFolder
    {
        ScopedFolder()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-input-capture-" + juce::Uuid().toDashedString()))
        {
            folder.createDirectory();
        }

        ~ScopedFolder() { folder.deleteRecursively(); }

        juce::File folder;
    };

    constexpr int rate = 48000;
    constexpr int block = 256;
    constexpr int inputChannels = 4;

    /*  A length that is no multiple of the block, so a capture a block out
        of place reads another frame's value and not the same one again. */
    constexpr int inputLength = 4801;

    /*  Which frame and which channel, in the value itself: the channel in the
        units, the frame in the fraction - never nought, so silence is only
        ever a hole. Exact through a 32-bit float WAV both ways. */
    float inputValue (int channel, std::int64_t sample)
    {
        return static_cast<float> (channel) + static_cast<float> (sample % inputLength + 1) * 1.0e-4f;
    }

    /*  The SAME float, bit for bit: what went into a 32-bit float WAV comes out
        exactly, and an equality on floats is a warning the strict job refuses. */
    bool same (float a, float b)
    {
        return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b);
    }

    juce::File writeInputFile (const juce::File& folder)
    {
        const auto file = folder.getChildFile ("inputs.wav");
        juce::AudioBuffer<float> buffer { inputChannels, inputLength };

        for (int channel = 0; channel < inputChannels; ++channel)
            for (int frame = 0; frame < inputLength; ++frame)
                buffer.setSample (channel, frame, inputValue (channel, frame));

        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };
        REQUIRE (stream != nullptr);

        juce::WavAudioFormat format;
        auto writer = format.createWriterFor (stream,
                                              juce::AudioFormatWriterOptions{}
                                                .withSampleRate (static_cast<double> (rate))
                                                .withNumChannels (inputChannels)
                                                .withBitsPerSample (32)
                                                .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioSampleBuffer (buffer, 0, inputLength));
        return file;
    }

    juce::AudioBuffer<float> readWav (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };
        REQUIRE (reader != nullptr);

        juce::AudioBuffer<float> buffer { static_cast<int> (reader->numChannels),
                                          static_cast<int> (reader->lengthInSamples) };
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        return buffer;
    }

    juce::var readSidecar (const audio::InputCapture& capture)
    {
        return juce::JSON::parse (juce::File (juce::String (capture.sidecarPath())).loadFileAsString());
    }

    /*  The hosted pump, its inputs fed from the file, its graph of one track,
        running. */
    struct Pump
    {
        explicit Pump (const juce::File& folder)
            : driver (folder.getChildFile ("engine").getFullPathName().toStdString())
        {
            audio::HostedAudioDriver::Settings settings;
            settings.sampleRate = rate;
            settings.blockSize = block;
            settings.outputChannels = 2;
            settings.inputFile = writeInputFile (folder).getFullPathName().toStdString();
            REQUIRE (driver.open (settings));

            audio::EditSpec spec;
            spec.tracks = 1;
            REQUIRE (driver.host().buildEdit (spec));
            REQUIRE (driver.start());
        }

        audio::HostedAudioDriver driver;
    };
}

TEST_CASE ("input capture: taken up while the hosted pump runs, its sidecar's first sample is the clock's when it began, and each frame is the input on that sample")
{
    ScopedFolder storage;
    Pump pump { storage.folder };

    //  The pump well under way, so a first sample of nought would be a coincidence it cannot be.
    std::this_thread::sleep_for (150ms);

    audio::InputCapture::Settings settings;
    settings.file = storage.folder.getChildFile ("capture.wav").getFullPathName().toStdString();
    settings.firstInput = 2;
    settings.lastInput = 3;
    audio::InputCapture capture { settings };

    const auto before = pump.driver.clock().samplesElapsed();
    REQUIRE (pump.driver.attachCapture (capture));
    const auto after = pump.driver.clock().samplesElapsed();

    std::this_thread::sleep_for (300ms);
    pump.driver.stop();
    capture.stop();

    CHECK (capture.lastError().empty());
    REQUIRE (capture.hasStarted());

    const auto sidecar = readSidecar (capture);
    REQUIRE (sidecar.isObject());

    const auto first = static_cast<juce::int64> (sidecar["firstSample"]);
    CHECK (first == capture.firstSample());
    CHECK (before > 0);
    CHECK (first >= before);
    CHECK (first <= after + block);
    CHECK (first % block == 0);

    CHECK (static_cast<int> (sidecar["sampleRate"]) == rate);
    CHECK (static_cast<int> (sidecar["blockSize"]) == block);
    CHECK (static_cast<int> (sidecar["deviceInputs"]) == inputChannels);
    REQUIRE (sidecar["inputs"].isArray());
    REQUIRE (sidecar["inputs"].size() == 2);
    CHECK (static_cast<int> (sidecar["inputs"][0]) == 2);
    CHECK (static_cast<int> (sidecar["inputs"][1]) == 3);
    CHECK (static_cast<juce::int64> (sidecar["droppedFrames"]) == 0);
    CHECK (static_cast<bool> (sidecar["closed"]));
    CHECK (sidecar["ended"].toString().isEmpty());

    const auto frames = static_cast<juce::int64> (sidecar["frames"]);
    CHECK (frames == capture.framesWritten());
    CHECK (frames >= 10 * block);
    CHECK (frames % block == 0);

    //  Every frame the input the graph was handed on its sample: channels 2 and 3, 1-based - the file's 1 and 2.
    const auto heard = readWav (juce::File (juce::String (settings.file)));
    REQUIRE (heard.getNumChannels() == 2);
    REQUIRE (heard.getNumSamples() == frames);

    auto mismatches = 0;

    for (int frame = 0; frame < heard.getNumSamples(); ++frame)
        for (int channel = 0; channel < 2; ++channel)
            if (! same (heard.getSample (channel, frame), inputValue (channel + 1, first + frame)))
                ++mismatches;

    CHECK (mismatches == 0);
}

TEST_CASE ("input capture: a ring too small for a held writer drops, counts, and writes the hole as silence - the frames after it still on their own samples")
{
    ScopedFolder storage;
    Pump pump { storage.folder };

    audio::InputCapture::Settings settings;
    settings.file = storage.folder.getChildFile ("small.wav").getFullPathName().toStdString();
    settings.firstInput = 1;
    settings.lastInput = 4;
    settings.ringSeconds = 0.0;         // one slot, a block deep
    audio::InputCapture capture { settings };

    capture.holdWriter (true);
    REQUIRE (pump.driver.attachCapture (capture));

    //  Some forty blocks with nowhere to go but the one slot.
    std::this_thread::sleep_for (200ms);
    capture.holdWriter (false);
    std::this_thread::sleep_for (200ms);

    pump.driver.stop();
    capture.stop();

    REQUIRE (capture.hasStarted());
    const auto dropped = capture.framesDropped();
    CHECK (dropped >= 20 * block);
    CHECK (dropped % block == 0);

    const auto sidecar = readSidecar (capture);
    REQUIRE (sidecar.isObject());
    CHECK (static_cast<juce::int64> (sidecar["droppedFrames"]) == dropped);
    CHECK (sidecar["ended"].toString().isEmpty());

    const auto first = static_cast<juce::int64> (sidecar["firstSample"]);
    const auto heard = readWav (juce::File (juce::String (settings.file)));
    REQUIRE (heard.getNumChannels() == 4);
    REQUIRE (heard.getNumSamples() == static_cast<juce::int64> (sidecar["frames"]));

    /*  Silence exactly where blocks were dropped, and every other frame the
        input on its own sample - before the hole and after it. */
    std::int64_t silent = 0;
    auto mismatches = 0;
    auto heardAfterHole = false;

    for (int frame = 0; frame < heard.getNumSamples(); ++frame)
    {
        if (same (heard.getSample (0, frame), 0.0f))
        {
            ++silent;

            for (int channel = 1; channel < 4; ++channel)
                if (! same (heard.getSample (channel, frame), 0.0f))
                    ++mismatches;

            continue;
        }

        heardAfterHole = heardAfterHole || silent > 0;

        for (int channel = 0; channel < 4; ++channel)
            if (! same (heard.getSample (channel, frame), inputValue (channel, first + frame)))
                ++mismatches;
    }

    CHECK (mismatches == 0);
    CHECK (silent > 0);
    CHECK (silent % block == 0);

    //  What was dropped before the last block that landed is in the file as silence; any after it is not.
    CHECK (silent <= dropped);
    CHECK (heardAfterHole);
}

TEST_CASE ("input capture: inputs the interface does not have are refused, saying which; a clock that goes back ends the capture, and the sidecar says why")
{
    ScopedFolder storage;

    SUBCASE ("refused")
    {
        audio::InputCapture::Settings settings;
        settings.file = storage.folder.getChildFile ("refused.wav").getFullPathName().toStdString();
        settings.firstInput = 3;
        settings.lastInput = 6;
        audio::InputCapture capture { settings };

        CHECK_FALSE (capture.prepare (4, rate, 64));
        CHECK_FALSE (capture.isPrepared());
        CHECK (capture.lastError().find ("3 to 6") != std::string::npos);

        audio::InputCapture none { settings };
        CHECK_FALSE (none.prepare (0, rate, 64));
        CHECK (none.lastError().find ("no inputs") != std::string::npos);
    }

    SUBCASE ("a clock that goes back")
    {
        audio::InputCapture::Settings settings;
        settings.file = storage.folder.getChildFile ("back.wav").getFullPathName().toStdString();
        audio::InputCapture capture { settings };
        REQUIRE (capture.prepare (2, rate, 64));
        CHECK (capture.inputs() == std::vector<int> { 1, 2 });

        std::vector<float> left (64, 0.25f), right (64, -0.25f);
        const float* inputs[] = { left.data(), right.data() };

        //  This thread the audio thread: two blocks in a row, then the count starting again.
        capture.write (inputs, 2, 64, 1000);
        capture.write (inputs, 2, 64, 1064);
        capture.write (inputs, 2, 64, 0);
        capture.write (inputs, 2, 64, 64);

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (capture.lastError().empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for (5ms);

        capture.stop();

        CHECK (capture.firstSample() == 1000);
        CHECK (capture.framesWritten() == 128);
        CHECK (capture.lastError().find ("clock moved") != std::string::npos);

        const auto sidecar = readSidecar (capture);
        REQUIRE (sidecar.isObject());
        CHECK (static_cast<juce::int64> (sidecar["firstSample"]) == 1000);
        CHECK (static_cast<juce::int64> (sidecar["frames"]) == 128);
        CHECK (sidecar["ended"].toString().contains ("clock moved"));
        CHECK (static_cast<bool> (sidecar["closed"]));

        //  And nothing more taken once it has ended.
        CHECK_FALSE (capture.prepare (2, rate, 64));
    }
}
