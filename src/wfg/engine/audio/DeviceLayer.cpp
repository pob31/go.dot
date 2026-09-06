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

#include <wfg/engine/audio/DeviceLayer.h>

#include <juce_audio_devices/juce_audio_devices.h>
#include <spatcore/io/DeviceHost.h>

#include <algorithm>
#include <atomic>

namespace wfg::audio
{
    namespace
    {
        /*  A manager with its device types scanned, and nothing opened.

            The scan has to happen before the types can be listed OR set:
            `setCurrentAudioDeviceType` on an unscanned manager takes a
            no-device-type early exit, opens nothing, and returns an EMPTY error
            string - which is the worst possible combination, because it reads
            as success. spatcore's DeviceHost documents the same trap and this is
            the same call. */
        void scanTypes (juce::AudioDeviceManager& manager)
        {
            manager.getAvailableDeviceTypes();          // populates, does not open
        }
    }

    //==============================================================================
    std::vector<DeviceDescription> availableDevices()
    {
        std::vector<DeviceDescription> out;

        juce::AudioDeviceManager manager;
        scanTypes (manager);

        const auto& types = manager.getAvailableDeviceTypes();
        const auto defaultType = manager.getCurrentAudioDeviceType();

        for (auto* type : types)
        {
            if (type == nullptr)
                continue;

            /*  A rescan rather than a cached list: a device plugged in since
                the manager was built is a device somebody is about to ask for
                by name, and finding out it is not there because a list was
                stale is the least useful failure available. */
            type->scanForDevices();

            for (const auto& name : type->getDeviceNames (false))
            {
                DeviceDescription described;
                described.type = type->getTypeName().toStdString();
                described.name = name.toStdString();
                described.isDefaultType = type->getTypeName() == defaultType;

                /*  OPENED TO BE ASKED, and closed again. A device type can
                    list a name without knowing anything about its channels or
                    rates; only the device object knows, and creating one is the
                    only way to get it. This is why `wfg devices` is slow and
                    why nothing calls it in a loop.

                    A device that will not open at all is still LISTED, with
                    zeroes: "it is there and I could not open it" is a different
                    thing to tell somebody than "it is not there", and it is the
                    one that sends them to look at the driver. */
                std::unique_ptr<juce::AudioIODevice> device {
                    type->createDevice (name, name) };

                if (device != nullptr)
                {
                    described.inputChannels = device->getInputChannelNames().size();
                    described.outputChannels = device->getOutputChannelNames().size();

                    for (const auto rate : device->getAvailableSampleRates())
                        described.sampleRates.push_back (rate);

                    for (const auto size : device->getAvailableBufferSizes())
                        described.bufferSizes.push_back (size);
                }

                out.push_back (std::move (described));
            }
        }

        return out;
    }

    //==============================================================================
    struct DeviceAudioDriver::Impl final : public juce::AudioIODeviceCallback
    {
        explicit Impl (std::string folder) : audioHost (std::move (folder)) {}

        ~Impl() override { closeDevice(); }

        //======================================================================
        /*  Where the graph's block goes: straight into the buffers the device
            handed the callback.

            The pointers are written and read on the AUDIO THREAD and on no
            other, which is what makes them safe without a lock - the callback
            sets them, calls the graph, and the graph calls back into here
            before the callback returns. AudioHost's own note about setting a
            sink with the audio stopped is about a sink CHANGING; this one is
            installed once and only its contents move. */
        struct DeviceSink final : BlockSink
        {
            void blockProduced (const float* const* channels, int numChannels,
                                int numSamples) noexcept override
            {
                if (destination == nullptr)
                    return;

                const auto rows = std::min (numChannels, destinationChannels);
                const auto frames = std::min (numSamples, destinationFrames);

                for (int channel = 0; channel < rows; ++channel)
                    if (destination[channel] != nullptr && channels[channel] != nullptr)
                        juce::FloatVectorOperations::copy (destination[channel],
                                                           channels[channel], frames);
            }

            float* const* destination = nullptr;
            int destinationChannels = 0;
            int destinationFrames = 0;
        };

