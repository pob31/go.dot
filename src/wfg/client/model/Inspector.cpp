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
#include <wfg/client/model/Dual.h>
#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/DirectOuts.h>
#include <wfg/client/model/InputList.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Picture.h>
#include <wfg/client/model/Rack.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Video.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <utility>
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
        const std::vector<std::string> samplerRows { "strip", "initialLevel", "release", "secondPress",
                                                     "velocity", "velocityFloor", "pressure",
                                                     "releaseFade" };

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
                    (Phase 6): the DCA it answers to, the strip it is played
                    from (2026-09-25), where its fader waits, then what a hand
                    on its strip does, in the order a press happens - it is let
                    go, it is pressed again, it was struck, it is leant on, it
                    fades. The speed and its mode sit after where the file
                    starts: two more things said about how the file is played
                    (namespace draft §22.7). */
                { "media",   { "file", "lockedTo", "channels", "directOut",
                               "level", "startOffset", "rate", "rateMode", "dca", "dcaCurve", "dcaOffset", "strip",
                               "initialLevel", "release",
                               "secondPress", "velocity", "velocityFloor", "pressure",
                               "releaseFade" } },

                /*  A MIC CUE (Phase 9b): what it takes and through what, how it
                    comes in, then where it goes - a media cue's sound rows with
                    the input and the channel where the file was. */
                { "mic",     { "input", "channel", "fadeIn", "directOut",
                               "level", "dca", "dcaCurve", "dcaOffset" } },

                /*  A VIDEO CUE (Phase 8a): what it shows, where, how high in
                    the stack, how solid and in what colour, then how it comes
                    in - and last, as a sound's, what a hand on its strip does
                    (namespace draft §49). */
                { "video",   { "source", "canvas", "videoInput", "file", "fit", "layer", "blend", "opacity", "dca",
                               "dcaCurve", "dcaOffset",
                               "videoInsert", "paint", "fadeIn",
                               "startOffset", "rate",
                               "scale", "offsetX", "offsetY", "rotation", "flipH", "flipV",
                               "contrast", "saturation", "gamma", "hue",
                               "curveLuma", "curveRed", "curveGreen", "curveBlue",
                               "shape", "feather", "invert",
                               "strip", "initialLevel", "release", "secondPress", "velocity", "velocityFloor",
                               "pressure", "releaseFade" } },

                /*  What it moves - a cue, or a DCA instead - then where to and
                    how. Each thing a fade can move is a switch and then where it
                    goes, level then speed (namespace draft §22.7); the curve is
                    both's shape, so it comes after both. */
                { "fade",    { "target", "dca", "levelOn", "level", "rateOn", "rate", "curve", "points", "video",
                               "stopWhenDone" } },
                { "transport", { "target", "verb", "andGo", "range", "curve" } },
                { "start",   { "target" } },
                //  A process cue (namespace draft §51): its patch, then its MIDI.
                { "process", { "patch", "midiIn", "midiOut" } },
                /*  WHAT DOH! DOES WITH WHAT IT SENT last on both (PRD §3.32,
                    2026-10-01): a question about after the send, so it comes
                    after everything the send itself is. */
                { "osc",     { "device", "address", "value", "wait", "timeout", "duration", "loop", "doh",
                               "dohRollback" } },
                { "midi",    { "port", "channel", "type", "data1", "data2", "sysex", "wait", "doh",
                               "dohRollback" } },

                /*  `takeover` BESIDE `mode`, because it is a question only a
                    sampler group is asked and the answer to `mode` is what
                    makes it one; the DCA the whole group answers to last. */
                { "group",   { "mode", "takeover", "advance", "selection", "play", "loops",
                               "seed", "dca", "dcaCurve", "dcaOffset" } },
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
                //  A process cue's two ports (namespace draft §51).
                { "midiIn", "MIDI in" },
                { "midiOut", "MIDI out" },
                { "stopWhenDone", "stop when done" },
                { "andGo", "and Go" },
                { "shortName", "short name" },
                { "secondPress", "second press" },
                { "velocityFloor", "velocity floor" },
                { "releaseFade", "release fade" },
                { "initialLevel", "initial level" },
                /*  NOT "strip": a movie's strip is the row of its pictures at the
                    foot (§47, AAC), and a sampler member's is the fader or the
                    pad it is played from (namespace draft §49, ABI). */
                { "strip", "fader or pad" },
                { "rate", "speed" },
                { "rateMode", "speed mode" },
                { "levelOn", "moves level" },
                { "rateOn", "moves speed" },
                /*  NOT "colour", which is the cue's tint in the list and is on
                    the same panel (namespace draft 35, VG). */
                { "paint", "colour on canvas" },
                /*  A FADE ON A PICTURE (§36, VS): the values it moves, as pairs. */
                { "video", "moves picture" },
                { "offsetX", "offset right" },
                { "offsetY", "offset up" },
                { "flipH", "flip horizontally" },
                { "flipV", "flip vertically" },
                { "curveLuma", "luminosity curve" },
                { "curveRed", "red curve" },
                { "curveGreen", "green curve" },
                { "curveBlue", "blue curve" },
                { "doh", "on Doh!" },
                /*  A MOVIE'S SOUND (namespace draft 37.5, WJ). */
                { "lockedTo", "locked to movie" },
                /*  A CAPTURE'S INPUT AND A CUE'S INSERT (namespace draft §44):
                    the author's words, the rows' names kept apart from a
                    sound's `input` and `insert`. */
                { "videoInput", "input" },
                { "videoInsert", "insert" },
                { "dohRollback", "rollback" },
                /*  WHAT THE CUE'S DCA MARK CARRIES (namespace draft §50, ABT):
                    what the knob above its DCA's strip turns. */
                { "dcaCurve", "picture curve" },
                { "dcaOffset", "sound offset" },
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

            /*  A LIST ROW READ WHOLE (namespace draft §47): `text` answers empty
                for one - four numbers have no single text - and a box showing
                nothing for a curve that has points was a box that lied. Its
                values in words, as it is typed. */
            if (node.values.size() > 1)
            {
                field.value.clear();

                for (const auto& value : node.values)
                    field.value += (field.value.empty() ? "" : " ") + text (value);
            }

            /*  A DECIDED NUMBER WITHOUT ITS LONG TAIL (namespace draft §47,
                AAB): what a dial turn, a fade or a sum left as 0.30000000000000004
                reads "0.3". The box still writes what is typed into it. */
            if (node.typeTags == "d")
                if (const auto sole = node.soleValue(); sole.has_value() && sole->type() == osc::Value::Type::float64)
                    field.value = shownNumber (sole->getFloat64(), node.unit);

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
            else if (name == "notes" || name == "patch") field.control = Control::longText;
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

        /*  THE MEDIA ROW THAT DEPENDS ON THE SHOW RATHER THAN ON THE TABLE,
            decided after the scan because it needs a fact the scan has to have
            finished gathering: which outputs the rig declares. */
        void fitToTheRig (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                          std::vector<Field>& decided)
        {
            for (auto& field : decided)
            {
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
            rather than hidden (the inspector's rule throughout): an
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

        /*  WHAT A FADE'S TWO SWITCHES LEAVE ALONE (namespace draft §22.7):
            the level and its drawn curve while `levelOn` is off, the speed
            while `rateOn` is. Greyed and never hidden - the inspector's
            rule - so turning a switch on finds its row where it already was.
            The curve is the shape of both, and greyed only with neither on. */
        void greyWhatAFadeLeavesAlone (std::vector<Field>& decided)
        {
            auto levelOn = true;
            auto rateOn = false;

            for (const auto& field : decided)
            {
                if (field.name == "levelOn")
                    levelOn = field.value != "false";
                else if (field.name == "rateOn")
                    rateOn = field.value == "true";
            }

            for (auto& field : decided)
            {
                if (field.name == "level" || field.name == "points")
                    field.applies = levelOn;
                else if (field.name == "rate")
                    field.applies = rateOn;
                else if (field.name == "curve")
                    field.applies = levelOn || rateOn;
            }
        }

        /*  WHAT A TRANSPORT CUE'S VERB DOES, in words, and WHAT IT LEAVES ALONE
            greyed (2026-10-05, namespace draft §27): the slice only for
            advance, the curve and the length only for a fade, "and Go" only
            for a jump. Greyed and never hidden, the fade's rule, so changing
            the verb finds each row where it was. The stored words are the
            tree's; a verb added later keeps its own word in the menu. */
        void wordTheVerb (std::vector<Field>& decided)
        {
            static const std::map<std::string, std::string> words
            {
                { "hard", "stop at once" },
                { "fade", "fade out and stop" },
                { "afterMember", "stop after this member" },
                { "afterIteration", "stop after this round" },
                { "advance", "advance to the next slice" },
                { "record", "Rec on the take" },
                { "loop", "Loop on the take" },
                { "overdub", "Overdub on the take" },
                { "clear", "Clear the take" },
                { "enable", "enable the target for this run" },
                { "disable", "disable the target for this run" },
                { "jump", "jump standby to the target" },
            };

            std::string verb = "hard";

            for (auto& field : decided)
            {
                if (field.name != "verb")
                    continue;

                verb = field.value.empty() ? std::string ("hard") : field.value;

                if (field.writable && ! field.options.empty())
                {
                    field.choices.clear();

                    for (const auto& option : field.options)
                    {
                        const auto found = words.find (option);
                        field.choices.emplace_back (option, found != words.end() ? found->second : option);
                    }
                }
            }

            for (auto& field : decided)
            {
                if (field.name == "range")
                    field.applies = verb == "advance";
                else if (field.name == "curve" || field.name == "duration")
                    field.applies = verb == "fade";
                else if (field.name == "andGo")
                    field.applies = verb == "jump";
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

            /*  AND A PROCESS CUE'S TWO (namespace draft §51, PC.3): where its
                patch hears MIDI and where it sends it. */
            for (auto& field : decided)
            {
                if ((field.name != "port" && field.name != "midiIn" && field.name != "midiOut") || ! field.writable)
                    continue;

                field.control = Control::portRef;
                field.choices = portChoices (ports);
            }
        }

        /** The cue a row's address is under: `/godot/cue/<id>/<row>`. */
        std::string cueIdOf (const std::string& address)
        {
            const std::string head = "/godot/cue/";

            if (address.rfind (head, 0) != 0)
                return {};

            const auto end = address.find ('/', head.size());
            return address.substr (head.size(), end == std::string::npos ? std::string::npos : end - head.size());
        }

        /*  A CUE'S OWN MESSAGE as the rollback field is typed - the engine's
            `cue::spellMessageOf`, read through the door. */
        std::string spellMessage (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            const auto base = "/godot/cue/" + cueId + "/";
            const auto kind = text (snapshot, base + "kind");

            if (kind == "osc")
                return text (snapshot, base + "address") + " " + text (snapshot, base + "value");

            if (kind != "midi")
                return {};

            const auto or_ = [&snapshot, &base] (const char* name, const char* fallback)
            {
                const auto value = text (snapshot, base + name);
                return value.empty() ? std::string (fallback) : value;
            };

            const auto type = or_ ("type", "noteOn");

            if (type == "sysex")
                return "sysex " + text (snapshot, base + "sysex");

            const auto channel = or_ ("channel", "1");

            if (type == "programChange")
                return type + " " + channel + " " + or_ ("data1", "0");

            if (type == "channelPressure" || type == "pitchBend")
                return type + " " + channel + " " + or_ ("data2", "0");

            return type + " " + channel + " " + or_ ("data1", "0") + " " + or_ ("data2", "0");
        }

        /*  WHAT DOH! DOES WITH WHAT THIS CUE SENT (PRD §3.32; the author,
            2026-10-01: "a default per device that can be overriden at cue
            level"), in words. The row stays the enum the node declares -
            `device`, `takeBack`, `leave`, written as they are - and the menu
            says what each means, the first naming what the cue's device says
            NOW: an OSC cue's by its address, through `deviceOf` - the engine's
            own resolver, longest prefix and the smallest identifier on a tie -
            and a MIDI cue's by its port. Read as the engine reads it: only the
            exact word takes back, and no device leaves. The words are the
            author's (2026-10-02): "Undo(h)" for `takeBack` and "Meh" for
            `leave` - the stored words do not change. */
        void wordTheDoh (const tree::TreeSnapshot& snapshot, const std::string& kind,
                         std::vector<Field>& decided)
        {
            const auto valueOf = [&decided] (const char* name)
            {
                for (const auto& field : decided)
                    if (field.name == name)
                        return field.value;

                return std::string {};
            };

            for (auto& field : decided)
            {
                if (field.name != "doh" || ! field.writable)
                    continue;

                auto deviceTakesBack = false;

                if (kind == "osc")
                {
                    const auto devices = readDevices (snapshot);
                    const auto deviceId = deviceOf (valueOf ("address"), devices);

                    for (const auto& row : devices)
                        if (row.id == deviceId)
                            deviceTakesBack = row.doh == "takeBack";
                }
                else
                {
                    const auto portId = valueOf ("port");

                    for (const auto& row : readPorts (snapshot))
                        if (row.id == portId)
                            deviceTakesBack = row.doh == "takeBack";
                }

                field.choices = { { "device", deviceTakesBack ? "as the device (Undo(h))"
                                                               : "as the device (Meh)" },
                                  { "takeBack", "Undo(h)" },
                                  { "leave", "Meh" } };
                break;
            }

            /*  AND THE ROLLBACK UNDER IT (2026-10-03, OV-OX): live under Undo(h)
                only - Meh never reads it, so it is greyed there, not hidden -
                and an empty box shows what the Doh would send: the device's
                general go-back command, else the previous command. */
            const auto own = valueOf ("doh");
            auto deviceId = std::string {};
            auto general = std::string {};
            auto deviceTakesBack = false;

            if (kind == "osc")
            {
                const auto devices = readDevices (snapshot);
                deviceId = deviceOf (valueOf ("address"), devices);

                for (const auto& row : devices)
                    if (row.id == deviceId)
                    {
                        deviceTakesBack = row.doh == "takeBack";
                        general = row.dohRollback;
                    }
            }
            else
            {
                deviceId = valueOf ("port");

                for (const auto& row : readPorts (snapshot))
                    if (row.id == deviceId)
                    {
                        deviceTakesBack = row.doh == "takeBack";
                        general = row.dohRollback;
                    }
            }

            const auto takesBack = own == "takeBack" || (own != "leave" && deviceTakesBack);

            for (auto& field : decided)
            {
                if (field.name != "dohRollback")
                    continue;

                field.applies = takesBack;

                if (field.value.empty() && ! field.mixed)
                    field.placeholder = ! general.empty()
                                          ? general
                                          : previousCommand (snapshot, kind, deviceId, cueIdOf (field.address));
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

        /*  WHICH STRIP A SAMPLER MEMBER IS PLAYED FROM, as a menu of the
            show's faders and pads that says what each one carries - another
            member of this group, what the list put there before, or free
            (author, 2026-09-25). The words are `stripChoices`'; the rows they
            come from are the engine's. */
        void offerTheStrips (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                             std::vector<Field>& decided)
        {
            for (auto& field : decided)
            {
                if (field.name != "strip" || ! field.writable)
                    continue;

                field.control = Control::stripRef;
                field.choices = stripChoices (snapshot, cueId);
                return;
            }
        }

        /*  WHAT A MIC CUE TAKES AND WHAT IT PLAYS THROUGH (Phase 9b), as two
            menus of what the show declares - its named inputs and its rack
            channels - rather than eight characters typed from memory. The rows
            store identifiers and a person reads names, as for a DCA: renaming
            an input must leave every mic cue on it where it was. Each item
            says the fact the other menu has to agree with - an input's width,
            a channel's class - since a mono channel cannot take a stereo line
            and the run would fail saying so. Always menus, even before the
            show has any, when "(none)" alone says what is true. */
        /*  WHICH CANVAS A VIDEO CUE IS LAID ONTO (Phase 8a), as a menu of the
            show's canvases with their sizes, for the DCA's reason: the row
            stores the identifier and a person reads the name. Always a menu,
            "(none)" alone before the show has a canvas. */
        void offerTheCanvases (const tree::TreeSnapshot& snapshot, std::vector<Field>& decided)
        {
            for (auto& field : decided)
            {
                if (! field.writable)
                    continue;

                if (field.name == "canvas")
                {
                    field.control = Control::canvasRef;
                    field.choices = canvasChoices (readCanvases (snapshot));
                }
                else if (field.name == "videoInput")
                {
                    //  A CAPTURE'S INPUT (§44, YC), from the show's video inputs.
                    field.control = Control::videoInputRef;
                    field.choices = videoInputChoices (readVideoInputs (snapshot));
                }
                else if (field.name == "videoInsert")
                {
                    //  AND THE INSERT ITS PICTURE GOES THROUGH (§44, YE).
                    field.control = Control::videoInsertRef;
                    field.choices = videoInsertChoices (readVideoInserts (snapshot));
                }
            }
        }

        /*  WHICH MOVIE A SOUND IS LOCKED TO (namespace draft 37.5, WJ), as a
            menu of the show's movies, "not locked" first; and while it is,
            what the movie leads - its start offset and speed - drawn but not
            typed into, since the engine would refuse it as locked-to-movie. */
        void offerTheMovies (const tree::TreeSnapshot& snapshot, std::vector<Field>& decided)
        {
            Field* lock = nullptr;

            for (auto& field : decided)
                if (field.name == "lockedTo")
                    lock = &field;

            if (lock == nullptr)
                return;

            if (lock->writable)
            {
                lock->control = Control::movieRef;
                lock->choices = { { "", "not locked" } };

                for (const auto* node : snapshot.all())
                {
                    constexpr std::string_view prefix = "/godot/cue/";
                    constexpr std::string_view suffix = "/source";
                    const std::string_view address = node->address;

                    if (address.size() <= prefix.size() + suffix.size() || address.substr (0, prefix.size()) != prefix
                          || address.substr (address.size() - suffix.size()) != suffix || text (node) != "movie")
                        continue;

                    const auto id = std::string (address.substr (prefix.size(), address.size() - prefix.size() - suffix.size()));
                    const auto number = text (snapshot, "/godot/cue/" + id + "/number");
                    const auto name = shownCueName (snapshot, id);
                    lock->choices.push_back ({ id, (number.empty() ? std::string {} : number + "  ") + (name.empty() ? id : name) });
                }
            }

            if (lock->value.empty() || lock->mixed)
                return;

            for (auto& field : decided)
                if (field.name == "startOffset" || field.name == "rate")
                {
                    field.writable = false;
                    field.description = "Locked to its movie, which leads: " + field.description;
                }
        }

        void offerTheInputsAndChannels (const tree::TreeSnapshot& snapshot, std::vector<Field>& decided)
        {
            for (auto& field : decided)
            {
                if (! field.writable)
                    continue;

                if (field.name == "input")
                {
                    field.control = Control::inputRef;
                    field.choices = { { "", "(none)" } };

                    for (const auto& input : readInputs (snapshot))
                        field.choices.push_back ({ input.id, input.name + " \xc2\xb7 "
                                                                + input.widthWord()
                                                                + (input.width > 1 ? ", inputs " : ", input ")
                                                                + input.channelWord() });
                }
                else if (field.name == "channel")
                {
                    field.control = Control::channelRef;
                    field.choices = { { "", "(none)" } };

                    for (const auto& channel : readRack (snapshot).channels)
                        field.choices.push_back ({ channel.id, channel.name + " \xc2\xb7 "
                                                                  + channel.classWord() });
                }
            }
        }

        /*  A GROUP AS A MENU READS IT: number and name, as its row does, else
            whichever of the two it has, else its identifier. */
        std::string groupLabel (const tree::TreeSnapshot& snapshot, const std::string& groupId)
        {
            const auto base = "/godot/cue/" + groupId + "/";
            const auto number = text (snapshot, base + "number");
            const auto name = text (snapshot, base + "name");

            if (! number.empty() && ! name.empty())
                return number + " " + name;

            if (! name.empty())
                return name;

            return number.empty() ? groupId : number;
        }

        /*  WHICH GROUP'S HEADER GETS THIS CUE READY, as a menu of the groups
            around it rather than a box wanting an identifier typed from memory
            (namespace draft §30, decision QZ - the author: "I could not see a
            header/preset toggle in the cues").

            THE PARENTS, WALKED UP AS THE TREE NAMES THEM: a header's or a
            footer's cue has its group as parent too, and the engine prepares a
            preset wherever under the group it sits, so they are offered the
            same groups a member is. Innermost first, the order the ctrl/⌘-arrows
            step them in, and bounded as the new-cue lists' walk is.

            A VALUE NAMING NO GROUP AROUND IT is kept as an item of its own and
            said to be one - a mark the engine ignores with a warning, which
            the menu would otherwise show as nothing picked, a different and
            wrong sentence. */
        void offerTheGroupsAround (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                                   std::vector<Field>& decided)
        {
            for (auto& field : decided)
            {
                if (field.name != "preset" || ! field.writable)
                    continue;

                field.control = Control::groupRef;
                field.choices = { { "", "not prepared ahead" } };

                auto at = text (snapshot, "/godot/cue/" + cueId + "/parent");

                for (int depth = 0; depth < 64 && ! at.empty(); ++depth)
                {
                    if (text (snapshot, "/godot/cue/" + at + "/kind") != "group")
                        break;

                    field.choices.push_back ({ at, groupLabel (snapshot, at) });
                    at = text (snapshot, "/godot/cue/" + at + "/parent");
                }

                const auto& value = field.value;
                const auto known = std::any_of (field.choices.begin(), field.choices.end(),
                                                [&value] (const auto& choice) { return choice.first == value; });

                if (! known)
                    field.choices.push_back ({ value, groupLabel (snapshot, value) + " (not a group it is in)" });

                return;
            }
        }

        /*  WHAT ONLY A HAND ON A STRIP ASKS, greyed where no hand can reach it
            (PRD §3.27). A media cue carries the sampler rows whatever group it
            is in, and they mean something only on a MEMBER of a SAMPLER group:
            anywhere else nothing presses it. Greyed and never hidden - the
            inspector's rule - so a designer who moves a cue into a sampler
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

            //  A sound's and a picture's alike (namespace draft §49).
            if (kind != "media" && kind != "video")
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

        /*  WHAT A DCA MARK CARRIES MEANS SOMETHING ONLY WITH A DCA (namespace
            draft §50, ABT): both rows greyed on a cue marked with none. The
            picture's curve has nothing to shape under a sound or a mic; the
            sound's offset nothing to move under a picture with no sound - a
            movie's sound plays under its movie's run, so a movie keeps it, as a
            group does for whatever it holds. */
        void greyTheMapping (const std::string& kind, std::vector<Field>& decided)
        {
            std::string dca, source;

            for (const auto& field : decided)
            {
                if (field.name == "dca")
                    dca = field.value;
                else if (field.name == "source")
                    source = field.value;
            }

            for (auto& field : decided)
            {
                if (field.name == "dcaCurve")
                    field.applies = ! dca.empty() && (kind == "video" || kind == "group");
                else if (field.name == "dcaOffset")
                    field.applies = ! dca.empty() && (kind != "video" || source == "movie");
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

                /*  AND EVERY FURTHER MESSAGE WITH IT (namespace draft 45, YV): a
                    cue talks to one device, so a choice rewrites each message's
                    address as it rewrites the cue's - built in the order
                    `targetChoices` builds its keys, none first. */
                const auto cueBase = field.address.substr (0, field.address.size() - std::string ("address").size());
                std::vector<std::pair<std::string, std::string>> messages;

                for (const auto& id : words (text (snapshot, cueBase + "messages")))
                {
                    const auto row = "/godot/message/" + id + "/address";
                    messages.push_back ({ row, text (snapshot, row) });
                }

                if (! messages.empty())
                {
                    std::vector<std::string> deviceIds { std::string {} };

                    for (const auto& row : devices)
                        if (! row.prefixes().empty())
                            deviceIds.push_back (row.id);

                    for (const auto& deviceId : deviceIds)
                    {
                        auto& writes = aim.alongside[retarget (field.value, devices, deviceId)];

                        for (const auto& [row, address] : messages)
                            writes.push_back ({ row, retarget (address, devices, deviceId) });
                    }
                }

                decided.push_back (std::move (aim));
                return;
            }
        }
    }

    std::vector<Field> openersFor (const std::string& kind, const std::string& cueId,
                                   const std::string& source)
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

        /*  THE SEND LEVELS TOO, since they joined the bar at the head of the
            panel (2026-09-30): one bar holding every panel a cue has, in the
            order its sound goes - the file, its EQ, its plugins, where it is
            sent. The button beside the direct out stays where the author asked
            for it (2026-09-22); two doors to one room is not a trap. */
        if (kind == "media")
        {
            offer ("Waveform, in and out points", "waveform");
            offer ("EQ, four bands and two filters", "eq");
            offer ("FX, the signal chain on this cue", "fx");
            offer ("Sends, levels into the show's mix channels", "sends");
        }
        else if (kind == "video")
        {
            /*  A MOVIE'S STRIP (namespace draft §47, AAC): its length across the
                foot, its in and out points and loops dragged on it, as a sound's
                waveform is. The author, 2026-10-09: "Video files should display
                something similar to the waveform with a toggle at the top of the
                inspector ... I couldn't figure out how to do this." The panel
                had edited a movie's Ranges since 37.5; nothing offered it. A
                fill, a picture or a capture has no length to draw. */
            if (source == "movie")
                offer ("Strip, in and out points", "waveform");

            /*  AND ITS PICTURE (namespace draft §47, AAG): the author, "another
                foot panel with all colour and geometry adjustments rather
                listing them in the inspector". */
            offer ("Picture, place, colour and mask", "picture");
        }
        else if (kind == "mic")
        {
            /*  A LIVE INPUT HAS NO WAVEFORM: nothing is recorded to draw - but
                on a sampling channel it has a take (Phase 9c), and the panel
                says in its own notice when the channel records nothing. */
            offer ("EQ, four bands and two filters", "eq");
            offer ("FX, its channel's plugins on this cue", "fx");
            offer ("Sends, levels into the show's mix channels", "sends");
            offer ("Take, the loop its channel records", "take");
        }
        else if (kind == "fade")
        {
            /*  OFFERED EVEN THOUGH PICKING A FADE OPENS IT, because the row is
                also how it is SHUT: a panel that opened by itself and could
                only be closed from somewhere else would be a trap. */
            offer ("Mixer, what the fade moves and where to", "fade");
            offer ("EQ, the target's bands the fade moves", "eq");
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
        else if (kind == "osc")
        {
            /*  AN OSC CUE'S MESSAGES (namespace draft 45): every message it
                sends, every value of each, and the curves on them. */
            offer ("Messages, what this cue sends and the curves on it", "messages");
        }
        else if (kind == "process")
        {
            /*  A PROCESS CUE'S PATCH (namespace draft §51, PC.5): picking one
                opens it, as a fade's mixer opens - offered so it can be shut. */
            offer ("Patch, the boxes and lines this cue runs", "patch");
        }

        return out;
    }

    std::vector<Field> openersForMany (const std::vector<std::string>& kinds, const std::string& anchor)
    {
        /*  THE CUES THE TWO PANELS SERVE, media and mic, which both have an EQ
            and sends; a memo, a fade or a group in the selection is passed
            over, and the label says how many are left. */
        const auto served = static_cast<std::size_t> (std::count_if (kinds.begin(), kinds.end(),
                                                                     [] (const std::string& kind)
                                                                     { return kind == "media" || kind == "mic"; }));

        if (served == 0)
            return {};

        const auto onHowMany = served == kinds.size()
                                 ? ", on all " + std::to_string (served) + " cues at once"
                                 : ", on " + std::to_string (served) + " of the " + std::to_string (kinds.size())
                                     + " cues picked - " + (served == 1 ? "the media or mic cue" : "the media and mic cues");

        std::vector<Field> out;

        for (const auto& [label, subject] : { std::pair { "EQ, four bands and two filters", "eq" },
                                              std::pair { "Sends, levels into the show's mix channels", "sends" } })
        {
            Field field;
            field.name = subject;
            field.label = label + onHowMany;
            field.control = Control::opener;
            field.value = subject;
            field.address = anchor;
            out.push_back (std::move (field));
        }

        return out;
    }

    bool mayDial (const Field& field)
    {
        return field.writable && field.applies && ! field.boolean
                 && field.options.empty() && field.choices.empty()
                 && (field.typeTags == "d" || field.typeTags == "i")
                 && (field.control == Control::text || field.control == Control::loopCount);
    }

    std::string dialLine (const tree::TreeSnapshot& snapshot)
    {
        const auto address = text (snapshot, "/godot/surface/dial");

        if (address.empty())
            return {};

        //  /godot/<owner>/<id>/<row>
        std::vector<std::string> parts;
        std::string::size_type from = 1;

        while (from <= address.size())
        {
            const auto slash = address.find ('/', from);
            parts.push_back (address.substr (from, slash == std::string::npos ? std::string::npos
                                                                              : slash - from));
            if (slash == std::string::npos)
                break;

            from = slash + 1;
        }

        if (parts.size() != 4)
            return address;

        const auto& owner = parts[1];
        const auto& id = parts[2];
        const auto& row = parts[3];

        const auto nameOf = [&snapshot] (const std::string& kind, const std::string& objectId)
        {
            const auto name = text (snapshot, "/godot/" + kind + "/" + objectId + "/name");
            return name.empty() ? objectId : name;
        };

        /*  A SEND IS NAMED BY ITS CUE AND WHERE IT GOES, which is how the
            send mixer names it; anything else by its own name. */
        std::string who;

        if (owner == "send")
            who = nameOf ("cue", text (snapshot, "/godot/send/" + id + "/cue")) + " to "
                    + nameOf ("bus", text (snapshot, "/godot/send/" + id + "/bus"));
        else
            who = nameOf (owner, id);

        const auto found = labels().find (row);
        auto line = who + ": " + (found != labels().end() ? found->second : row);

        if (const auto* node = snapshot.find (address))
        {
            auto value = text (node);

            if (const auto sole = node->soleValue(); sole.has_value() && sole->type() == osc::Value::Type::float64)
                value = shownNumber (sole->getFloat64(), node->unit);

            if (! value.empty())
                line += " " + value + (node->unit.empty() ? std::string {} : " " + node->unit);
        }

        return line;
    }

    std::string previousCommand (const tree::TreeSnapshot& snapshot, const std::string& kind,
                                 const std::string& deviceId, const std::string& cueId)
    {
        if (deviceId.empty() || cueId.empty())
            return {};

        const auto devices = kind == "osc" ? readDevices (snapshot) : std::vector<DeviceRow> {};

        const auto routedHere = [&] (const std::string& id)
        {
            const auto base = "/godot/cue/" + id + "/";

            if (text (snapshot, base + "kind") != kind)
                return false;

            return (kind == "osc" ? deviceOf (text (snapshot, base + "address"), devices)
                                  : text (snapshot, base + "port")) == deviceId;
        };

        /*  IN PLAY ORDER, as the engine walks the document: a group's header,
            its members, its footer; the last match before the cue wins. */
        std::string found;
        auto reached = false;

        std::function<void (const std::string&, bool)> walk = [&] (const std::string& container, bool isList)
        {
            const auto base = (isList ? "/godot/list/" : "/godot/cue/") + container + "/";

            for (const auto* order : { "headerOrder", "order", "footerOrder" })
            {
                if (isList && std::string (order) != "order")
                    continue;

                for (const auto& id : words (text (snapshot, base + order)))
                {
                    if (reached)
                        return;

                    if (id == cueId)
                    {
                        reached = true;
                        return;
                    }

                    if (routedHere (id))
                        found = spellMessage (snapshot, id);

                    if (text (snapshot, "/godot/cue/" + id + "/kind") == "group")
                        walk (id, false);
                }
            }
        };

        for (const auto& listId : words (text (snapshot, "/godot/list/order")))
        {
            found.clear();
            walk (listId, true);

            if (reached)
                return found;
        }

        return {};
    }

    Inspection inspect (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        Inspection out;

        if (cueId.empty())
            return out;

        out.cueId = cueId;
        out.cueName = shownCueName (snapshot, cueId);
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
        {
            fitToTheRig (snapshot, cueId, decided);
            offerTheStrips (snapshot, cueId, decided);
        }

        //  A picture's strip, a menu as a sound's is (namespace draft §49).
        if (out.kind == "video")
            offerTheStrips (snapshot, cueId, decided);

        /*  A VIDEO CUE'S CANVAS is a menu of the show's (Phase 8a). */
        if (out.kind == "video")
            offerTheCanvases (snapshot, decided);

        /*  A MIC CUE'S OUTPUTS are a media cue's, and its source is two menus
            of its own (Phase 9b). */
        if (out.kind == "mic")
        {
            fitToTheRig (snapshot, cueId, decided);
            offerTheInputsAndChannels (snapshot, decided);
        }

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

        if (out.kind == "osc" || out.kind == "midi")
            wordTheDoh (snapshot, out.kind, decided);

        if (out.kind == "fade")
            greyWhatAFadeLeavesAlone (decided);

        if (out.kind == "transport")
            wordTheVerb (decided);

        /*  AN UNNAMED CUE'S NAME BOX shows, greyed, what the cue is called
            while it stays unnamed (namespace draft §53): a fade after its
            target, a sound after its file. Typing names it; emptying the box
            gives the automatic name back, and it follows the cue again. */
        for (auto& field : decided)
            if (field.name == "name" && field.value.empty() && ! field.mixed)
                field.placeholder = text (snapshot, "/godot/cue/" + cueId + "/autoName");

        /*  THE DCA A CUE ANSWERS TO, on the three kinds that carry the row -
            a media cue and a group marked with one, a fade that moves one - and
            asked of the rows rather than of the kind, so that whichever kind
            gains it next gets the menu with no line here. */
        aimAtADca (snapshot, decided);
        offerTheMovies (snapshot, decided);

        //  Which group's header prepares it, on every kind: the row is every cue's (§30, QZ).
        offerTheGroupsAround (snapshot, cueId, decided);

        greyWhatOnlyAHandAsks (snapshot, cueId, out.kind, decided);
        greyTheMapping (out.kind, decided);

        /*  THE EQ'S NINETEEN ROWS HAVE AN EDITOR OF THEIR OWN (Phase 9a), the
            panel at the foot, and are not listed here - by prefix, so a
            twentieth row joins the panel without a name in this file.
            They stay reachable: the opener below is the door. */
        if (out.kind == "media" || out.kind == "mic")
            std::erase_if (decided, [] (const Field& field)
                                    { return field.name.rfind ("eq", 0) == 0; });

        /*  AND THE LEVEL LANE IS DRAWN OVER THE WAVEFORM (namespace draft
            §20.5), where its points can be seen against the sound they ride;
            a list of numbers in a text box is not a way to edit a curve. The
            Waveform opener is the door, and the page keeps the row. */
        if (out.kind == "media")
            std::erase_if (decided, [] (const Field& field) { return field.name == "levelLane"; });

        /*  AND THE FOLD IS NOBODY'S TO ASK FOR ANY MORE (2026-10-08): a stereo
            cue onto a mono output is folded by the routing itself, so the
            switch had nothing left to say. Its attribute stays in the schema
            so a show that set it still opens. */
        if (out.kind == "media" || out.kind == "mic")
            std::erase_if (decided, [] (const Field& field) { return field.name == "stereoToMono"; });

        /*  AND A PICTURE'S PLACE, COLOUR AND MASK ARE ITS PANEL'S (namespace
            draft §47, AAG): a frame dragged on its canvas, curves drawn, a mask
            outlined - the inspector keeps what it is, where and how it comes
            in. */
        if (out.kind == "video")
            std::erase_if (decided, [] (const Field& field)
                                    { return std::find (pictureRows().begin(), pictureRows().end(), field.name)
                                               != pictureRows().end(); });

        /*  AND WHAT ELSE A FADE MOVES IS ITS MIXER'S (namespace draft §26, PG):
            a send, an EQ number or a plugin value is a strip, a box or a
            plugin's own knob with a tick box, and a list of pairs in a text box
            is not a way to edit any of them. The Mixer opener is the door. */
        if (out.kind == "fade")
            std::erase_if (decided, [] (const Field& field)
                                    { return field.name == "sends" || field.name == "eq" || field.name == "fx"; });

        //  The four blocks, in the order somebody fills them in.
        const auto kindRows = [&out]
        {
            const auto found = kindOrder().find (out.kind);
            return found != kindOrder().end() ? found->second : std::vector<std::string> {};
        }();

        Block isBlock { "what it is", {} }, whenBlock { "when", {} },
              doesBlock { "what it does", {} }, listBlock { "in the list", {} },
              samplerBlock { "sampler", {} };

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

        /*  A MEDIA CUE'S SAMPLER ROWS ARE A DRAWER OF THEIR OWN (author,
            2026-09-30: "we can also make more drawers for things"). Eight rows
            that only a hand on a strip asks, greyed on every cue that is not a
            sampler member - which is most of them - and they were the bottom
            half of what a media cue does. Moved whole and in the order they
            had, so a sampler member reads as it did, one heading further down;
            `dca` stays behind, since a DCA trims any cue (see `samplerRows`).
            A video cue's too, since a picture plays from a strip (§49). */
        if (out.kind == "media" || out.kind == "video")
        {
            std::stable_partition (doesBlock.fields.begin(), doesBlock.fields.end(),
                                   [] (const Field& field) { return ! named (samplerRows, field.name); });

            const auto firstSampler = std::find_if (doesBlock.fields.begin(), doesBlock.fields.end(),
                                                    [] (const Field& field)
                                                    { return named (samplerRows, field.name); });

            samplerBlock.fields.assign (std::make_move_iterator (firstSampler),
                                        std::make_move_iterator (doesBlock.fields.end()));
            doesBlock.fields.erase (firstSampler, doesBlock.fields.end());
        }

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
        /*  SINCE 2026-09-30 THEY ARE NOT AMONG THE ROWS AT ALL but in a bar of
            their own at the head of the panel (author: "the toggles for the
            foot panels in the inspector should be at the top to make opening
            the panel really quick"). At the end of what a cue does they were a
            scroll away on any cue with a long list of rows - a media cue in a
            sampler group most of all - and a door is quickest where the eye
            lands first. */
        out.panels = openersFor (out.kind, cueId, text (snapshot, "/godot/cue/" + cueId + "/source"));
        out.panelCue = cueId;

        for (auto* block : { &isBlock, &whenBlock, &doesBlock, &samplerBlock, &listBlock })
            if (! block->fields.empty())
                out.blocks.push_back (std::move (*block));

        sortInto (reported, {});
        out.details = std::move (reported);
        out.count = 1;

        return out;
    }

    Inspection inspectDual (const tree::TreeSnapshot& snapshot, const std::string& movie,
                            const std::string& sound, const std::string& picked)
    {
        auto out = inspect (snapshot, movie);
        const auto locked = inspect (snapshot, sound);

        if (out.empty() || locked.empty())
            return inspect (snapshot, picked);

        /*  THE SOUND'S DRAWER: its name, then what it does, as its own
            inspector orders them. Its start offset, speed and mode are the
            movie's, copied onto it in the same edit (37.5), and refused on
            the sound - offered once, on the picture. */
        Block drawer { "the sound", {} };

        const auto dropped = [] (const std::string& name)
        {
            return name == "startOffset" || name == "rate" || name == "rateMode";
        };

        for (const auto& block : locked.blocks)
            for (const auto& field : block.fields)
                if (field.name == "name")
                    drawer.fields.push_back (field);

        for (const auto& block : locked.blocks)
            if (block.heading == "what it does")
                for (const auto& field : block.fields)
                    if (! dropped (field.name))
                        drawer.fields.push_back (field);

        if (! drawer.fields.empty())
            out.blocks.push_back (std::move (drawer));

        /*  AND THE PANELS OF BOTH, the picture's first - its strip - then the
            sound's EQ, FX and sends, each opening on its own cue. The sound's
            waveform is the movie's strip, which draws it. */
        for (const auto& panel : locked.panels)
            if (panel.value != "waveform")
                out.panels.push_back (panel);

        out.cueId = picked;
        return out;
    }

    Inspection inspectMany (const tree::TreeSnapshot& snapshot, const std::vector<std::string>& cueIds,
                            const std::string& anchor)
    {
        if (cueIds.empty())
            return {};

        /*  EITHER LINE OF A MOVIE AND ITS SOUND, or both: the two as one
            (namespace draft §47, AAD). */
        if (cueIds.size() <= 2)
        {
            const auto dual = dualOf (snapshot, cueIds.front());
            const auto both = cueIds.size() == 1
                                || (dual.isPair() && (cueIds.back() == dual.movie || cueIds.back() == dual.sound));

            if (dual.isPair() && both)
                return inspectDual (snapshot, dual.movie, dual.sound,
                                    std::find (cueIds.begin(), cueIds.end(), anchor) != cueIds.end() ? anchor
                                                                                                      : cueIds.front());
        }

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

        /*  AND THE PANELS THAT ACT ON ALL OF THEM AT ONCE (namespace draft
            §30.11), opening on the anchor - the cue clicked last on purpose,
            whose values the panel draws. */
        out.panelCue = std::find (cueIds.begin(), cueIds.end(), anchor) != cueIds.end() ? anchor : cueIds.front();

        std::vector<std::string> eachKind;

        for (const auto& one : each)
            eachKind.push_back (one.kind);

        out.panels = openersForMany (eachKind, out.panelCue);

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

                /*  THE GROUPS AROUND EVERY ONE OF THEM, and no others (§30,
                    QZ). Each cue's menu is the groups it sits in, and the one
                    answer is written to all of them - so an item only some of
                    them sit under would mark the rest with a group no header
                    of theirs will ever prepare them in. "Not prepared ahead"
                    is around everything, so the menu is never empty. */
                if (field.control == Control::groupRef)
                    std::erase_if (field.choices, [&each, &findField, &first] (const auto& choice)
                    {
                        for (std::size_t index = 1; index < each.size(); ++index)
                        {
                            const auto* theirs = findField (each[index], first.name);

                            if (theirs == nullptr
                                  || std::none_of (theirs->choices.begin(), theirs->choices.end(),
                                                   [&choice] (const auto& their)
                                                   { return their.first == choice.first; }))
                                return true;
                        }

                        return false;
                    });

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
