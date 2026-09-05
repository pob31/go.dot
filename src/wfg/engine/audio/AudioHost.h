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

#pragma once

#include <wfg/engine/clock/SampleClock.h>

#include <memory>
#include <string>

/*  Tracktion Engine, stood up with no audio hardware, and the sample counter it
    advances.

    THIS HEADER NAMES NO TRACKTION TYPE, and the reason is not tidiness. Two
    traps live in those headers and both are recorded in Console.cpp:
    tracktion_engine.h bare-`#undef`s `__TEXT`, and tracktion_engine_playback.cpp
    `#undef`s and redefines `VERSION` mid-translation-unit. A pimpl keeps both
    inside one .cpp instead of leaking into every test that wants to pump a
    block. It also keeps the compile cost where it is paid once.

    WHY A HOST AT ALL, RATHER THAN CALLING TRACKTION FROM THE ENGINE. Because
    PRD §3.25 inverts the usual arrangement: Go.dot owns time and Tracktion is a
    player it commands. The tick thread owns the model and must never touch a
    Tracktion ValueTree - every one of those writes asserts the message thread -
    so the two live behind a seam from the first line rather than being
    separated later, when there would be call sites to find.

    WHAT THIS CLASS DOES NOT DO YET. It opens no device: the hosted interface is
    driven by whoever calls processBlock(), which in tests is the test and in
    PR 2.7 will be a real audio callback. It generates no Edit. Both arrive in
    their own commits; this one answers the first question, which is whether the
    engine comes up at all in our build without touching the user's machine.
*/
namespace wfg::audio
{
    /** How the hosted audio interface is opened. No defaults: a rate Go.dot
        chose for itself is a rate nobody chose, which is the same reason
        `wfg serve` refuses to guess one. */
    struct HostSettings
    {
        int sampleRate = 0;
        int blockSize = 0;

        /** Hardware output channels the graph is built to fill. */
        int outputChannels = 0;
    };

    //==============================================================================
    class AudioHost
    {
    public:
        /*  `storageFolder` is where Tracktion writes its own preferences and
            cache. It is a parameter and not a default because a test that wrote
            into the real application-data directory would leave a trace of
            itself on the machine that ran it, and because two tests running at
            once would then share one. */
        explicit AudioHost (std::string storageFolder);
        ~AudioHost();

        AudioHost (const AudioHost&) = delete;
        AudioHost& operator= (const AudioHost&) = delete;

        /** Brings the engine up and opens the hosted interface. False if the
            settings are unusable or the engine could not be created; `lastError`
            says which. Message thread. */
        bool start (const HostSettings& settings);

        /** Message thread. Safe to call without a matching start. */
        void stop();

        bool isRunning() const noexcept;

        /** Why the last start failed, empty if it did not. */
        const std::string& lastError() const noexcept;

        /*  One block through the graph, advancing the sample counter by exactly
            the block size.

            THE AUDIO THREAD CALLS THIS, from PR 2.7. Today it is called by a
            test, which is what makes a test deterministic: the graph advances
            when the test says so and not when a device interrupt says so.
            Either way it is the only thing that moves the clock. */
        void processBlock();

        /** Blocks processed since the last start. Any thread. */
        std::int64_t blocksProcessed() const noexcept;

        /*  The sample counter, for TickThread. It only ever moves forward: the
            type is an AudioClockSource and not a ManualClock precisely so that
            nothing on this path can rewind it. */
        const SampleClock& clock() const noexcept;

        /** The settings the interface was opened with. Zeroed when stopped. */
        const HostSettings& settings() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
