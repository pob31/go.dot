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
    The sound card, on a machine that may not have one.

    THE HARD PART OF TESTING THIS IS THAT IT CANNOT BE TESTED, on the machines
    where it matters most: no CI runner has an audio interface, and the two
    machines that do are the author's. So the line these cases hold is between
    what is true of the CODE and what is true of the MACHINE, and every one of
    them has to say which it is asserting.

    What is true of the code: enumeration answers rather than throwing, an empty
    list is a fact and not an error, a device that will not open is reported and
    not skipped, and asking for a device nobody has fails with a message naming
    it rather than a crash or a silence.

    What is true of the machine: whether opening one works, at what rate, and
    whether the graph keeps up. Those run WHEN THERE IS A DEVICE and are skipped
    - reported as skipped, in the output, never silently - when there is not.

    THAT IS THE ONLY SKIP IN THIS SUITE, and it is not the SKIP_RETURN_CODE kind
    the project refuses elsewhere. Nothing here reports green without running:
    the case still executes, still asserts everything that does not need
    hardware, and says in its own output that the machine had nothing to open.
    A hardware checklist in docs/ is what covers the rest, on the two boxes that
    can answer it.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/audio/DeviceLayer.h>

#include <juce_core/juce_core.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    /** A folder of our own, so Tracktion's preferences never touch the machine. */
    struct ScopedRoom
    {
        ScopedRoom()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("wfg-device-test-" + juce::Uuid().toDashedString()))
        {
        }

        ~ScopedRoom() { folder.deleteRecursively(); }

        std::string path() const { return folder.getFullPathName().toStdString(); }

        juce::File folder;
    };

    // Only the callback writes these peaks; the test reads after close() joins it.
    struct DevicePeaks final : audio::BlockSink
    {
        void blockProduced (const float* const* channels, int count, int frames) noexcept override
        {
            for (int channel = 0; channel < std::min (count, 2); ++channel)
                if (channels[channel] != nullptr)
                    for (int sample = 0; sample < frames; ++sample)
                        peaks[channel] = std::max (peaks[channel], std::abs (channels[channel][sample]));
            if (peaks[1] > 0.0005f)
                received.store (true, std::memory_order_relaxed);
        }
        float peaks[2] {};
        std::atomic<bool> received { false };
    };

    juce::File writeQuietTone (const juce::File& folder)
    {
        folder.createDirectory();
        const auto file = folder.getChildFile ("quiet-tone.wav");
        std::unique_ptr<juce::OutputStream> stream { file.createOutputStream() };
        juce::WavAudioFormat format;
        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
            .withSampleRate (48000).withNumChannels (1).withBitsPerSample (16));
        if (writer == nullptr) return {};
        juce::AudioBuffer<float> samples (1, 48000 * 3);
        for (int n = 0; n < samples.getNumSamples(); ++n)
            samples.setSample (0, n, 0.001f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                       * 440.0f * static_cast<float> (n) / 48000.0f));
        if (! writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples())) return {};
        return file;
    }
}

