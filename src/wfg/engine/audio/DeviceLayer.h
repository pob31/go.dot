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

#pragma once

/*
    The sound card, and the same graph running off it.

    WHAT THIS IS NOT: a second engine. `wfg serve --hosted` and `wfg serve` on a
    device are the same program with a different block source. The hosted driver
    paces AudioHost::processBlock off a steady_clock; this paces it off a device
    interrupt. Everything downstream - the tick clock, the Runner, the whole
    control plane - sees a SampleClock and cannot tell which one it got, which
    is the property that makes a render on a CI runner evidence about a show.

    THE RATE IS OBSERVED, NEVER SET (PRD §6.2). Go.dot asks a device to open and
    then reads back what it actually opened at: a Dante interface follows its
    clock domain and will tell you 48 kHz while you are asking for 96, and a
    program that believed its own request would run every cue at the wrong speed
    and have no way of noticing. The observed rate is what reaches the tick clock
    and what `audio.deviceStarted` reports.

    ALL CHANNELS, WITH EXPLICIT MASKS, which is spatcore's DeviceHost policy used
    as-is rather than re-derived. juce::AudioDeviceManager keeps a
    `useDefaultInputChannels` / `useDefaultOutputChannels` pair defaulting to
    true, and while either is set it throws away the caller's channel mask and
    substitutes a count frozen at the last initialise() - so code that builds a
    full mask and sets it has no effect at all, and the app meters channels the
    driver never opened. spatcore hit that and wrote the policy down; this uses
    the header.

    THREADS. `availableDevices` and everything on DeviceAudioDriver except the
    callback is message-thread work. The callback IS the audio thread, and what
    it does is one call into the graph and one copy out: PRD §4.2 applies to it
    exactly as it applies to the hosted pump.
*/

#include <wfg/engine/audio/AudioHost.h>

#include <memory>
#include <string>
#include <vector>

namespace juce
{
    class AudioDeviceManager;
}

namespace wfg::audio
{
    /*  One thing this machine could play through.

        Rates and buffer sizes are what the DEVICE says it supports, which is
        not the same as what it will actually open at - a driver may decline,
        and the truth is only knowable after the open. This is for `wfg devices`
        to print, so somebody can find out what to type. */
    struct DeviceDescription
    {
        std::string type;                 ///< "Windows Audio", "CoreAudio", "ASIO", "ALSA"
        std::string name;
        int inputChannels = 0;
        int outputChannels = 0;
        std::vector<double> sampleRates;
        std::vector<int> bufferSizes;

        /** Whether its type is the one JUCE would pick unasked. */
        bool isDefaultType = false;
    };

    /*  Every device, of every type this build can speak.

        NEVER THROWS AND NEVER REFUSES. A machine with no sound card returns an
        empty list, which is a fact rather than an error - every CI runner is
        such a machine, and `wfg devices` has to exit 0 on one or the verb
        cannot be tested anywhere it matters.

        Scanning is slow and touches drivers, so this is not something to call
        in a loop. Message thread. */
    std::vector<DeviceDescription> availableDevices();

    //==============================================================================
    /*  A show running off a real interface.

        Owns the device manager, the open policy and an AudioHost, and hands the
        graph a block every time the device asks for one.
    */
    class DeviceAudioDriver
    {
    public:
        explicit DeviceAudioDriver (std::string storageFolder);
        ~DeviceAudioDriver();

        DeviceAudioDriver (const DeviceAudioDriver&) = delete;
        DeviceAudioDriver& operator= (const DeviceAudioDriver&) = delete;

        struct Request
        {
            /** Empty for whatever the machine would choose unasked. */
            std::string deviceName;

            /** Empty for the platform's default type. */
            std::string deviceType;

            /*  What to ASK for. What is granted is in `settings()` afterwards,
                and the two differ often enough that believing the request is a
                bug rather than an optimisation. */
            int blockSize = 0;

            /** The show's track count and channel width, for the Edit. */
            EditSpec edit;
        };

        /*  Opens the device, brings the engine up on the rate it actually got,
            builds the Edit and starts the callback. Message thread.

            False with `lastError` set when the device could not be opened, the
            show is wider than the device, or the graph could not be built. */
        bool open (const Request&);

        /** Message thread. Safe without a matching open. */
        void close();

        bool isRunning() const noexcept;
        const std::string& lastError() const noexcept;

        /** What the device actually opened at. Zeroed when closed. */
        const HostSettings& settings() const noexcept;

        /** The device that is open, or empty. */
        const std::string& deviceName() const noexcept;

        AudioHost& host() noexcept;

        /*  Blocks the device has asked for since the open, and how late the
            last one was against its own deadline. The device owns the schedule
            here, so lateness is a diagnostic about the machine rather than
            about Go.dot - but it is published the same way and read from the
            same node, because an operator watching it does not care whose
            fault it is until it is not zero. */
        std::int64_t blocksDelivered() const noexcept;

        /*  The sample at which the device's clock took over, for the tick
            clock's rebase. Zero before the first callback. */
        std::int64_t switchSample() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
