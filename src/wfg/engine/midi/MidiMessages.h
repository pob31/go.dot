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

/*  A MIDI CUE'S FIELDS, TURNED INTO THE BYTES THAT LEAVE.

    A PURE FUNCTION OF THE DOCUMENT, which is the whole shape of this file. What
    a cue sends is decided by seven attributes and nothing else - no port, no
    device, no clock - so it can be checked byte for byte on a machine with no
    MIDI interface, and every CI runner is one.

    It is also where the hex of a sysex dump is read, because that is the one
    field a person types from a manual and therefore the one field that will be
    typed wrong. What comes back says which.

    NOT JUCE'S MidiMessage. JUCE's factories are convenient and opinionated -
    they clamp, they renumber channels, and `noteOn` with velocity nought
    becomes a note-off - and every one of those opinions is a place where what
    the document says and what leaves the socket could differ. §3.10 asks for
    every event type; this writes them.
*/

#pragma once

#include <wfg/engine/midi/MidiSink.h>

#include <string>

namespace wfg::midi
{
    /** One MIDI cue, as the document holds it. */
    struct MessageSpec
    {
        std::string type { "noteOn" };
        int channel = 1;          ///< 1..16, as every device prints it
        int number = 0;           ///< note, controller or program
        int data = 0;             ///< velocity, value, or the 14-bit bend
        std::string sysex;        ///< hex bytes, F0 … F7
    };

    struct BuiltMessage
    {
        Bytes bytes;

        /** From `sendError`, when the fields do not make a message. */
        std::string problem;

        bool ok() const noexcept { return problem.empty(); }
    };

    /*  The bytes for one cue, or the reason there are none.

        REFUSES RATHER THAN CLAMPS. A channel of 17 or a velocity of 300 is
        somebody having meant something this cue cannot do, and sending the
        nearest legal message instead is how a show goes out wrong quietly. The
        schema already refuses both when the document is written; this is the
        second net, and it is the one that catches a value that arrived over the
        wire.
    */
    BuiltMessage messageFor (const MessageSpec& spec);

    /*  The bytes of a hex string, or nothing when it is not one.

        Whitespace between bytes is ignored and case is not significant, so
        "F0 7E 00 06 01 F7" and "f07e00060 1f7" both work - the first is how a
        manual prints it and the second is how it arrives when somebody deletes
        a space by accident. An odd number of digits is refused, because half a
        byte is not a byte.

        Public because it is worth testing on its own: it is the one part of a
        MIDI cue a person types character by character. */
    bool hexBytes (const std::string& text, Bytes& out);
}
