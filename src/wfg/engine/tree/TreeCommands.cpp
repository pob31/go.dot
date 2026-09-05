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

#include <wfg/engine/tree/TreeCommands.h>

#include <string>

namespace wfg::tree
{
    namespace
    {
        /*  The origin the transport stamped on the event.

            An event with no origin at all is an engine-internal one, and those
            do not touch anything - so a touch without an origin is refused
            rather than filed under the empty string, where it would silence a
            node for every client that also has no origin. */
        const std::string* originOf (const CommandContext& context)
        {
            if (context.origin == nullptr || context.origin->empty())
                return nullptr;

            return context.origin;
        }
    }

    void registerTreeCommands (CommandRegistry& registry, TouchTable& touches)
    {
        //----------------------------------------------------------------------
        registry.add ({ "node.touch",
                        "Holds a node for this origin: it stops receiving pushes for it until"
                        " it releases.",
                        { { "address", 's', false } },
                        true,
                        [&touches] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto* origin = originOf (context);

                            if (origin == nullptr)
                                return Outcome::rejected (reason::badAddress);

                            const auto& address = args[0].getString();

                            if (address.empty())
                                return Outcome::rejected (reason::badAddress);

                            /*  Touching something already held is applied, not
                                rejected. A surface that sends touch on every
                                movement rather than only on the first is doing
                                something reasonable, and an R record per frame
                                would bury the log in noise about nothing. */
                            touches.touch (*origin, address);
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "node.release",
                        "Gives a node back: this origin receives its current value once, and"
                        " pushes for it resume.",
                        { { "address", 's', false } },
                        true,
                        [&touches] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto* origin = originOf (context);

                            if (origin == nullptr)
                                return Outcome::rejected (reason::badAddress);

                            const auto& address = args[0].getString();

                            if (address.empty())
                                return Outcome::rejected (reason::badAddress);

                            /*  Releasing something not held is applied too, and
                                for a sturdier reason than the one above: a
                                surface reconnecting after a drop has no idea
                                what the engine still thinks it holds, and the
                                honest thing for it to do is release everything
                                it might have. Refusing would punish exactly the
                                client that is trying to get back in step. */
                            touches.release (*origin, address);
                            return Outcome::ok (args);
                        } });
    }
}
