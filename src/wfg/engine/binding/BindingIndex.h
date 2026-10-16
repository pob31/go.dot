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
    THE SHOW'S BINDINGS, READ ONCE AND MATCHED (namespace draft §74, AQX).

    Built from the document on the tick thread whenever the show changes, and
    held by shared_ptr and swapped whole, as the trigger index is: the MIDI and
    OSC threads read a copy of the pointer and match against something nothing
    can change under them. The matchers are pure functions over it, so they are
    tested with no port and no socket.

    A MIDI MESSAGE IS DECODED HERE, from its bytes: a controller, a note's
    velocity, pitch bend and channel pressure, which the trigger's event does
    not carry. A pair of controllers for 14 bits matches on either half - the
    number and the number plus 32 - and the half not in this message is the
    bridge's to remember.
*/

#include <wfg/engine/binding/Scaling.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::binding
{
    enum class Source { midi, osc };
    enum class MidiType { controlChange, controlChange14, noteVelocity, pitchBend, channelPressure };
    enum class Control { absolute, relative, rate };

    struct Binding
    {
        std::string id;
        std::string name;
        std::string cue;            ///< empty for one live while the show is open
        bool enabled = true;

        Source source = Source::midi;
        std::string port;           ///< midi: a declared port, by identifier; empty is any
        int channel = 0;            ///< midi: 1..16, nought any
        MidiType type = MidiType::controlChange;
        int number = 0;

        std::string address;        ///< osc
        int argument = 0;           ///< osc

        Control control = Control::absolute;
        std::string target;         ///< a node's address, or a row of `cue` by name
        Scale scale;
        bool pickup = true;
        bool feedback = false;

        /*  THE ADDRESS IT WRITES: `target` as written when it is an address, or
            `/godot/cue/<cue>/<target>` when it names a row of its cue. Empty
            when it names nothing it could write. */
        std::string targetAddress() const;
    };

    struct BindingIndex
    {
        std::vector<Binding> bindings;

        /** Reads them out of a document. Tick thread only. */
        static std::shared_ptr<const BindingIndex> build (const doc::ShowDocument& document);

        const Binding* find (const std::string& id) const;
    };

    //==========================================================================
    /*  A MIDI MESSAGE, AS A BINDING READS IT. `value` is the controller's
        value, the velocity, the pressure, or the whole of a pitch bend (nought
        to 16383). A note-on of velocity nought is a note-off, which moves
        nothing. */
    struct MidiArrival
    {
        enum class Kind { controlChange, noteOn, pitchBend, channelPressure };

        Kind kind = Kind::controlChange;
        int channel = 1;
        int number = 0;
        int value = 0;
    };

    std::optional<MidiArrival> decodeMidi (const std::uint8_t* bytes, std::size_t size) noexcept;

    /*  THE BINDINGS A MIDI MESSAGE ON `port` MOVES, switched on, in document
        order: its port (or any), its channel (or any), its kind and number - a
        14-bit pair on either of its two controllers. */
    std::vector<const Binding*> matchMidi (const BindingIndex& index, const std::string& port,
                                           const MidiArrival& arrival);

    /** And an OSC message on Go.dot's port, by its address. */
    std::vector<const Binding*> matchOsc (const BindingIndex& index, const std::string& address);

    /*  WHAT A MIDI VALUE IS on the control's travel, nought to one: a 7-bit
        value of 127, a 14-bit one of 16383. */
    double midiFraction (MidiType type, int value) noexcept;
}