        //======================================================================
        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            /*  THE OBSERVED RATE, and it is read here rather than remembered
                from the request because this is the first moment the truth
                exists. A Dante interface follows its clock domain; a driver may
                round a buffer size; an exclusive-mode device may refuse both.
                What the device says here is what the show runs at. */
            observedRate.store (device->getCurrentSampleRate(), std::memory_order_relaxed);
            observedBlock.store (device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
            delivered.store (0, std::memory_order_relaxed);
        }

        void audioDeviceStopped() override {}

        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            /*  Cleared first and unconditionally. Every early return below is a
                block the graph did not fill, and a device buffer that is not
                written is whatever was in it last time - which on a PA is a
                loop of the last 3 ms, at full level, for as long as it lasts. */
            for (int channel = 0; channel < numOutputChannels; ++channel)
                if (outputChannelData[channel] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[channel], numSamples);

            if (numSamples != audioHost.settings().blockSize)
            {
                /*  THE DEVICE CHANGED ITS MIND ABOUT THE BLOCK SIZE. Some
                    drivers do, mid-stream. The graph is sized for one number
                    and rendering a different one would read past buffers that
                    were allocated for it, so this reports and stays silent
                    rather than guessing. */
                mismatches.fetch_add (1, std::memory_order_relaxed);
                return;
            }

            sink.destination = outputChannelData;
            sink.destinationChannels = numOutputChannels;
            sink.destinationFrames = numSamples;

            audioHost.processBlock();

            sink.destination = nullptr;

            const auto count = delivered.fetch_add (1, std::memory_order_relaxed) + 1;

            /*  WHERE THE DEVICE'S CLOCK TOOK OVER, recorded once. The tick
                clock rebases at the tick boundary after this sample, so that a
                session which started hosted and moved onto hardware does not
                have a discontinuity in the middle of a tick. */
            if (count == 1)
                switchAt.store (audioHost.clock().samplesElapsed(),
                                std::memory_order_relaxed);
        }

        //======================================================================
        void closeDevice()
        {
            manager.removeAudioCallback (this);
            manager.closeAudioDevice();
            audioHost.setBlockSink (nullptr);
            audioHost.stop();
        }

        juce::AudioDeviceManager manager;
        spatcore::io::DeviceHost policy { manager };

        AudioHost audioHost;
        DeviceSink sink;

        HostSettings granted;
        std::string openedName;
        std::string error;

