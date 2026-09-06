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
#include <wfg/engine/cue/TriggerIndex.h>
#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/tree/Touches.h>

#include <memory>
#include <mutex>
#include <string>

namespace wfg::oscquery
{
    class EngineNamespace final : public Namespace
    {
    public:
        /*  The UDP endpoint rather than its port number, and that is what
                makes the wiring order work rather than a style preference. The
                socket's handler needs this object, and this object needs the
                socket's port for HOST_INFO - circular while the port is a
                value. Holding the endpoint breaks it: the endpoint is
                constructed before it is started, so the namespace can be built
                between the two, and boundPort() is atomic and reads correctly
                from the server thread once it is. */
        EngineNamespace (Engine& engineToDrive,
                         tree::ParameterTree& treeToRead,
                         tree::TouchTable& touchesToConsult,
                         const osc::UdpEndpoint& udpEndpoint);

        //  --- Namespace -------------------------------------------------------
        std::shared_ptr<const tree::TreeSnapshot> snapshot() const override;
        void write (const std::string& origin, const osc::Packet& packet) override;
        void forget (const std::string& origin) override;

        bool shouldPush (const std::string& toOrigin,
                         const std::string& address,
                         const std::string& causedBy) const override;

        int oscPort() const override { return udp.boundPort(); }

        //======================================================================
        /*  `/godot/cmd/standby/next` -> `standby.next`, and an empty string for
            an address that is not a command.

            Exposed rather than kept private so a test can assert the two halves
            of the convention against each other: ParameterTree turns a command
            name into an address, this turns it back, and a round trip through
            both is the only thing that keeps them honest. */
        static std::string commandNameFor (const std::string& address);

        /*  WHAT FIRES A CUE FROM OUTSIDE, published here because this is where
            the datagrams arrive.

            Built on the tick thread whenever the document changes and swapped
            whole, exactly as the tree snapshot is - and for the same reason:
            `write` runs on a socket thread, which may not read a document. A
            reader takes a copy of the pointer and is then reading something
            nothing can change under it.

            Empty until somebody publishes one, which is what every test that
            does not care about triggers gets. */
        void publishTriggers (std::shared_ptr<const cue::TriggerIndex> index);

    private:
        Engine& engine;
        tree::ParameterTree& parameters;
        tree::TouchTable& touches;
        const osc::UdpEndpoint& udp;

        mutable std::mutex triggerMutex;
        std::shared_ptr<const cue::TriggerIndex> triggers;
    };
}
