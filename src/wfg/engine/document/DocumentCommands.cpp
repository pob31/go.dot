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

#include <wfg/engine/document/DocumentCommands.h>

#include <wfg/engine/document/CanonicalXml.h>

namespace wfg::doc
{
    namespace
    {
        /*  Every create command takes its identifier as an OPTIONAL last
            argument, and returns the one it used. That single convention is
            what makes replay work without randomness: the engine draws an
            identifier, the log records the call WITH it, and replaying that
            record supplies it rather than drawing again.

            So a handler's job is: do the work, then hand back the arguments as
            they were actually applied. */
        std::vector<osc::Value> withId (std::vector<osc::Value> args,
                                        std::size_t idIndex,
                                        const std::string& id)
        {
            if (args.size() > idIndex)
                args[idIndex] = osc::Value::string (id);
            else
                args.push_back (osc::Value::string (id));

            return args;
        }

        Outcome fromEdit (const EditResult& edit, std::vector<osc::Value> appliedArgs)
        {
            if (! edit.ok)
                return Outcome::rejected (edit.reason);

            return Outcome::ok (std::move (appliedArgs));
        }
    }

    //==============================================================================
    void registerDocumentCommands (CommandRegistry& registry, ShowDocument& document)
    {
        //----------------------------------------------------------------------
        registry.add ({ "list.create",
                        "Creates a cue list. Generates an identifier if none is given.",
                        { { "name", 's', false }, { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto name = args[0].getString();
                            const auto id = args.size() > 1 ? args[1].getString() : std::string {};

                            const auto edit = document.createList (name, id);
                            return fromEdit (edit, withId (args, 1, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "cue.create",
                        "Creates a cue or a group inside a list or a group.",
                        { { "parent", 's', false }, { "index", 'i', false },
                          { "kind", 's', false }, { "name", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 4 ? args[4].getString() : std::string {};

                            const auto edit = document.createCue (args[0].getString(),
                                                                 args[1].getInt32(),
                                                                 args[2].getString(),
                                                                 args[3].getString(),
                                                                 id);

                            return fromEdit (edit, withId (args, 4, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "mount.create",
                        "Declares a foreign namespace to be mounted at a prefix.",
                        { { "prefix", 's', false }, { "namespace", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createMount (args[0].getString(),
                                                                    args[1].getString(),
                                                                    id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        /*  One delete for every kind of object, because the identifier says what
            it is and a second command would only give a caller a way to be
            wrong about it. Deleting a group takes its contents with it. */
        registry.add ({ "object.delete",
                        "Deletes an object and everything inside it.",
                        { { "id", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.remove (args[0].getString()), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "object.move",
                        "Moves an object to a new parent and position.",
                        { { "id", 's', false }, { "parent", 's', false },
                          { "index", 'i', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.move (args[0].getString(),
                                                            args[1].getString(),
                                                            args[2].getInt32()),
                                             args);
                        } });

        //----------------------------------------------------------------------
        /*  The value write, and the only one. PRD §4.11 wants every action to be
            a named command; a property edit is `node.set` against the address
            the parameter tree publishes, so a client that can read the namespace
            can write to it without a second vocabulary.

            The value arrives as text rather than typed. That looks like a step
            backwards and is not: the schema decides the type, so a client that
            sends the string "3" for an integer attribute and one that sends the
            integer 3 produce the identical document — and neither can smuggle a
            string into a numeric attribute, which is exactly the failure a typed
            argument would let through. Phase 1.5 adds the typed form on top of
            this one for the wire. */
        registry.add ({ "node.set",
                        "Sets one value, by its address in the parameter tree.",
                        { { "address", 's', false }, { "value", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.setAttribute (args[0].getString(),
                                                                    args[1].getString()),
                                             args);
                        } });
    }
}