        std::atomic<double> observedRate { 0.0 };
        std::atomic<int> observedBlock { 0 };
        std::atomic<std::int64_t> delivered { 0 };
        std::atomic<std::int64_t> switchAt { 0 };
        std::atomic<std::int64_t> mismatches { 0 };
        bool running = false;
    };

    //==============================================================================
    DeviceAudioDriver::DeviceAudioDriver (std::string storageFolder)
        : impl (std::make_unique<Impl> (std::move (storageFolder)))
    {
    }

    DeviceAudioDriver::~DeviceAudioDriver() = default;

    bool DeviceAudioDriver::open (const Request& request)
    {
        close();
        impl->error.clear();

        /*  INITIALISED FIRST, ALWAYS, EVEN WHEN A NAME WAS GIVEN.

            This was a bug for exactly as long as it took to run the test.
            `openNamedDevice` sets the device type and then names the device,
            and on a manager that has never been initialised the second half
            answers "No such device" for a device the enumeration had just
            listed by that exact name. spatcore's policy assumes a manager an
            application has already brought up - its own consumers restore from
            saved state on launch - and reading the header rather than its call
            sites is what missed it.

            So: bring the manager up with no device open (`selectDefaultOnFailure
            = false`, so a machine with nothing does not sit through a full
            rescan), and only then ask for one by name. The empty-name case is
            the same call with the default allowed. */
        scanTypes (impl->manager);

        const auto wantsDefault = request.deviceName.empty();

        if (const auto brought = impl->manager.initialise (0, 512, nullptr, wantsDefault);
            brought.isNotEmpty() && wantsDefault)
        {
            impl->error = brought.toStdString();
            return false;
        }

        if (! wantsDefault)
        {
            /*  THE CHOSEN TYPE'S OWN DEVICE LIST, SCANNED, and this is the
                second half of the same trap.

                spatcore's `ensureDeviceTypesScanned` touches the TYPE list,
                which is what stops setAudioDeviceSetup taking its silent
                no-device-type exit. It does not scan the devices WITHIN a type,
                and JUCE only does that lazily on paths that happen to ask. So a
                device this build had just enumerated by name came back as "No
                such device" from the very next call - the list it was looked up
                in was empty.

                One line, and it is here rather than in spatcore because
                spatcore's own consumers restore from saved state on launch and
                never take this path. */
            if (! request.deviceType.empty())
                impl->manager.setCurrentAudioDeviceType (juce::String (request.deviceType), true);

            if (auto* type = impl->manager.getCurrentDeviceTypeObject())
                type->scanForDevices();

            /*  NAMED AS AN INPUT ONLY IF IT IS ONE, which is the rule
                spatcore's setDeviceAllChannels does not have.

                That function sets the input device name and the output device
                name to the same string unconditionally. It is right for the
                interfaces it was written for - a WFS rig's RME is one device
                with both halves - and wrong for anything that only plays: this
                machine's built-in output has no inputs at all, so naming it as
                one made JUCE look the name up in an empty list and answer "No
                such device" for a device the enumeration had just printed.

                The FIX IS NOT "never ask for inputs" (author, 2026-09-06). A
                rack has them, and on Windows most users will be on ASIO, where
                there is one device for both directions and no separate
                selection to make - so refusing inputs there would be refusing
                half of the only device on offer. The rule is that a device is
                named as an input when it HAS inputs, which is true of an
                interface and false of a pair of speakers, and needs no flag.

                The POLICY is still spatcore's and is the part worth reusing:
                explicit channel masks with both `useDefault…Channels` flags
                cleared, because while either is set setAudioDeviceSetup throws
                the caller's mask away and substitutes a count frozen at the
                last initialise. That is the half nobody discovers by reading
                the API. */
            auto hasInputs = false;

            if (auto* type = impl->manager.getCurrentDeviceTypeObject())
                hasInputs = type->getDeviceNames (true)
                              .contains (juce::String (request.deviceName));

            auto setup = impl->manager.getAudioDeviceSetup();

            setup.inputDeviceName = hasInputs ? juce::String (request.deviceName)
                                              : juce::String {};
            setup.outputDeviceName = juce::String (request.deviceName);
            setup.useDefaultInputChannels = false;
            setup.useDefaultOutputChannels = false;
            setup.inputChannels.clear();
            setup.outputChannels.clear();

            if (hasInputs)
                setup.inputChannels.setRange (0, 512, true);

            setup.outputChannels.setRange (0, 512, true);

            /*  Zero means "the device's own", which is the only pair certain to
                be valid: the previous device's rate and block size may be
                nothing this one supports. */
            setup.sampleRate = 0;
            setup.bufferSize = 0;

            if (const auto problem = impl->manager.setAudioDeviceSetup (setup, true);
                problem.isNotEmpty())
            {
                impl->error = problem.toStdString();
                return false;
            }
        }

        /*  Trimmed to what the device really has, now that it exists to be
            asked. The mask above was provisional - 512 channels is not a claim,
            it is "as many as there are" before anybody knows how many that is.

            BOTH DIRECTIONS, because a device that has inputs was opened with
            them: PRD §3.25 makes Tracktion a commanded player and Phase 2
            records nothing, but the rack will have inputs and an ASIO device
            hands you both halves whether or not today's show wants one. Holding
            them open now is what makes that a show-document question later
            rather than a device-layer one. */
        if (auto* opened = impl->manager.getCurrentAudioDevice())
        {
            const auto current = impl->manager.getAudioDeviceSetup();
            auto setup = current;

            setup.useDefaultInputChannels = false;
            setup.useDefaultOutputChannels = false;
            setup.inputChannels.clear();
            setup.outputChannels.clear();
            setup.inputChannels.setRange (0, opened->getInputChannelNames().size(), true);
            setup.outputChannels.setRange (0, opened->getOutputChannelNames().size(), true);

            /*  Compared against what was REQUESTED rather than against the
                device's active masks: a device that declines to open everything
                it was asked for would otherwise be reopened on every call, for
                ever. spatcore's note, and its reasoning. */
            if (setup.inputChannels != current.inputChannels
                  || setup.outputChannels != current.outputChannels)
                if (const auto trimmed = impl->manager.setAudioDeviceSetup (setup, true);
                    trimmed.isNotEmpty())
                {
                    impl->error = trimmed.toStdString();
                    return false;
                }
        }

        auto* device = impl->manager.getCurrentAudioDevice();

        if (device == nullptr)
        {
            impl->error = request.deviceName.empty()
                            ? "this machine has no audio device to open"
                            : "no device called \"" + request.deviceName + "\"";
            return false;
        }

        /*  A buffer size is a REQUEST, and the device answers. Asking through
            the setup rather than assuming means the number below is the one the
            driver granted, which may be neither what was asked nor a power of
            two. */
        if (request.blockSize > 0)
        {
            auto setup = impl->manager.getAudioDeviceSetup();
            setup.bufferSize = request.blockSize;

            if (const auto refused = impl->manager.setAudioDeviceSetup (setup, true);
                refused.isNotEmpty())
            {
                impl->error = refused.toStdString();
                return false;
            }

            device = impl->manager.getCurrentAudioDevice();

            if (device == nullptr)
            {
                impl->error = "the device closed itself when asked for a buffer of "
                                + std::to_string (request.blockSize);
                return false;
            }
        }

        HostSettings settings;
        settings.sampleRate = static_cast<int> (device->getCurrentSampleRate());
        settings.blockSize = device->getCurrentBufferSizeSamples();
        settings.outputChannels = device->getActiveOutputChannels().countNumberOfSetBits();

        if (settings.outputChannels <= 0)
        {
            impl->error = "\"" + device->getName().toStdString()
                            + "\" opened with no output channels";
            return false;
        }

        /*  THE ENGINE IS BROUGHT UP ON WHAT THE DEVICE GAVE, never on what was
            asked for. This line is the whole of PRD §6.2's "the rate is
            observed": everything downstream - the tick clock's samples per
            tick, the Edit's tempo, the launch-tick arithmetic - is computed
            from the number the driver reported, so a device that opened at
            44.1 when it was asked for 48 makes a show that is in tune. */
        if (! impl->audioHost.start (settings))
        {
            impl->error = impl->audioHost.lastError();
            return false;
        }

        if (! impl->audioHost.buildEdit (request.edit))
        {
            impl->error = impl->audioHost.lastError();
            impl->audioHost.stop();
            return false;
        }

        impl->granted = settings;
        impl->openedName = device->getName().toStdString();

        impl->audioHost.setBlockSink (&impl->sink);
        impl->manager.addAudioCallback (impl.get());
        impl->running = true;
        return true;
    }

    void DeviceAudioDriver::close()
    {
        if (! impl->running)
            return;

        impl->closeDevice();
        impl->granted = {};
        impl->openedName.clear();
        impl->running = false;
    }

    //==============================================================================
    bool DeviceAudioDriver::isRunning() const noexcept          { return impl->running; }
    const std::string& DeviceAudioDriver::lastError() const noexcept { return impl->error; }
    const HostSettings& DeviceAudioDriver::settings() const noexcept { return impl->granted; }
    const std::string& DeviceAudioDriver::deviceName() const noexcept { return impl->openedName; }
    AudioHost& DeviceAudioDriver::host() noexcept               { return impl->audioHost; }

    std::int64_t DeviceAudioDriver::blocksDelivered() const noexcept
    {
        return impl->delivered.load (std::memory_order_relaxed);
    }

    std::int64_t DeviceAudioDriver::switchSample() const noexcept
    {
        return impl->switchAt.load (std::memory_order_relaxed);
    }
}
