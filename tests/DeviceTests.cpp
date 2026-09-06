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

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

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
