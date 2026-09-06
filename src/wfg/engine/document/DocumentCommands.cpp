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

#include <optional>
#include <string>

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
    namespace
    {
        /*  An OSC value as the canonical text the schema parses.

            Numbers go through the project's own formatter rather than
            std::to_string or a stream, for the reason recorded at length in
            osc/OscValue.cpp: everything else here writes the shortest text that
            reads back as the identical value, and a write path that did not
            would let a client set a number the document could not store.

            Nothing for a blob, a nil, an impulse or a time tag. No row in the
            table is any of those, so a value of one of those types is a type
            mismatch rather than something to be coerced into a string. */
        std::optional<std::string> canonicalText (const osc::Value& value)
        {
            if (value.isString())  return value.getString();
            if (value.isBool())    return std::string (value.getBool() ? "true" : "false");
            if (value.isInt32())   return std::to_string (value.getInt32());
            if (value.isInt64())   return std::to_string (value.getInt64());
            if (value.isFloat32()) return osc::formatFloat (value.getFloat32());
            if (value.isFloat64()) return osc::formatDouble (value.getFloat64());

            return std::nullopt;
        }
    }

    void registerDocumentCommands (CommandRegistry& registry, ShowDocument& document,
                                   ForeignWrite foreign)
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
        registry.add ({ "route.create",
                        "Adds a destination to a media cue: the bus it feeds. The coefficients"
                        " are written afterwards, like any other value.",
                        { { "cue", 's', false }, { "bus", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createRoute (args[0].getString(),
                                                                    args[1].getString(),
                                                                    id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "trigger.create",
                        "Adds a trigger to a cue: what fires it when nobody presses GO.",
                        { { "cue", 's', false }, { "kind", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createTrigger (args[0].getString(),
                                                                      args[1].getString(),
                                                                      id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "group.role",
                        "Gives a group its header or its footer - the cue lists that run before"
                        " its members and at its exit. Asking twice answers with the one it has.",
                        { { "group", 's', false }, { "role", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createRole (args[0].getString(),
                                                                   args[1].getString(),
                                                                   id);

                            return fromEdit (edit, withId (args, 2, edit.id));
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

            THE VALUE IS DECLARED '*', which is the registry's "whatever the
            target says". A write to a `d` node carries a double and a write to
            an `s` node carries a string, and there is no way to know which
            until the address has been resolved - which is the draft's point
            that node.set has no /cmd node because its signature IS the
            target's.

            Nothing is loosened by that. Every value takes the same road it
            always did: it becomes canonical text, and the schema parses that
            text against the row the address resolves to. So a client sending
            the string "3" to an integer node and one sending the integer 3
            produce the identical document, and neither can put a word into a
            number - the check simply happens one layer in, where the type is
            actually known, rather than at a parameter list that cannot know
            it. */
        registry.add ({ "node.set",
                        "Sets one value, by its address in the parameter tree.",
                        { { "address", 's', false }, { "value", '*', false } },
                        true,
                        [&document, foreign = std::move (foreign)]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto address = args[0].getString();

                            /*  THE DOCUMENT FIRST, ALWAYS. `/godot` is Go.dot's
                                and a mount prefix may not be `/`, so the two
                                can never both claim an address - but asking the
                                show first means a mount could never shadow it
                                even if that rule were ever relaxed. */
                            if (foreign && address.rfind ("/godot", 0) != 0)
                                return foreign (address, args[1]);

                            const auto text = canonicalText (args[1]);

                            if (! text.has_value())
                                return Outcome::rejected (reason::typeMismatch);

                            return fromEdit (document.setAttribute (address, *text), args);
                        } });
    }
}
