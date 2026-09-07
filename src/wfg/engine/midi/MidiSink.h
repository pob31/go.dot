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

/*  WHERE A MIDI CUE'S BYTES GO, as the cue layer sees it.

    NAMES NO JUCE TYPE, deliberately, and for the same reason `cue::Player`
    names no Tracktion one: this header is included by the Runner and by tests,
    a null implementation is a complete one, and a show replayed with no sender
    creates the same runs and writes the same log with nothing reaching a port.

    IT IS ALSO WHAT MAKES THE BYTES TESTABLE AT ALL. JUCE creates virtual MIDI
    ports on macOS and Linux and not on Windows, so a test that wanted to hear
    what a cue sent would run on two platforms of three. A recording sink runs
    on all of them and on every CI runner, which have no MIDI hardware at all.

    THE PORT IS THE SHOW'S NAME FOR IT - "Lights", "The desk" - and never a
    device. Binding that name to a cable is `wfg serve --midi-out=<name>=<device>`
    and is a fact about the building (§4.10).
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::midi
{
    /*  The bytes of one MIDI message, as the wire carries them.

        A vector rather than a fixed array because a system-exclusive dump is
        whatever length the manual says, and PRD §3.10 asks for sysex by name.
    */
    using Bytes = std::vector<std::uint8_t>;

    /** Why a send did not happen. Empty means it did. */
    namespace sendError
    {
        /** The show named a port nothing was bound to. */
        inline constexpr const char* noPort = "no-port";

        /** The cue's own fields do not make a message anybody could send. */
        inline constexpr const char* badMessage = "bad-message";
    }

    struct MidiSink
    {
        virtual ~MidiSink() = default;

        /*  Queues one message for a declared port. Returns the reason it could
            not be, or empty when it was taken.

            CALLED FROM THE TICK THREAD, at the flush, and it must not block
            there: on Windows `sendMessageNow` for a system-exclusive message
            busy-waits the calling thread until the port has taken every byte -
            about thirty milliseconds for a hundred bytes at MIDI baud, which is
            a tick and a half of the thread that owns the model. So an
            implementation that talks to hardware queues here and sends
            somewhere else. */
        virtual std::string send (const std::string& port, const Bytes& bytes) = 0;
    };
}
