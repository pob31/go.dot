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

#include <wfg/engine/oscquery/EngineNamespace.h>

#include <algorithm>

namespace wfg::oscquery
{
    namespace
    {
        constexpr std::string_view commandPrefix = "/godot/cmd/";
    }

    //==========================================================================
    EngineNamespace::EngineNamespace (Engine& engineToDrive,
                                      tree::ParameterTree& treeToRead,
                                      tree::TouchTable& touchesToConsult,
                                      const osc::UdpEndpoint& udpEndpoint)
        : engine (engineToDrive),
          parameters (treeToRead),
          touches (touchesToConsult),
          udp (udpEndpoint)
    {
    }

    //==========================================================================
    std::shared_ptr<const tree::TreeSnapshot> EngineNamespace::snapshot() const
    {
        return parameters.snapshot();
    }

    //==========================================================================
    std::string EngineNamespace::commandNameFor (const std::string& address)
    {
        if (address.rfind (commandPrefix, 0) != 0)
            return {};

        auto name = address.substr (commandPrefix.size());

        if (name.empty())
            return {};

        /*  The exact inverse of what ParameterTree publishes
            (ParameterTree.cpp:401-402), which turns `standby.next` into
            `/godot/cmd/standby/next` by replacing dots with slashes. */
        std::replace (name.begin(), name.end(), '/', '.');
        return name;
    }

    //==========================================================================
    void EngineNamespace::write (const std::string& origin, const osc::Packet& packet)
    {
        /*  A bundle is applied element by element, in order, and the time tag
            is ignored in Phase 1. That is not the bundle being mishandled - it
            is the honest thing to do before there is a scheduler to hand it to.
            Phase 4's state solver is where a time tag starts meaning something,
            and pretending to honour one now would make a client believe its
            timing had been respected. */
        if (packet.isBundle())
        {
            for (const auto& element : packet.elements)
                write (origin, element);

            return;
        }

        if (const auto command = commandNameFor (packet.address); ! command.empty())
        {
            engine.submit (Event { origin, command, packet.args });
            return;
        }

        /*  A TRIGGER, and it is asked about BEFORE the argument-less return
            below - because a foot switch that sends a bare address and nothing
            else is the ordinary case, and that return would have swallowed it
            silently as "a write of nothing".

            OVER UDP ONLY. A WebSocket client is a client: it has the whole
            command set and can send `cue.fire`, so a trigger fired from one
            would be a second road to the same place with no advantage and one
            more thing to reason about. §3.7's triggers are device-facing, and
            the devices that send them send datagrams.

            A MATCH ENDS IT. Anything that is not a trigger takes the road it
            always took, which is the node write below - so an address that is
            both a trigger and a node cannot exist, and the load refusal in
            `validate()` is what makes sure of that rather than a rule here. */
        if (origin.rfind ("udp:", 0) == 0)
        {
            std::shared_ptr<const cue::TriggerIndex> index;

            {
                const std::lock_guard<std::mutex> lock { triggerMutex };
                index = triggers;
            }

            if (index != nullptr)
            {
                const auto fired = cue::matchOsc (*index, packet.address, packet.args);

                for (const auto& id : fired)
                    engine.submit (Event { origin, "trigger.fire",
                                           { osc::Value::string (id) } });

                if (! fired.empty())
                    return;
            }
        }

        /*  Everything else is a value written to a node, by its address. The
            engine decides whether that address exists, whether the node is
            writable and whether the type fits; a rejection comes back as an `R`
            record and a reading at /godot/engine/lastError, because OSC has no
            reply channel to put it on. */
        std::vector<osc::Value> args;
        args.reserve (2);
        args.push_back (osc::Value::string (packet.address));

        /*  An argument-less message to a state node is not a write of nothing.
            It has no value to set, so there is nothing to submit and nothing to
            reject - the engine would be asked to store an absence. */
        if (packet.args.empty())
            return;

        args.push_back (packet.args.front());

        engine.submit (Event { origin, "node.set", std::move (args) });
    }

    //==========================================================================
    void EngineNamespace::publishTriggers (std::shared_ptr<const cue::TriggerIndex> index)
    {
        const std::lock_guard<std::mutex> lock { triggerMutex };
        triggers = std::move (index);
    }

    //==========================================================================
    void EngineNamespace::forget (const std::string& origin)
    {
        /*  Released through the QUEUE, not by reaching into the table.

            The touch table is the tick thread's, and a disconnect arrives on a
            server thread. Calling touches.releaseAll() from here would be the
            one place in the engine where a socket thread wrote to the model -
            and it would race the flush that reads the same table.

            So the disconnect becomes a command like any other, and the release
            lands in the log where a replay can reproduce it. A surface that
            crashed mid-gesture is a thing that HAPPENED, and PRD 3.15 says the
            log records what happened. */
        engine.submit (Event { origin, "node.releaseAll", {} });
    }

    //==========================================================================
    bool EngineNamespace::shouldPush (const std::string& toOrigin,
                                      const std::string& address,
                                      const std::string& causedBy) const
    {
        return tree::shouldPush (touches, toOrigin, address, causedBy);
    }
}
