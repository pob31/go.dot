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
#include <vector>

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
        /** Space-separated words, as `order` and the paste's ids are spelled. */
        std::vector<std::string> splitWords (const std::string& text)
        {
            std::vector<std::string> words;
            std::string word;

            for (const auto c : text)
            {
                if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
                {
                    if (! word.empty()) { words.push_back (word); word.clear(); }
                    continue;
                }

                word += c;
            }

            if (! word.empty())
                words.push_back (word);

            return words;
        }

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
        registry.add ({ "audio.configure", "Set the show's audio interface and channel patches as one edit.",
                        { { "enabled", 'T', false }, { "deviceType", 's', false },
                          { "outputDevice", 's', false }, { "inputDevice", 's', false },
                          { "bufferSize", 'i', false }, { "inputPatch", 's', false },
                          { "outputPatch", 's', false } }, true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            audio::AudioSettings settings;
                            settings.enabled = args[0].getBool();
                            settings.deviceType = args[1].getString();
                            settings.outputDevice = args[2].getString();
                            settings.inputDevice = args[3].getString();
                            settings.bufferSize = args[4].getInt32();
                            settings.inputPatch = args[5].getString();
                            settings.outputPatch = args[6].getString();
                            return fromEdit (document.configureAudio (settings), args);
                        } });
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

        registry.add ({ "send.create",
                        "Adds a send from a media cue into one mix channel. The level is written"
                        " afterwards, like any other value. Refuses a second send into a bus this"
                        " cue already sends to.",
                        { { "cue", 's', false }, { "bus", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createSend (args[0].getString(),
                                                                   args[1].getString(),
                                                                   id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        registry.add ({ "group.wrap", "Create a group containing the selected cues in show order.",
                        { { "cues", 's', false }, { "id", 's', true } }, true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto edit = document.groupSelection (splitWords (args[0].getString()),
                                                                       args.size() > 1 ? args[1].getString() : std::string {});
                            return fromEdit (edit, withId (args, 1, edit.id));
                        } });

        registry.add ({ "route.default", "Route an imported media cue to the first output bus, preserving existing assignments.",
                        { { "cue", 's', false }, { "channels", 'i', false }, { "id", 's', true } }, true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto edit = document.defaultMediaRoute (args[0].getString(), args[1].getInt32(),
                                                                          args.size() > 2 ? args[2].getString() : std::string {});
                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "range.create",
                        "Adds a range to a media cue: a named region of its file, and one"
                        " entry in the playlist the cue plays instead of the whole thing.",
                        { { "cue", 's', false }, { "in", 'd', false }, { "out", 'd', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 3 ? args[3].getString() : std::string {};

                            const auto edit = document.createRange (args[0].getString(),
                                                                    args[1].getFloat64(),
                                                                    args[2].getFloat64(),
                                                                    id);

                            return fromEdit (edit, withId (args, 3, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "range.split",
                        "Cuts one of a media cue's ranges in two at a second of its file, the"
                        " second half keeping the first's place in the playlist. Refused when"
                        " that second is not strictly inside a range - on a cut, at the top of"
                        " the file or at its end, there is nothing to divide.",
                        { { "cue", 's', false }, { "at", 'd', false }, { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.splitRange (args[0].getString(),
                                                                   args[1].getFloat64(), id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        /*  PHASE 4'S SLOTS, declared. Four commands rather than one, because
            they are four different objects and §4.11 wants each gesture named:
            a processor input belongs to a mount, a rack channel to the rack,
            and a feed and an insert to a cue. */
        registry.add ({ "slot.create",
                        "Declares one of a processor's inputs on its mount: a slot a cue can"
                        " claim, at an address under that mount's prefix.",
                        { { "mount", 's', false }, { "address", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createSlot (args[0].getString(),
                                                                   args[1].getString(),
                                                                   id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        /*  THE OUTPUT LAYOUT. Four commands rather than writes to
            `Bus/@firstChannel`, because the channels are the running sum of
            the widths before each output and nothing else may set them: a
            client that could write one could leave two outputs summing into
            the same interface channel, and nobody would hear it until the
            night. `document/OutputLayout.h` holds the arithmetic, including
            what each of these does to `audio/@outputPatch`. */
        registry.add ({ "bus.create",
                        "Adds an output to the show: a direct out, where one cue's channels land,"
                        " or a mix channel many cues send into. Width is 1 for mono and 2 for"
                        " stereo. Index is a position in the output list; -1 appends.",
                        /*  `index` IS REQUIRED although -1 is the ordinary
                            answer, because the identifier after it is the
                            optional one: `withId` fills a trailing argument
                            and cannot fill a gap, so a create sent without an
                            index would record its drawn identifier where the
                            index goes and refuse itself on replay. Found by
                            replaying a session rather than by reading. */
                        { { "kind", 's', false }, { "width", 'i', false },
                          { "index", 'i', false }, { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 3 ? args[3].getString() : std::string {};

                            const auto edit = document.createBus (args[0].getString(),
                                                                  args[1].getInt32(),
                                                                  args[2].getInt32(), id);

                            return fromEdit (edit, withId (args, 3, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "bus.delete",
                        "Takes an output away and repacks the ones after it. Every route that"
                        " named it goes too, and every processor input that fed from it is left"
                        " feeding from nowhere - all in one undoable step.",
                        { { "bus", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.removeBus (args[0].getString()), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "bus.move",
                        "Puts an output at another place in the list. Index is a position in the"
                        " list as it stands, as object.move's is.",
                        { { "bus", 's', false }, { "index", 'i', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.moveBus (args[0].getString(),
                                                               args[1].getInt32()), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "bus.width",
                        "Makes an output mono, stereo or wider, and repacks the ones after it."
                        " Narrowing drops the channels at its end.",
                        { { "bus", 's', false }, { "width", 'i', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.resizeBus (args[0].getString(),
                                                                 args[1].getInt32()), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "channel.create",
                        "Adds a channel to the live rack: one position in the pool a cue's insert"
                        " claims. Makes the rack if the show has none.",
                        { { "class", 's', false }, { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 1 ? args[1].getString() : std::string {};

                            const auto edit = document.createRackChannel (args[0].getString(), id);

                            return fromEdit (edit, withId (args, 1, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "feed.create",
                        "Adds a destination to a media cue that is a processor input rather than"
                        " a bus: the audio goes there and the cue claims the slot.",
                        { { "cue", 's', false }, { "slot", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createFeed (args[0].getString(),
                                                                   args[1].getString(),
                                                                   id);

                            return fromEdit (edit, withId (args, 2, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "insert.create",
                        "Puts a media cue through a rack channel. In Phase 4 the claim is"
                        " bookkeeping: the pool is here and the plugins are Phase 9's.",
                        { { "cue", 's', false }, { "channel", 's', false },
                          { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 2 ? args[2].getString() : std::string {};

                            const auto edit = document.createInsert (args[0].getString(),
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
        registry.add ({ "list.persistent",
                        "Gives a list its persistent section - the cues that should be running"
                        " at all times, checked after every trigger. Asking twice answers with the"
                        " one it has.",
                        { { "list", 's', false }, { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 1 ? args[1].getString() : std::string {};
                            const auto edit = document.createPersistent (args[0].getString(), id);
                            return fromEdit (edit, withId (args, 1, edit.id));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "port.create",
                        "Declares a MIDI port the show can send on.",
                        { { "name", 's', false }, { "id", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args.size() > 1 ? args[1].getString() : std::string {};
                            const auto edit = document.createPort (args[0].getString(), id);

                            return fromEdit (edit, withId (args, 1, edit.id));
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
        /*  COPY AND PASTE (author, 2026-09-18). Copy is a read that leaves a
            fragment where the tree publishes it (`document/clipboard`); paste
            is the write, one transaction however many cues, its record
            carrying the names it drew so a replay draws none. The fragment
            travels between two windows on the operating system's clipboard,
            which is why it is text and why paste takes it as an argument
            rather than reading the engine's own copy. */
        registry.add ({ "document.copy",
                        "Copies cues, by id, into the clipboard the tree publishes as canonical XML.",
                        { { "ids", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            document.copyToClipboard (splitWords (args[0].getString()));
                            return Outcome::ok (args);
                        } });

        registry.add ({ "document.paste",
                        "Pastes a fragment's cues into a list or group at a member position, under"
                        " new ids; the record carries the ids drawn.",
                        { { "parent", 's', false }, { "index", 'i', false },
                          { "fragment", 's', false }, { "ids", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto ids = args.size() > 3 ? splitWords (args[3].getString())
                                                             : std::vector<std::string> {};

                            const auto edit = document.paste (args[0].getString(), args[1].getInt32(),
                                                              args[2].getString(), ids);

                            return fromEdit (edit, withId (args, 3, edit.id));
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

        //----------------------------------------------------------------------
        /*  UNDO IS A LOGGED COMMAND, and the alternative is a replay that
            diverges in silence.

            The rejected design was rewinding the tree from a client gesture with
            no record, and it fails for the reason every hook in this engine is a
            submitted command: a replay runs no gestures, only records, so a log
            of `cue.create`, `node.set`, `node.set` would replay into a document
            that still had the edits while the live session's did not - the same
            inputs, a different show, and `wfg replay` exiting 0 because the
            records were identical.

            WHAT STOPS THAT IS THE APPLIED ARGUMENTS. Replay compares one line
            against another and never compares the document or the stack, so the
            record carries the domain AND the name of the transaction that came
            off it: an undo that pops a differently named transaction than the
            recorded session popped writes a different line and fails on that
            record with both names on screen. It is the `go` pattern applied to
            a stack - log what was APPLIED, not what was asked.

            Which is also why the transaction name is a declared parameter and
            not only an output. A command whose own record fails its own arity
            check is a session that cannot reproduce itself; so the name is
            optional on the way in, ignored when it is supplied, and always
            written on the way out.

            THE LOCK IS ASKED IN THE HANDLER and not at a door, because undo
            knocks at none: it writes through JUCE's own actions, underneath the
            four predicates. Asked FIRST, before the domain word is read, for the
            reason `remove` gives about the identifier it has not looked up yet -
            a locked show refuses the corrected command as well, so of the two
            things that can be wrong with an `undo` tonight the lock is the one
            worth reading first. */
        registry.add ({ "undo",
                        "Takes back the last transaction on a domain's history.",
                        { { "domain", 's', true }, { "transaction", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto word = args.empty()
                                                ? std::string (undoDomainWord (UndoDomain::document))
                                                : args[0].getString();

                            const auto domain = undoDomainForWord (word);

                            if (! domain.has_value())
                                return Outcome::rejected (reason::badValue);

                            const auto undone = document.undo (*domain);

                            if (! undone.has_value())
                                return Outcome::rejected (reason::nothingToUndo);

                            return Outcome::ok (
                                { osc::Value::string (std::string (undoDomainWord (*domain))),
                                  osc::Value::string (*undone) });
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "redo",
                        "Puts back the last transaction taken off a domain's history.",
                        { { "domain", 's', true }, { "transaction", 's', true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto word = args.empty()
                                                ? std::string (undoDomainWord (UndoDomain::document))
                                                : args[0].getString();

                            const auto domain = undoDomainForWord (word);

                            if (! domain.has_value())
                                return Outcome::rejected (reason::badValue);

                            const auto redone = document.redo (*domain);

                            if (! redone.has_value())
                                return Outcome::rejected (reason::nothingToRedo);

                            return Outcome::ok (
                                { osc::Value::string (std::string (undoDomainWord (*domain))),
                                  osc::Value::string (*redone) });
                        } });
    }
}
