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

/*  WHAT EACH DECLARED PORT IS ACTUALLY PLUGGED INTO, and what this machine has
    to plug it into.

    The document says a show has a port called "Lights". Whether anything is
    behind it tonight is a fact about this building, so it is not stored: it is
    kept here and published, exactly as a mount's `loaded` and `problem` are.
    PRD §4.10 is the line between them - the document holds what somebody
    decided, never what the machine happened to find.

    WHY BOTH HALVES OF A DEVICE ARE REMEMBERED (author, 2026-09-22, asked as
    "what is best if switching USB ports?"). `juce::MidiDeviceInfo::identifier`
    is operating-system formatted and is not documented as stable: on Windows it
    carries the device's instance path, so moving a cable to another socket
    usually changes it. A NAME survives that move and cannot tell two identical
    interfaces apart. So a port stores both and the binder tries the identifier
    first, falling back to the name - WFS-DIY's own rule, whose source comment
    gives this reasoning and cites JUCE's `openLastRequestedMidiDevices`.

    AND WHERE NEITHER FITS, OR WHERE A NAME FITS TWO, THE PORT STAYS UNBOUND
    AND SAYS WHY. A MIDI cue arriving at the wrong desk is worse than one that
    does not arrive, so an ambiguous name is a refusal rather than a guess, and
    `problemOf` is the sentence a client shows for it.

    NO JUCE HERE, deliberately. This is a plain record that the cue layer, the
    parameter tree and their tests can all hold; the two files that own JUCE's
    MIDI headers stay `MidiInputs.h` and `MidiSender.h`, which is what lets a
    MIDI port be tested on a runner with no ports at all.
*/

#include <map>
#include <string>
#include <vector>

namespace wfg::midi
{
    /*  What this machine has, by the names a person would type, and what each
        one's identifier is. Read when the show opens and again on a rescan,
        never on the tick thread: enumerating MIDI devices blocks for
        milliseconds on Windows. */
    struct Device
    {
        std::string name;
        std::string identifier;
    };

    class PortTable
    {
    public:
        /*  What a declared port ended up plugged into. `problem` is empty when
            it is plugged into something. */
        struct Binding
        {
            bool bound = false;
            std::string problem;

            /** The identifier actually matched, which the engine writes back. */
            std::string deviceId;
        };

        void setBinding (const std::string& portId, Binding binding)
        {
            bindings[portId] = std::move (binding);
        }

        void forget (const std::string& portId) { bindings.erase (portId); }

        bool isBound (const std::string& portId) const
        {
            const auto found = bindings.find (portId);
            return found != bindings.end() && found->second.bound;
        }

        std::string problemOf (const std::string& portId) const
        {
            const auto found = bindings.find (portId);
            return found == bindings.end() ? std::string {} : found->second.problem;
        }

        std::string deviceIdOf (const std::string& portId) const
        {
            const auto found = bindings.find (portId);
            return found == bindings.end() ? std::string {} : found->second.deviceId;
        }

        //======================================================================
        void setDevices (std::vector<Device> in, std::vector<Device> out)
        {
            inputs_ = std::move (in);
            outputs_ = std::move (out);
        }

        const std::vector<Device>& inputs() const noexcept { return inputs_; }
        const std::vector<Device>& outputs() const noexcept { return outputs_; }

        /*  The names, one per line. NEWLINE and not space, because a device
            name contains spaces and nothing else would split it back - the
            same argument `document/warnings` makes. */
        static std::string namesOf (const std::vector<Device>& devices)
        {
            std::string out;

            for (const auto& device : devices)
            {
                if (! out.empty())
                    out += '\n';

                out += device.name;
            }

            return out;
        }

        //======================================================================
        /*  THE MATCH, and it is the whole of the author's decision in one
            function so that both sides of the cable use the same rule and a
            test can hand it a list with no hardware in the room.

            The identifier first, because it is the only thing that tells two
            identical interfaces apart. Then the name, because it is the only
            thing that survives a cable moving to another socket. A name that
            fits two devices is refused rather than guessed.

            Answers the device, or nothing with `why` filled in. */
        static const Device* match (const std::vector<Device>& devices,
                                    const std::string& wantedName,
                                    const std::string& wantedId,
                                    std::string& why)
        {
            why.clear();

            if (wantedName.empty() && wantedId.empty())
                return nullptr;

            if (! wantedId.empty())
                for (const auto& device : devices)
                    if (device.identifier == wantedId)
                        return &device;

            if (wantedName.empty())
            {
                why = "the device it was bound to is not on this machine any more";
                return nullptr;
            }

            const Device* found = nullptr;
            auto seen = 0;

            for (const auto& device : devices)
                if (device.name == wantedName)
                {
                    found = &device;
                    ++seen;
                }

            if (seen == 1)
                return found;

            if (seen > 1)
            {
                why = "this machine has " + std::to_string (seen) + " devices called \""
                      + wantedName + "\" and nothing says which one is meant";
                return nullptr;
            }

            why = "this machine has no MIDI device called \"" + wantedName + "\"";
            return nullptr;
        }

    private:
        std::map<std::string, Binding> bindings;
        std::vector<Device> inputs_, outputs_;
    };
}
