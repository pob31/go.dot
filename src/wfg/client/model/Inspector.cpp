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

#include <wfg/client/model/Inspector.h>

#include <wfg/client/model/Devices.h>
#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/DirectOuts.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        /*  THE PAGE'S TABLES, TRANSCRIBED (views/inspector.js:100-116). Data,
            not code: reordering a kind is a line here rather than a change to
            anything, which is what made it cheap to argue about with the page
            open - and the same has to be true with the window open. */
        const std::vector<std::string> saidFirst { "number", "name", "shortName", "colour", "notes" };
        const std::vector<std::string> when      { "preWait", "duration", "postWait" };
        const std::vector<std::string> saidLast  { "enabled", "preset" };

        /*  WHAT ONLY A HAND ON A STRIP ASKS OF A MEDIA CUE (PRD §3.27): where
            its fader waits, what letting go does, what a second press does,
            how hard it was struck and pressed, and the fade a release ends in.
            `dca` arrived with them and is not one of them - a DCA trims any
            cue, fired or pressed. */
        const std::vector<std::string> samplerRows { "initialLevel", "release", "secondPress", "velocity",
                                                     "velocityFloor", "pressure", "releaseFade" };

        /*  WHAT A SAMPLER GROUP IS NEVER ASKED: how it advances, how a round is
            drawn and how many rounds it plays. The hand launches its members,
            in any order and any number of times, and the Runner spawns nothing
            for one (`beginPhase`), so these are a sequence's questions only. */
        const std::vector<std::string> roundRows { "advance", "selection", "play", "loops", "seed" };

        const std::map<std::string, std::vector<std::string>>& kindOrder()
        {
            static const std::map<std::string, std::vector<std::string>> table
            {
                /*  A SAMPLER MEMBER'S ROWS AFTER EVERYTHING A MEDIA CUE HAS
                    (Phase 6): the DCA it answers to, where its fader waits,
                    then what a hand on its strip does, in the order a press
                    happens - it is let go, it is pressed again, it was struck,
                    it is leant on, it fades. */
                { "media",   { "file", "channels", "stereoToMono", "directOut",
                               "level", "startOffset", "dca", "initialLevel", "release",
                               "secondPress", "velocity", "velocityFloor", "pressure",
                               "releaseFade" } },

                //  What it moves - a cue, or a DCA instead - then where to and how.
                { "fade",    { "target", "dca", "level", "curve", "points", "stopWhenDone" } },
                { "transport", { "target", "verb", "range", "curve" } },
                { "start",   { "target" } },
                { "osc",     { "device", "address", "value", "wait", "timeout" } },
                { "midi",    { "port", "channel", "type", "data1", "data2", "sysex", "wait" } },

                /*  `takeover` BESIDE `mode`, because it is a question only a
                    sampler group is asked and the answer to `mode` is what
                    makes it one; the DCA the whole group answers to last. */
                { "group",   { "mode", "takeover", "advance", "selection", "play", "loops",
                               "seed", "dca" } },
                { "range",   { "name", "in", "out", "loops" } },
                { "trigger", { "kind", "enabled", "address", "value", "port", "channel",
                               "type", "number", "data", "at" } },
            };

            return table;
        }

        /*  WHAT A ROW IS CALLED ON SCREEN when its own name is not what
            somebody reading it would call it. A short table: an entry is one
            the author asked for while using the panel, or two words the tree
            runs together in camelCase, which reads as code rather than as a
            question - and not a translation layer over the parameter table,
            which would go stale the day a row is added by somebody who never
            opens this file. A name that is not here keeps the tree's own word,
            so nothing can vanish by being forgotten. */
        const std::map<std::string, std::string>& labels()
        {
            static const std::map<std::string, std::string> table
            {
                { "play", "items to play" },
                { "stopWhenDone", "stop when done" },
                { "shortName", "short name" },
                { "secondPress", "second press" },
                { "velocityFloor", "velocity floor" },
                { "releaseFade", "release fade" },
                { "initialLevel", "initial level" },
            };

            return table;
        }

        bool named (const std::vector<std::string>& names, const std::string& name)
        {
            return std::find (names.begin(), names.end(), name) != names.end();
        }

        Field fieldFrom (const tree::Node& node, const std::string& name)
        {
            Field field;
            field.address = node.address;
            field.name = name;
            field.description = node.description;
            field.unit = node.unit;
            field.typeTags = node.typeTags;
            field.options = node.enumValues;
            field.hasMinimum = node.hasMinimum;
            field.hasMaximum = node.hasMaximum;
            field.minimum = node.minimum;
            field.maximum = node.maximum;

            /*  ASKED OF THE NODE AND NEVER OF A LIST OF NAMES: `ACCESS & write`
                is what makes a row a decision rather than a reading, so a row
                that becomes writable moves out of the fold by itself. */
            field.writable = (static_cast<int> (node.access)
                                & static_cast<int> (tree::Access::write)) != 0;

            field.boolean = node.typeTags == "T" || node.typeTags == "F";
            field.value = text (&node);

            if (const auto found = labels().find (name); found != labels().end())
                field.label = found->second;
            else
                field.label = name;

            /*  WHICH CONTROL ASKS THIS BEST. Decided from the node wherever the
                node can say - a `T` row is a switch, a closed set of values is
                a choice - and NAMED only twice, where no property of a row
                could have said it: `loops`, where one integer carries three
                questions and no single control can put them, and `file`, where
                the value is a name on a disk and a machine can be asked to go
                and find it. Both send the same `node.set` a typed answer would.
                A read-only row is never composed: there is nothing to ask. */
            if (! field.writable)          field.control = Control::text;
            else if (field.boolean)        field.control = Control::toggle;
            else if (! field.options.empty()) field.control = Control::choice;
            else if (name == "loops")      field.control = Control::loopCount;
            else if (name == "file")       field.control = Control::file;
            else if (name == "target")     field.control = Control::cueRef;
            else if (name == "notes")      field.control = Control::longText;
            else                           field.control = Control::text;

            return field;
        }

        void sortInto (std::vector<Field>& fields, const std::vector<std::string>& order)
        {
            /*  NAMED FIRST IN THE ORDER GIVEN, then everything else
                alphabetically: a row nobody has named turns up at the end of
                its own block rather than vanishing or leading. */
            std::stable_sort (fields.begin(), fields.end(),
                              [&order] (const Field& a, const Field& b)
                              {
                                  const auto at = [&order] (const std::string& name)
                                  {
                                      const auto found = std::find (order.begin(), order.end(), name);
                                      return found == order.end()
                                               ? order.size()
                                               : static_cast<std::size_t> (found - order.begin());
                                  };

                                  if (at (a.name) != at (b.name))
                                      return at (a.name) < at (b.name);

                                  return a.name < b.name;
                              });
        }

        /*  THE TWO MEDIA ROWS THAT DEPEND ON THE SHOW RATHER THAN ON THE
            TABLE, decided after the scan because both need facts the scan has
            to have finished gathering: how many channels the file has, and
            which outputs the rig declares. */
        void fitToTheRig (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                          std::vector<Field>& decided)
        {
            const auto channels = text (snapshot, "/godot/cue/" + cueId + "/channels");

            for (auto& field : decided)
            {
                if (field.name == "stereoToMono")
                {
                    /*  A fold is a statement about a two-channel file and the
                        engine ignores it otherwise, so the window says so
                        rather than offering a switch that does nothing. Greyed
                        and not hidden: an absence reads as "this program does
                        not do that", which is the wrong thing to say. */
                    field.applies = channels == "2";
                    continue;
                }

                if (field.name != "directOut")
                    continue;

                field.control = Control::busRef;

                /*  EMPTY IS A CHOICE AND NOT AN ABSENCE. No direct out is
                    what every cue is until somebody picks one, and it has to
                    be possible to go back to. */
                field.choices.push_back ({ "", "(none)" });

                /*  THE DIRECT OUTS ONLY. `bus/kind`'s own description says the
                    word decides which menu a bus appears in: a mix channel is
                    reached through a send, at a level, which is the whole
                    difference between the two.

                    AND EVERY ONE OF THEM, marked rather than filtered (PRD
                    3.9b, amended 2026-09-22): an output that is busy is
                    exactly the one somebody is deciding about, and leaving it
                    out would remove the decision instead of informing it. */
                const auto outputs = readOutputs (snapshot);
                const auto marks = readOutMarks (snapshot, cueId, outputs);

                std::size_t at = 0;

                for (const auto& row : outputs)
                {
                    if (row.kind != "direct")
                        continue;

                    const auto name = row.name.empty() ? row.id : row.name;

                    /*  The marks are the direct outs in the same order, so
                        they walk in step rather than being looked up. */
                    field.choices.push_back (
                        { row.id, at < marks.size()
                                    ? markedLabel (name, row.widthWord(), marks[at])
                                    : name + " · " + row.widthWord() });

                    ++at;
                }
            }
        }

        /*  WHAT THE TWO NUMBERS ON A MIDI CUE ARE FOR, which depends entirely
            on the type and which the panel used to leave somebody to know.

            The author, looking at it: *"There are two number fields in the
            MIDI cue."* They are `number` and `data`, and neither word means
            anything at the keyboard: for a note-on they are the NOTE and the
            VELOCITY, for a control change the CONTROLLER and its VALUE, for a
            program change the program and nothing at all. The row names are
            right - one row carries the payload whatever the payload is, which
            is what stops a bend needing a second place for a number to be
            wrong - and what was missing is the panel saying which is which.

            SO THE LABEL FOLLOWS THE TYPE, and what the type ignores is GREYED
            rather than hidden (the rule `stereoToMono` already follows): an
            absence reads as "this program cannot do that", which is the wrong
            thing to say about a field that would work perfectly well if the
            type above it were different.

            The table is `midi::build`'s own, read off it rather than guessed:
            pitch bend is fourteen bits in `data` and has no `number`, program
            change carries its program in `number` and has no `data`, channel
            pressure is the other way round, and sysex uses neither and not the
            channel either. */
        void nameTheNumbers (std::vector<Field>& decided)
        {
            std::string type = "noteOn";

            for (const auto& field : decided)
                if (field.name == "type" && ! field.value.empty())
                    type = field.value;

            const auto note = type == "noteOn" || type == "noteOff" || type == "aftertouch";
            const auto control = type == "controlChange";
            const auto program = type == "programChange";
            const auto bend = type == "pitchBend";
            const auto pressure = type == "channelPressure";
            const auto system = type == "sysex";

            for (auto& field : decided)
            {
                if (field.name == "data1")
                {
                    field.label = note ? "note" : control ? "controller"
                                : program ? "program" : "data1";
                    field.applies = note || control || program;
                }
                else if (field.name == "data2")
                {
                    field.label = note && type != "aftertouch" ? "velocity"
                                : type == "aftertouch" || pressure ? "pressure"
                                : control ? "value" : bend ? "bend" : "data2";
                    field.applies = ! program && ! system;
                }
                else if (field.name == "sysex")
                {
                    field.applies = system;
                }
                else if (field.name == "channel")
                {
                    field.applies = ! system;
                }
            }
        }

        /*  WHICH PORT A MIDI CUE SENDS ON, as a menu rather than as the eight
            characters of an identifier typed by hand.

            AN IDENTIFIER AND NOT A NAME, which is the row's own rule and the
            reason moving an interface is cheap: the cue names the port, the
            port names the cable, and only the second changes when somebody
            re-patches. So the menu's key is the identifier the row stores and
            its label is what a person reads. */
        void aimAtAPort (const tree::TreeSnapshot& snapshot, std::vector<Field>& decided)
        {
            const auto ports = readPorts (snapshot);

            if (ports.empty())
                return;

            for (auto& field : decided)
            {
                if (field.name != "port" || ! field.writable)
                    continue;

                field.control = Control::portRef;
                field.choices = portChoices (ports);
                return;
            }
        }

        /*  WHICH DCA A CUE ANSWERS TO, as a menu rather than as the eight
            characters of an identifier typed by hand: the port row's shape, and
            for its reason - the row stores the identifier, a person reads the
            name, and a DCA renamed must leave every cue marked with it marked.

            ALWAYS A MENU, even before the show has a DCA, when it offers
            "(none)" alone. Unlike the device line, the row is there anyway on
            every media cue, group and fade; the question is only how it is
            asked, and a box would take any eight characters typed from memory -
            a mark naming a DCA that is not there is a warning when the show is
            checked, not a refusal at the door - where a menu of one entry says
            what is true: there is nothing to assign it to yet.

            A SECOND SCAN, and only for a cue that has the row: `readDcas` walks
            the tree its own way, for the reason `fitToTheRig` reads the outputs
            through `readOutputs` rather than gathering them in the scan above. */
        void aimAtADca (const tree::TreeSnapshot& snapshot, std::vector<Field>& decided)
        {
            for (auto& field : decided)
            {
                if (field.name != "dca" || ! field.writable)
                    continue;

                field.control = Control::dcaRef;
                field.choices = dcaChoices (readDcas (snapshot));
                return;
            }
        }

        /*  WHAT ONLY A HAND ON A STRIP ASKS, greyed where no hand can reach it
            (PRD §3.27). A media cue carries the sampler rows whatever group it
            is in, and they mean something only on a MEMBER of a SAMPLER group:
            anywhere else nothing presses it. Greyed and never hidden - the
            `stereoToMono` rule - so a designer who moves a cue into a sampler
            group finds the rows where they already saw them.

            A HEADER'S CUES ARE NOT MEMBERS, though the tree names the group as
            their parent: a header is the group's preparation, fired when it
            starts, and no strip ever holds one. So the cue's `role` is asked as
            well as its parent's `mode`.

            AND TWO THAT DEPEND ON THE ROWS BESIDE THEM. A second press cannot
            reach a hold clip - the hand holding it is still down - so
            `secondPress` means nothing under `release = hold`. And the floor is
            the bottom of the velocity and pressure scale, so it means nothing
            with both of those off.

            A group's `takeover` is the same idea from the other side: what
            arming this group does to the sampler groups already armed, which is
            a question only a sampler group is ever asked. */
        void greyWhatOnlyAHandAsks (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                                    const std::string& kind, std::vector<Field>& decided)
        {
            const auto valueOf = [&decided] (const std::string& name)
            {
                for (const auto& field : decided)
                    if (field.name == name)
                        return field.value;

                return std::string {};
            };

            if (kind == "group")
            {
                const auto sampler = valueOf ("mode") == "sampler";

                for (auto& field : decided)
                {
                    if (field.name == "takeover")
                        field.applies = sampler;
                    else if (sampler && named (roundRows, field.name))
                        field.applies = false;
                }

                return;
            }

            if (kind != "media")
                return;

            /*  THE PARENT'S MODE, read where it lives. A cue at the top of a
                list has a parent with no `mode` row at all, which is not a
                sampler group and greys the rows as it should. */
            const auto base = "/godot/cue/" + cueId + "/";
            const auto parent = text (snapshot, base + "parent");
            const auto member = text (snapshot, base + "role") == "member";
            const auto pressable = member && ! parent.empty()
                                     && text (snapshot, "/godot/cue/" + parent + "/mode") == "sampler";

            const auto holds = valueOf ("release") == "hold";
            const auto scaled = valueOf ("velocity") == "true" || valueOf ("pressure") == "true";

            for (auto& field : decided)
            {
                if (! named (samplerRows, field.name))
                    continue;

                if (field.name == "secondPress")
                    field.applies = pressable && ! holds;
                else if (field.name == "velocityFloor")
                    field.applies = pressable && scaled;
                else
                    field.applies = pressable;
            }
        }

        /*  WHICH DEVICE A NETWORK CUE IS AIMED AT, as a line of its own above
            the address it is derived from.

            IT IS NOT A ROW. No attribute in the document says a cue's target,
            deliberately: the address carries its device's prefix, and a second
            field naming the device would be a second truth to keep in step
            with the first. So the menu reads the front of the address and
            writes the whole address back, which is why its `address` is the
            cue's `address` row and its choices are whole addresses.

            LABELLED "target" AND NAMED "device", because the row a person
            reads and the key the panel is built from are different questions:
            `target` is already the name of a cueRef row on three other kinds,
            and two rows of one name in one panel would collide in `shapeOf`.

            Only when the show HAS devices. A menu with one entry reading
            "(none)" tells nobody anything and takes a line from a panel that
            is short of them; a show with no device declared is one where the
            address box is the whole answer. */
        /*  NO CUE IDENTIFIER, unlike `fitToTheRig` beside it, and that is the
            shape of the thing rather than an oversight: this line is derived
            from a field the scan has already found, so everything it needs is
            in `decided`. GCC's -Werror=unused-parameter is what said so. */
        void aimAtADevice (const tree::TreeSnapshot& snapshot,
                           std::vector<Field>& decided)
        {
            const auto devices = readDevices (snapshot);

            if (devices.empty())
                return;

            for (const auto& field : decided)
                if (field.name == "device")
                    return;

            for (const auto& field : decided)
            {
                if (field.name != "address" || ! field.writable)
                    continue;

                Field aim;
                aim.address = field.address;
                aim.name = "device";
                aim.label = "target";
                aim.control = Control::deviceRef;
                aim.value = field.value;
                aim.typeTags = field.typeTags;
                aim.writable = true;
                aim.choices = targetChoices (field.value, devices);
                aim.description = "Which of the show's devices this cue writes to."
                                  " Choosing one rewrites the address below it, because"
                                  " the address is where a cue says where it is going.";

                decided.push_back (std::move (aim));
                return;
            }
        }
    }

    std::vector<Field> openersFor (const std::string& kind, const std::string& cueId)
    {
        std::vector<Field> out;

        const auto offer = [&out, &cueId] (const char* label, const char* subject)
        {
            Field field;
            field.name = subject;
            field.label = label;
            field.control = Control::opener;
            field.value = subject;          // the SUBJECT, in the words `Subject` uses
            field.address = cueId;          // what the panel would open ON
            out.push_back (std::move (field));
        };

        if (kind == "media")
        {
            offer ("Waveform, in and out points", "waveform");
        }
        else if (kind == "fade")
        {
            /*  OFFERED EVEN THOUGH PICKING A FADE OPENS IT, because the row is
                also how it is SHUT: a panel that opened by itself and could
                only be closed from somewhere else would be a trap. */
            offer ("Curve, the shape of the fade", "curve");
        }
        else if (kind == "group")
        {
            /*  ONLY A GROUP HAS MEMBERS TO ARRANGE, and the panel says in its
                own notice when the group is a sequence rather than a timeline -
                offered either way, because seeing the shape of a sequence is
                worth the look even where nothing can be dragged. */
            offer ("Timeline, members arranged by dragging", "timeline");
        }

        return out;
    }

    Inspection inspect (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        Inspection out;

        if (cueId.empty())
            return out;

        out.cueId = cueId;
        out.cueName = text (snapshot, "/godot/cue/" + cueId + "/name");
        out.kind = text (snapshot, "/godot/cue/" + cueId + "/kind");

        /*  ONE SCAN OF THE TREE, AND ONLY WHEN A SELECTION CHANGES. `all()` is
            linear and allocates, which is why the cue list is forbidden it -
            there it would run per row per pass. Here it runs when somebody
            clicks, which is a few thousand comparisons against a human
            reaction time, and the alternative is this client keeping its own
            copy of which attributes a kind has: exactly the table §14.2 says a
            generic inspector must not have.

            (`childrenOf` would do the same scan and is banned outright, so the
            gate stays honest: what is forbidden is the per-row habit, not the
            one-off.) */
        const auto prefix = "/godot/cue/" + cueId + "/";

        std::vector<Field> decided, reported;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (prefix, 0) != 0)
                continue;

            const auto name = node->address.substr (prefix.size());

            //  Only this cue's own rows: anything deeper belongs to something else.
            if (name.find ('/') != std::string::npos)
                continue;

            auto field = fieldFrom (*node, name);

            (field.writable ? decided : reported).push_back (std::move (field));
        }

        /*  WHAT THE SHOW'S OUTPUTS ARE, for the rows that point at one.

            A SECOND SCAN, AND ONLY FOR A MEDIA CUE. `readOutputs` walks the
            tree again rather than this loop gathering buses as it goes, and
            that is a trade taken deliberately: doing it by hand here would be
            a second copy of how a bus is read, and the two would drift the
            first time a column was added to that row. What it costs is one
            more linear pass when somebody clicks a media cue, against the same
            human reaction time the comment above weighs. */
        if (out.kind == "media")
            fitToTheRig (snapshot, cueId, decided);

        /*  And the same kind of second pass for a network cue, for the same
            reason: which devices exist is a fact about THIS show and cannot
            come from the parameter table. */
        if (out.kind == "osc")
            aimAtADevice (snapshot, decided);

        if (out.kind == "midi")
        {
            aimAtAPort (snapshot, decided);
            nameTheNumbers (decided);
        }

        /*  THE DCA A CUE ANSWERS TO, on the three kinds that carry the row -
            a media cue and a group marked with one, a fade that moves one - and
            asked of the rows rather than of the kind, so that whichever kind
            gains it next gets the menu with no line here. */
        aimAtADca (snapshot, decided);

        greyWhatOnlyAHandAsks (snapshot, cueId, out.kind, decided);

        //  The four blocks, in the order somebody fills them in.
        const auto kindRows = [&out]
        {
            const auto found = kindOrder().find (out.kind);
            return found != kindOrder().end() ? found->second : std::vector<std::string> {};
        }();

        Block isBlock { "what it is", {} }, whenBlock { "when", {} },
              doesBlock { "what it does", {} }, listBlock { "in the list", {} };

        for (auto& field : decided)
        {
            if (named (saidFirst, field.name))      isBlock.fields.push_back (std::move (field));
            else if (named (when, field.name))      whenBlock.fields.push_back (std::move (field));
            else if (named (saidLast, field.name))  listBlock.fields.push_back (std::move (field));
            else                                    doesBlock.fields.push_back (std::move (field));
        }

        sortInto (isBlock.fields, saidFirst);
        sortInto (whenBlock.fields, when);
        sortInto (doesBlock.fields, kindRows);
        sortInto (listBlock.fields, saidLast);

        /*  AND THE PANELS THIS CUE HAS, at the end of what it DOES and after
            the sorts, so an opener never lands in the middle of the rows a
            kind orders. The author asked for them here rather than in a menu
            (2026-09-21: *"the controls to show the waveform, the send levels,
            the EQ, the group timeline ... No hunting in the menus"*), and the
            reasoning is the ordinary one: the inspector is where a cue is
            being worked on, so a longer look at that same cue is a thing to
            ask for from there.

            They are NOT parameters and write nothing. A row here is usually a
            decision the document holds; these are doors. The window draws them
            differently for that reason - §4.8's rule applies to form as well
            as to colour. */
        for (auto& opener : openersFor (out.kind, cueId))
            doesBlock.fields.push_back (std::move (opener));

        for (auto* block : { &isBlock, &whenBlock, &doesBlock, &listBlock })
            if (! block->fields.empty())
                out.blocks.push_back (std::move (*block));

        sortInto (reported, {});
        out.details = std::move (reported);
        out.count = 1;

        return out;
    }

    Inspection inspectMany (const tree::TreeSnapshot& snapshot, const std::vector<std::string>& cueIds)
    {
        if (cueIds.empty())
            return {};

        if (cueIds.size() == 1)
            return inspect (snapshot, cueIds.front());

        /*  EACH ONE INSPECTED, THEN THE INTERSECTION. A row survives when every
            cue has it by name and it is writable everywhere; its value is the
            one they all give or `mixed`. The first cue's blocks give the order,
            so a selection of two fades reads like one fade with some boxes
            blank - which is honest, and the page's own rule. */
        std::vector<Inspection> each;

        for (const auto& id : cueIds)
            each.push_back (inspect (snapshot, id));

        Inspection out;
        out.count = cueIds.size();
        out.cueId = cueIds.front();

        for (std::size_t at = 1; at < cueIds.size(); ++at)
            out.cueId += " " + cueIds[at];

        out.cueName = std::to_string (cueIds.size()) + " cues";

        std::vector<std::string> kinds;

        for (const auto& one : each)
            if (! named (kinds, one.kind))
                kinds.push_back (one.kind);

        for (std::size_t at = 0; at < kinds.size(); ++at)
            out.kind += (at == 0 ? "" : " + ") + kinds[at];

        const auto findField = [] (const Inspection& in, const std::string& name) -> const Field*
        {
            for (const auto& block : in.blocks)
                for (const auto& field : block.fields)
                    if (field.name == name)
                        return &field;

            return nullptr;
        };

        for (const auto& block : each.front().blocks)
        {
            Block shared { block.heading, {} };

            for (const auto& first : block.fields)
            {
                /*  THE TARGET MENU IS NOT OFFERED OVER A SELECTION, and it is
                    the one line here that could not be.

                    Every other row writes the same text to N addresses, which
                    is what `addresses` is for. This one writes a REWRITE of
                    each cue's own address, and every cue has a different one -
                    so one value cannot be committed to all of them, and a menu
                    that quietly aimed six cues at one address would be the
                    worst kind of helpful. Aiming several cues at a device is
                    worth having and is a command that does not exist yet;
                    until it does, the honest drawing is no menu. */
                if (first.control == Control::deviceRef)
                    continue;

                auto field = first;
                field.addresses.clear();
                field.addresses.push_back (first.address);

                auto everywhere = true;

                for (std::size_t at = 1; at < each.size() && everywhere; ++at)
                {
                    const auto* other = findField (each[at], first.name);

                    if (other == nullptr || ! other->writable)
                    {
                        everywhere = false;
                        break;
                    }

                    field.addresses.push_back (other->address);

                    if (other->value != field.value)
                        field.mixed = true;

                    /*  GREYED ONLY WHERE IT MEANS NOTHING FOR EVERY ONE OF THEM.
                        A row that applies to one cue of the selection is a row
                        somebody may want to set, and the commit writes it to
                        all of them - where it means nothing, the engine ignores
                        it, exactly as it does for one cue. Taking the first
                        cue's answer, as this did, greyed a sampler row over a
                        selection that happened to start outside the group. */
                    field.applies = field.applies || other->applies;
                }

                if (! everywhere)
                    continue;

                if (field.mixed)
                    field.value.clear();

                shared.fields.push_back (std::move (field));
            }

            if (! shared.fields.empty())
                out.blocks.push_back (std::move (shared));
        }

        return out;
    }
}
