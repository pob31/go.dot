// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*  A CONSOLE'S ECHOES ON THE MIDI WIRE (namespace draft §57, AFL; DP.10).

    A console on MIDI over TCP, or on a cable, says what it does: an Allen &
    Heath desk sends the same Note On for a mute pressed on its surface as
    it takes for one, and the same NRPN for a fader moved by hand. Those are
    the device's own reports, heard as a datagram under its prefix is heard
    (namespace draft §45, O.8): kept, logged as `mount.heard`, never written
    back. This is the reverse of `renderMidi`: the bytes as they came, in any
    cut the stream made of them, parsed with the running status a console
    uses, and each complete message matched against the shapes the mount's
    nodes declared - a note by its channel and number, an NRPN by its channel
    and parameter, a Control Change by its channel and controller - to the
    address and the value the console is reporting.

    One parser per mount, since a stream's running status and a message cut
    in two belong to the connection they came on. Learned on the tick thread
    from the mount table; fed from the link's packets on the tick, and from a
    MIDI port's input on its own thread, so the parsers sit behind a lock a
    push_back's worth of waiting long. */

#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace wfg::tree
{
    class MidiEchoes
    {
    public:
        struct Heard
        {
            std::string address;
            osc::Values values;
        };

        /*  Learns a mount's shapes anew: every node whose MIDI shape an echo
            can name - note, nrpn, cc - keyed by what the console sends for it,
            on the channel the shape answers on from the mount's base channel.
            A Program Change names no node: a scene recalled is an event. */
        void learn (const std::string& mountId, const std::vector<const Node*>& nodes, int baseChannel);

        void forget (const std::string& mountId);

        /*  Bytes as they came under one mount, any cut: the reports the
            messages they complete amount to, in order. A message no node
            answers to is dropped; a System Exclusive is skipped whole. */
        std::vector<Heard> feed (const std::string& mountId, const std::vector<std::uint8_t>& bytes);

    private:
        //  channel 1..16, the kind ('n' note, 'c' control change, 'p' NRPN), two numbers
        using Key = std::tuple<int, char, int, int>;

        struct Target
        {
            std::string address;
            MidiShape shape;
        };

        struct Parser
        {
            std::uint8_t status = 0;
            std::vector<std::uint8_t> data;
            bool inSysEx = false;
            int nrpnMsb[16] = {};
            int nrpnLsb[16] = {};
            int dataMsb[16] = {};
        };

        void complete (const std::map<Key, Target>& targets, Parser& parser, std::uint8_t status,
                       const std::vector<std::uint8_t>& data, std::vector<Heard>& out) const;

        std::map<std::string, std::map<Key, Target>> learned;
        std::map<std::string, Parser> parsers;
        mutable std::mutex guard;
    };
}
