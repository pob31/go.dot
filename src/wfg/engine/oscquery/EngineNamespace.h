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
    The seam, wired to the real engine.

    OscQueryServer knows OSCQuery and nothing about Go.dot; this is the file
    that knows both, and it is deliberately the only one. Everything
    Go.dot-shaped that the server needs - where a snapshot comes from, what a
    write becomes, who is touching what - is answered here and nowhere else.

    WHAT A WRITE BECOMES is the one real decision in this file, and there are
    exactly two answers:

      * an address under `/godot/cmd/` is a COMMAND. `/godot/cmd/standby/next`
        invokes `standby.next`, with the packet's arguments as its parameters.
        The mapping is the inverse of the one ParameterTree publishes, dots for
        slashes, and it is written here beside a note saying so because two
        halves of one convention in two files is how they drift apart.

      * anything else is `node.set`, with the address as its first argument and
        the value as its second. There is no third case: PRD 4.11 says every
        gesture-reachable action is a named command, and a socket that could
        reach the document any other way would be that rule's exception.

    NOTHING HERE APPLIES ANYTHING. Both answers are submitted to the queue and
    applied by the tick thread in arrival order, stamped with the origin the
    server captured. The server's threads never touch the model, and this class
    is where that stops being a convention and becomes a function call.
*/

#include <wfg/engine/oscquery/OscQueryServer.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/tree/Touches.h>

#include <memory>
#include <string>

namespace wfg::oscquery
{
    class EngineNamespace final : public Namespace
    {
    public:
        EngineNamespace (Engine& engineToDrive,
                         tree::ParameterTree& treeToRead,
                         tree::TouchTable& touchesToConsult,
                         int udpPort);

        //  --- Namespace -------------------------------------------------------
        std::shared_ptr<const tree::TreeSnapshot> snapshot() const override;
        void write (const std::string& origin, const osc::Packet& packet) override;
        void forget (const std::string& origin) override;

        bool shouldPush (const std::string& toOrigin,
                         const std::string& address,
                         const std::string& causedBy) const override;

        int oscPort() const override { return udp; }

        //======================================================================
        /*  `/godot/cmd/standby/next` -> `standby.next`, and an empty string for
            an address that is not a command.

            Exposed rather than kept private so a test can assert the two halves
            of the convention against each other: ParameterTree turns a command
            name into an address, this turns it back, and a round trip through
            both is the only thing that keeps them honest. */
        static std::string commandNameFor (const std::string& address);

    private:
        Engine& engine;
        tree::ParameterTree& parameters;
        tree::TouchTable& touches;
        int udp = 0;
    };
}