TEST_CASE ("devices: media reaches the patched hardware output")
{
    // Exercise one usable device per API, including ASIO when installed. A quiet
    // sine (-60 dBFS) goes through the real callback, never a test-driven pump.
    const auto devices = audio::availableDevices();
    std::vector<std::string> testedTypes;
    int tested = 0;
    for (const auto& device : devices)
    {
        if (device.outputChannels < 2
            || std::find (testedTypes.begin(), testedTypes.end(), device.type) != testedTypes.end())
            continue;
        ScopedRoom room;
        const auto tone = writeQuietTone (room.folder);
        REQUIRE (tone.existsAsFile());
        DevicePeaks capture; // Outlives the driver, even on an assertion failure.
        audio::DeviceAudioDriver driver { room.path() };
        audio::DeviceAudioDriver::Request request;
        request.deviceType = device.type;
        request.deviceName = device.name;
        request.inputDeviceName = std::string {};
        request.logicalOutputs = 2;
        request.outputPatch = { 1, 0 };
        request.outputObserver = &capture;
        request.edit.tracks = 1;
        request.edit.channelsPerTrack = 1;
        if (! driver.open (request))
        {
            MESSAGE ("media callback test could not open " << device.type << " / "
                      << device.name << ": " << driver.lastError());
            continue;
        }
        testedTypes.push_back (device.type);
        ++tested;
        INFO (device.type << " / " << device.name);
        REQUIRE (driver.host().setTrackSource (0, 0, tone.getFullPathName().toStdString()));
        driver.host().setTrackRouting (0, 0.0, { { 0.0, 0.0, 1.0 } });
        for (int i = 0; i < 1000 && ! driver.host().isTrackSourceReady (0); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        REQUIRE (driver.host().isTrackSourceReady (0));
        const auto target = driver.host().clock().samplesElapsed() + driver.settings().blockSize * 4;
        REQUIRE (driver.host().launchTrackAt (0, 0, driver.host().beatsAtSample (target)));
        for (int i = 0; i < 1000 && ! capture.received.load (std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        REQUIRE (capture.received.load (std::memory_order_relaxed));
        driver.reconnect();
        const auto pausedAt = driver.host().clock().samplesElapsed();
        const auto playedAt = driver.host().trackPlayState (0).playedBeats;
        CHECK (driver.recoveryPaused());
        CHECK_FALSE (driver.resumeConnection());
        bool recovered = false;
        for (int i = 0; i < 1000 && ! recovered; ++i)
        {
            recovered = driver.serviceRecovery().ready;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }
        REQUIRE_MESSAGE (recovered, driver.lastError());
        CHECK (driver.host().clock().samplesElapsed() == pausedAt);
        CHECK (driver.host().trackPlayState (0).playedBeats == playedAt);
        capture.received.store (false, std::memory_order_relaxed);
        REQUIRE (driver.resumeConnection());
        for (int i = 0; i < 1000 && ! capture.received.load (std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        CHECK (capture.received.load (std::memory_order_relaxed));
        CHECK (driver.host().clock().samplesElapsed() > pausedAt);
        CHECK (driver.host().trackPlayState (0).playedBeats >= playedAt);
        driver.close();
        MESSAGE (device.type << " / " << device.name << ": final device peaks "
                  << capture.peaks[0] << ", " << capture.peaks[1]);
        CHECK (capture.peaks[0] == 0.0f);
        CHECK (capture.peaks[1] > 0.0005f);
        CHECK (capture.peaks[1] < 0.0011f);
    }
    if (tested == 0)
        MESSAGE ("no usable output device; hardware media/patch verification did not run");
}

//==============================================================================
TEST_CASE ("devices: a machine with nothing to play through is a fact, not a failure")
{
    /*  Every CI runner is that machine, which is the whole reason this has to
        be true: a verb whose only testable outcome is failure is a verb nobody
        can put a gate on. */
    const auto devices = audio::availableDevices();

    MESSAGE ("this machine has " << devices.size() << " audio device(s)");

    for (const auto& device : devices)
        MESSAGE ("  " << device.type << " / " << device.name
                  << ": " << device.outputChannels << " out, "
                  << device.inputChannels << " in, "
                  << device.sampleRates.size() << " rate(s)");

    /*  The assertion is that it ANSWERED. An empty vector and a full one are
        both correct; a throw, a hang or a crash are not, and on a headless
        runner scanning drivers is exactly where those live. */
    CHECK (devices.size() == devices.size());
}

TEST_CASE ("devices: everything listed says what it is and where it belongs")
{
    /*  A name with no type is a name nobody can pass to `--device-type=`, and a
        device listed twice under one type is a list that cannot be indexed. */
    const auto devices = audio::availableDevices();

    for (const auto& device : devices)
    {
        INFO ("device " << device.type << " / " << device.name);

        CHECK_FALSE (device.type.empty());
        CHECK_FALSE (device.name.empty());
        CHECK (device.outputChannels >= 0);
        CHECK (device.inputChannels >= 0);

        /*  Rates are ascending and distinct where a driver reported any. A list
            with 48000 in it twice is a driver bug worth seeing rather than
            tidying away, so this asserts the shape rather than fixing it. */
        CHECK (std::is_sorted (device.sampleRates.begin(), device.sampleRates.end()));
    }

    for (const auto& device : devices)
    {
        const auto twins = std::count_if (devices.begin(), devices.end(),
                                          [&device] (const audio::DeviceDescription& other)
                                          {
                                              return other.type == device.type
                                                       && other.name == device.name;
                                          });

        INFO ("device " << device.type << " / " << device.name);
        CHECK (twins == 1);
    }
}

TEST_CASE ("devices: asking for one nobody has fails, and says which one")
{
    /*  A show that names a device the venue does not have is the ordinary
        Tuesday of touring, and what it needs from the engine is the NAME back.
        "could not open audio device" sends somebody to look at the machine;
        "no device called MADIface USB" sends them to look at the show, which is
        where the problem is. */
    ScopedRoom room;
    audio::DeviceAudioDriver driver { room.path() };

    audio::DeviceAudioDriver::Request request;
    request.deviceName = "A Device That Does Not Exist (Go.dot test)";
    request.edit.tracks = 1;
    request.edit.channelsPerTrack = 1;

    CHECK_FALSE (driver.open (request));
    CHECK_FALSE (driver.isRunning());

    INFO ("error: " << driver.lastError());
    CHECK_FALSE (driver.lastError().empty());
    CHECK (driver.lastError().find ("Go.dot test") != std::string::npos);
}

TEST_CASE ("devices: closing one that never opened is quiet")
{
    /*  The shutdown path runs on the way out of every failure above it, so it
        has to survive being called on an object that got nowhere. */
    ScopedRoom room;
    audio::DeviceAudioDriver driver { room.path() };

    driver.close();
    driver.close();

    CHECK_FALSE (driver.isRunning());
    CHECK (driver.settings().sampleRate == 0);
    CHECK (driver.deviceName().empty());
}

//==============================================================================
TEST_CASE ("devices: a real one opens, reports what it granted, and feeds the graph")
{
    /*  THE CASE THAT NEEDS A MACHINE. It runs on the author's two boxes and on
        nothing in CI, and it says so in its own output rather than passing
        quietly.

        WHAT IT IS REALLY ABOUT is PRD §6.2: the rate is OBSERVED, never set. A
        driver may open at a rate nobody asked for - a Dante interface follows
        its clock domain and will report 48 kHz while being asked for 96 - and a
        program that believed its own request would run every cue at the wrong
        speed with no way of noticing. So the assertion is not "it opened at
        what I asked": it is that what it reports is what the engine was brought
        up on. */
    const auto devices = audio::availableDevices();

    const auto usable = std::find_if (devices.begin(), devices.end(),
                                      [] (const audio::DeviceDescription& device)
                                      {
                                          return device.outputChannels >= 2;
                                      });

    if (usable == devices.end())
    {
        MESSAGE ("no device with two outputs on this machine - "
                 "the rest of this case needs one and did not run");
        return;
    }

    MESSAGE ("opening " << usable->type << " / " << usable->name);

    ScopedRoom room;
    audio::DeviceAudioDriver driver { room.path() };

    audio::DeviceAudioDriver::Request request;
    request.deviceName = usable->name;
    request.deviceType = usable->type;
    request.blockSize = 256;
    request.edit.tracks = 2;
    request.edit.channelsPerTrack = 1;

    if (! driver.open (request))
    {
        /*  A DEVICE THAT WILL NOT OPEN IS A REPORT, NOT A FAILED TEST. The
            machine's default output is often exclusive to something else - a
            conferencing app, a browser tab, another copy of this suite running
            in parallel under the other locale - and none of that is Go.dot
            being wrong. What would be a failure is opening it and lying. */
        MESSAGE ("could not open it: " << driver.lastError());
        CHECK_FALSE (driver.isRunning());
        return;
    }

    CHECK (driver.isRunning());

    INFO ("granted " << driver.settings().sampleRate << " Hz, "
           << driver.settings().blockSize << " frames, "
           << driver.settings().outputChannels << " outputs");

    MESSAGE ("granted " << driver.settings().sampleRate << " Hz / "
              << driver.settings().blockSize << " frames / "
              << driver.settings().outputChannels << " outputs on \""
              << driver.deviceName() << "\"");

    /*  WHAT IT GRANTED IS WHAT THE ENGINE IS RUNNING ON. Not what was asked
        for - the request above says 256 and a driver is entitled to say no. */
    CHECK (driver.settings().sampleRate > 0);
    CHECK (driver.settings().blockSize > 0);
    CHECK (driver.settings().outputChannels >= 2);
    CHECK_FALSE (driver.deviceName().empty());

    /*  AND THE DEVICE IS DRIVING THE GRAPH. The clock only moves because a
        callback ran, so a counter that has moved is the whole proof that the
        interrupt reached Go.dot's code - which is the one thing about this
        layer that nothing else can establish. */
    const auto before = driver.host().clock().samplesElapsed();

    for (int i = 0; i < 200 && driver.blocksDelivered() < 10; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    const auto after = driver.host().clock().samplesElapsed();

    INFO ("blocks delivered: " << driver.blocksDelivered());
    MESSAGE ("the device delivered " << driver.blocksDelivered()
              << " block(s) and moved the clock by " << (after - before) << " samples");

    CHECK (driver.blocksDelivered() > 0);
    CHECK (after > before);

    /*  The clock moved by whole blocks and by nothing else: the device's
        interrupt is the only thing that advances it, so a count that is not a
        multiple of the block size means something else did. */
    CHECK ((after - before) % driver.settings().blockSize == 0);

    /*  And where its clock took over, which is what the tick clock rebases
        against when a session moves from hosted to hardware. */
    CHECK (driver.switchSample() >= before);

    driver.close();
    CHECK_FALSE (driver.isRunning());
}

//==============================================================================
/*  AN INTERFACE WHOSE CLOCK A TEST CAN MOVE (2026-09-28), and the only way PRD
    §6.2's second failure runs anywhere but the author's rig: no CI runner has
    an interface, and nobody's Dante domain can be moved from a script.

    WHAT IT IS NOT: a model of any driver. It is the two behaviours §6.2 has to
    tell apart - an interface that takes the rate it is asked for (USB, most
    ASIO boxes) and one that keeps the rate its clock domain gives it (Dante,
    anything on word clock) - plus the one that surprised the design: a Dante
    or MADI interface at double speed offers half its channels.

    ONE DOMAIN FOR EVERY DEVICE OBJECT the type makes, because the device
    manager deletes the device and makes a new one on every reopen: the clock
    belongs to the room, not to the object. Message thread, except the block
    thread each device runs while started. */
namespace
{
    struct ClockDomain
    {
        double rate = 48000.0;
        bool settable = false;
        bool halvesAtDoubleSpeed = false;

        std::vector<int> asked;                         // every rate an open asked for
        juce::AudioIODevice* current = nullptr;

        int outputs() const { return halvesAtDoubleSpeed && rate > 50000.0 ? 4 : 8; }
    };

    juce::StringArray channelNames (const juce::String& stem, int count)
    {
        juce::StringArray out;

        for (int i = 1; i <= count; ++i)
            out.add (stem + " " + juce::String (i));

        return out;
    }

    juce::BigInteger within (juce::BigInteger mask, int available)
    {
        if (mask.getHighestBit() >= available)
            mask.setRange (available, mask.getHighestBit() + 1 - available, false);

        return mask;
    }

    class MovableDevice final : public juce::AudioIODevice, private juce::Thread
    {
    public:
        explicit MovableDevice (ClockDomain& domainToUse)
            : juce::AudioIODevice ("Movable clock", "Go.dot test clock"),
              juce::Thread ("movable clock"),
              domain (domainToUse)
        {
            domain.current = this;
        }

        ~MovableDevice() override
        {
            close();

            if (domain.current == this)
                domain.current = nullptr;
        }

        juce::StringArray getOutputChannelNames() override    { return channelNames ("out", domain.outputs()); }
        juce::StringArray getInputChannelNames() override     { return channelNames ("in", 2); }
        juce::Array<double> getAvailableSampleRates() override { return { 48000.0, 96000.0 }; }
        juce::Array<int> getAvailableBufferSizes() override   { return { 256 }; }
        int getDefaultBufferSize() override                   { return 256; }

        juce::String open (const juce::BigInteger& inputs, const juce::BigInteger& outputs,
                           double sampleRate, int bufferSize) override
        {
            close();
            domain.asked.push_back (static_cast<int> (sampleRate));

            if (domain.settable && sampleRate > 0.0)
                domain.rate = sampleRate;

            takeTheDomain (inputs, outputs);
            block = bufferSize > 0 ? bufferSize : 256;
            opened = true;
            return {};
        }

        void close() override       { stop(); opened = false; }
        bool isOpen() override      { return opened; }

        void start (juce::AudioIODeviceCallback* callbackToUse) override
        {
            if (! opened || callbackToUse == nullptr || callback != nullptr)
                return;

            callbackToUse->audioDeviceAboutToStart (this);
            callback = callbackToUse;
            startThread();
        }

        void stop() override
        {
            if (callback == nullptr)
                return;

            stopThread (2000);
            std::exchange (callback, nullptr)->audioDeviceStopped();
        }

        bool isPlaying() override                            { return callback != nullptr; }
        juce::String getLastError() override                 { return {}; }
        int getCurrentBufferSizeSamples() override           { return block; }
        double getCurrentSampleRate() override               { return rate; }
        int getCurrentBitDepth() override                    { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
        juce::BigInteger getActiveInputChannels() const override  { return activeInputs; }
        int getOutputLatencyInSamples() override             { return 0; }
        int getInputLatencyInSamples() override              { return 0; }

        /*  THE DOMAIN MOVES, and the driver resets itself the way a
            clock-slaved one does: stopped, and started again at whatever the
            domain now runs at, asking nobody - which is also what a settable
            interface does when somebody changes its rate at its own panel. */
        void moveClock (double newRate)
        {
            auto* was = callback;
            stop();
            domain.rate = newRate;
            takeTheDomain (activeInputs, activeOutputs);

            if (was != nullptr)
                start (was);
        }

    private:
        void takeTheDomain (const juce::BigInteger& inputs, const juce::BigInteger& outputs)
        {
            rate = domain.rate;
            activeInputs = within (inputs, 2);
            activeOutputs = within (outputs, domain.outputs());
        }

        void run() override
        {
            const auto ins = activeInputs.countNumberOfSetBits();
            const auto outs = activeOutputs.countNumberOfSetBits();
            juce::AudioBuffer<float> input (std::max (1, ins), block);
            juce::AudioBuffer<float> output (std::max (1, outs), block);
            const auto period = std::max (1.0, 1000.0 * block / rate);

            while (! threadShouldExit())
            {
                input.clear();
                output.clear();
                callback->audioDeviceIOCallbackWithContext (input.getArrayOfReadPointers(), ins,
                                                            output.getArrayOfWritePointers(), outs,
                                                            block, {});
                wait (period);
            }
        }

        ClockDomain& domain;
        juce::AudioIODeviceCallback* callback = nullptr;
        juce::BigInteger activeInputs, activeOutputs;
        double rate = 48000.0;
        int block = 256;
        bool opened = false;
    };

    class MovableType final : public juce::AudioIODeviceType
    {
    public:
        explicit MovableType (ClockDomain& domainToUse)
            : juce::AudioIODeviceType ("Go.dot test clock"), domain (domainToUse)
        {
        }

        void scanForDevices() override {}
        juce::StringArray getDeviceNames (bool) const override { return { "Movable clock" }; }
        int getDefaultDeviceIndex (bool) const override        { return 0; }
        bool hasSeparateInputsAndOutputs() const override      { return false; }

        int getIndexOfDevice (juce::AudioIODevice* device, bool) const override
        {
            return device != nullptr ? 0 : -1;
        }

        juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
        {
            if (output != "Movable clock" && input != "Movable clock")
                return nullptr;

            return new MovableDevice (domain);
        }

    private:
        ClockDomain& domain;
    };

    audio::DeviceAudioDriver::Request movableRequest()
    {
        audio::DeviceAudioDriver::Request request;
        request.deviceName = "Movable clock";
        request.deviceType = "Go.dot test clock";
        request.edit.tracks = 1;
        request.edit.channelsPerTrack = 2;
        return request;
    }

    /*  Blocks through the graph, which is the whole claim: a device that
        called back into a closed gate delivers none. */
    bool graphFed (audio::DeviceAudioDriver& driver, std::int64_t blocks = 20)
    {
        const auto from = driver.blocksDelivered();

        for (int i = 0; i < 500; ++i)
        {
            if (driver.blocksDelivered() - from >= blocks)
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        return false;
    }

    /*  The watchdog's own loop, run here the way the message thread runs it:
        a look every twenty milliseconds until the outage has come to what the
        case waits for, for at most ten seconds. */
    template <typename Until>
    audio::DeviceAudioDriver::Recovery serviceUntil (audio::DeviceAudioDriver& driver, Until until)
    {
        audio::DeviceAudioDriver::Recovery last;

        for (int i = 0; i < 500; ++i)
        {
            last = driver.serviceRecovery();

            if (until (last))
                return last;

            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        return last;
    }

    MovableDevice& movableOf (ClockDomain& domain)
    {
        REQUIRE (domain.current != nullptr);
        return static_cast<MovableDevice&> (*domain.current);
    }
}

TEST_CASE ("devices: a clock the interface will not give back is followed, on the interface as it runs")
{
    /*  THE DANTE CASE (PRD §6.2, decisions DF and DH): the domain moved from
        48 kHz to 96, the interface came back on it, was asked for 48 again
        and would not. It is the same interface - with half its channels, when
        it is the kind that halves them - so the show follows it: the engine
        brought up again on the device as it runs, without closing it. Until
        then the paused graph never runs on the new clock, which would be
        every cue at twice its pitch. */
    for (const auto halves : { false, true })
    {
        const auto* what = halves ? "half the channels at double speed" : "every channel at double speed";
        INFO (what);

        ClockDomain domain;
        domain.halvesAtDoubleSpeed = halves;

        ScopedRoom room;
        audio::DeviceAudioDriver driver { room.path() };
        driver.addDeviceType (std::make_unique<MovableType> (domain));

        const auto request = movableRequest();
        REQUIRE_MESSAGE (driver.open (request), driver.lastError());
        CHECK (driver.settings().sampleRate == 48000);
        CHECK (driver.outputChannels() == 8);
        REQUIRE (graphFed (driver));
        CHECK_FALSE (driver.recoveryPaused());

        domain.asked.clear();
        movableOf (domain).moveClock (96000.0);
        CHECK (driver.recoveryPaused());

        const auto moved = serviceUntil (driver, [] (const auto& r) { return r.moved; });
        REQUIRE (moved.moved);
        CHECK_FALSE (moved.ready);
        CHECK (moved.sampleRate == 96000);
        CHECK (moved.blockSize == 256);

        //  Asked for the clock the show ran on first, and refused.
        CHECK (std::find (domain.asked.begin(), domain.asked.end(), 48000) != domain.asked.end());
        CHECK (static_cast<int> (domain.rate) == 96000);

        //  Held until it is followed: nothing reaches the old graph meanwhile.
        const auto held = driver.blocksDelivered();
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        CHECK (driver.blocksDelivered() == held);
        CHECK (driver.recoveryPaused());

        REQUIRE (driver.followClock (request) == audio::DeviceAudioDriver::Follow::followed);
        CHECK (driver.lastError().empty());
        CHECK (driver.settings().sampleRate == 96000);
        CHECK (driver.host().settings().sampleRate == 96000);
        CHECK (driver.outputChannels() == (halves ? 4 : 8));
        CHECK_FALSE (driver.recoveryPaused());
        REQUIRE (graphFed (driver));

        //  Followed, it is the show's clock now: nothing moved, nothing to follow.
        CHECK_FALSE (driver.serviceRecovery().moved);
        CHECK (driver.followClock (request) == audio::DeviceAudioDriver::Follow::nothing);

        driver.close();
        CHECK_FALSE (driver.isRunning());
    }
}

TEST_CASE ("devices: a clock the interface gives back when asked is put back, and the paused show goes on")
{
    /*  THE USB CASE, and the reason a moved clock is asked back before it is
        followed (decision DF): an interface power-cycled mid-show comes back
        at its own default rate, and one that takes the rate it is asked for
        takes the old one again - so the outage stays the pause §6.2 promises,
        on the graph and the launch handles it had, rather than a stop nobody
        needed. */
    ClockDomain domain;
    domain.settable = true;

    ScopedRoom room;
    audio::DeviceAudioDriver driver { room.path() };
    driver.addDeviceType (std::make_unique<MovableType> (domain));

    REQUIRE_MESSAGE (driver.open (movableRequest()), driver.lastError());
    REQUIRE (graphFed (driver));

    movableOf (domain).moveClock (96000.0);
    CHECK (driver.recoveryPaused());
    const auto paused = driver.host().clock().samplesElapsed();

    const auto back = serviceUntil (driver, [] (const auto& r) { return r.ready || r.moved; });
    REQUIRE (back.ready);
    CHECK_FALSE (back.moved);
    CHECK (static_cast<int> (domain.rate) == 48000);
    CHECK (driver.settings().sampleRate == 48000);
    CHECK (driver.host().clock().samplesElapsed() == paused);

    REQUIRE (driver.resumeConnection());
    CHECK_FALSE (driver.recoveryPaused());
    REQUIRE (graphFed (driver));

    //  The same graph going on, not a new one: its clock resumed from where it paused.
    CHECK (driver.host().clock().samplesElapsed() > paused);
    CHECK (driver.followClock (movableRequest()) == audio::DeviceAudioDriver::Follow::nothing);

    driver.close();
}
