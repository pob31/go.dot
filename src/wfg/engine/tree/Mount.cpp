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

#include <wfg/engine/tree/Mount.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        /*  A member of an object, or nullptr. Nothing here throws and nothing
            guesses: a key that is not there is simply not there. */
        const json::Value* property (const json::Value& node, const char* key)
        {
            return node.find (key);
        }

        std::string stringProperty (const json::Value& node, const char* key)
        {
            const auto* value = property (node, key);
            return value != nullptr ? value->asString() : std::string {};
        }

        /*  A VALS or UNIT entry as text. They are strings in every description
            anybody writes, but a number is legal JSON there and reading one as
            an empty string would silently drop a legal member of a closed set. */
        std::string asText (const json::Value& value)
        {
            if (value.isString()) return value.asString();
            if (value.isNumber()) return osc::formatDouble (value.asNumber());
            if (value.isBool())   return value.asBool() ? "true" : "false";

            return {};
        }

        /*  THE ADDRESSES THE ENGINE ANSWERS ITSELF, one list rather than one
            hand-rolled comparison each.

            Both entries are HTTP routes on the same port as the tree, so a
            mount at either would be published and unreachable at once - visible
            in a tree dump, and answering something that is not a namespace to
            anybody who asked for it over HTTP. `/ui` is where the OSCQuery
            server serves the client; `/media` is where a timbre pyramid is
            served, content-addressed by hash, and a collision there would read
            as a cache miss rather than as the collision it is, which is the
            worst kind of failure to debug on a show day.

            A list because the second one proved the shape: two copies of the
            same three lines would have drifted the first time a third route was
            added, and the third one - whatever it turns out to be - is now a
            one-line change beside an argument rather than a check somebody has
            to remember to write. `/media` is reserved BEFORE the route exists,
            because a reservation is worth more before somebody's show file has
            already used the prefix than after. */
        struct Reserved
        {
            std::string_view prefix;   ///< the address itself, with no trailing slash
            std::string_view served;   ///< what answers there, for the operator's message
        };

        constexpr Reserved reservedPrefixes[] =
        {
            { "/ui",    "its client" },
            { "/media", "the timbre pyramids of the show's media" },

            /*  AND THE ENGINE'S OWN TREE (2026-09-22). It reads as though it
                were always covered - the comment below says a prefix of "/"
                would land "on top of /godot" - but nothing refused a mount at
                /godot itself, or under it, and a device published there would
                shadow the addresses every client and every command use.

                It was a theoretical hazard while a prefix could only be
                hand-written into show.xml by somebody who knew what it was.
                It stopped being theoretical when the settings window grew a
                box a person types one into. */
            { "/godot", "the engine's own parameters" },
        };

        /*  A prefix has to be an absolute OSC address with no trailing slash
            and no empty segment, because every mounted address is built by
            sticking it in front of one. A prefix of "/" would put somebody
            else's namespace at the root, on top of /godot. */
        bool onePrefixIsUsable (const std::string& prefix, std::string& why)
        {
            if (prefix.empty() || prefix.front() != '/')
            {
                why = "a mount prefix must start with '/'";
                return false;
            }

            /*  Refused at load, for the same reason "/" is: the file says
                something the engine cannot honour, and the show is read long
                before anybody presses GO. */
            for (const auto& reservation : reservedPrefixes)
            {
                const auto reservedSize = reservation.prefix.size();

                if (prefix.size() < reservedSize
                      || prefix.compare (0, reservedSize, reservation.prefix) != 0)
                    continue;

                /*  The reserved address itself, or a segment beneath it, and
                    nothing else. `/mediaserver` merely begins with the same
                    letters and is somebody's perfectly ordinary mount: refusing
                    it would be this check's own boundary bug rather than the
                    collision it is there to catch. */
                if (prefix.size() != reservedSize && prefix[reservedSize] != '/')
                    continue;

                why = "\"" + std::string (reservation.prefix) + "\" is where the engine serves "
                      + std::string (reservation.served) + ", so nothing can be mounted there";
                return false;
            }

            if (prefix == "/")
            {
                why = "a mount prefix of \"/\" would mount over the whole tree";
                return false;
            }

            if (prefix.back() == '/')
            {
                why = "a mount prefix must not end with '/'";
                return false;
            }

            if (prefix.find (' ') != std::string::npos)
            {
                why = "a mount prefix cannot contain a space";
                return false;
            }

            if (prefix.find ("//") != std::string::npos)
            {
                why = "a mount prefix must not contain an empty segment";
                return false;
            }

            return true;
        }

        /*  Every address the row names, each checked the same way, and the
            row itself refused when it names none - a device with no prefix
            answers nowhere and would take every cue aimed at nothing. */
        bool prefixesAreUsable (const MountDeclaration& mount, std::string& why)
        {
            const auto prefixes = prefixesOf (mount.prefix);

            if (prefixes.empty())
            {
                why = "a device has to say what address it answers at";
                return false;
            }

            for (const auto& prefix : prefixes)
                if (! onePrefixIsUsable (prefix, why))
                    return false;

            return true;
        }

        Access accessFrom (const json::Value& node, bool isContainer)
        {
            const auto* value = property (node, "ACCESS");

            /*  Absent means read-only rather than writable. This is somebody
                else's box: assuming we may write to a node it never said we
                could is the assumption that breaks a show, and refusing a write
                that was in fact allowed only costs a message. */
            if (value == nullptr)
                return isContainer ? Access::none : Access::read;

            switch (value->asInt())
            {
                case 0:  return Access::none;
                case 1:  return Access::read;
                case 2:  return Access::write;
                case 3:  return Access::readWrite;
                default: return Access::read;
            }
        }

        void applyRange (const json::Value& node, Node& out)
        {
            const auto* range = property (node, "RANGE");

            if (range == nullptr || ! range->isArray() || range->size() == 0)
                return;

            /*  ONE ENTRY PER ARGUMENT. The first fills the node's own bounds and
                its VALS, which every reader of a node of one value reads; the
                rest are kept as `laterRanges` (namespace draft §45), so a curve
                on the third value of `/adm/obj/1/xyz` is drawn and thinned to
                its own bounds. Arguments two onwards keep no VALS: a closed set
                of words is a property of a single-valued node in every
                description this has met. */
            for (std::size_t i = 1; i < range->size(); ++i)
            {
                Node::ArgumentRange later;
                const auto& entry = *range->at (i);

                if (const auto* minimum = property (entry, "MIN"); minimum != nullptr)
                {
                    later.hasMinimum = true;
                    later.minimum = minimum->asNumber();
                }

                if (const auto* maximum = property (entry, "MAX"); maximum != nullptr)
                {
                    later.hasMaximum = true;
                    later.maximum = maximum->asNumber();
                }

                out.laterRanges.push_back (later);
            }

            const auto& first = *range->at (0);

            if (const auto* vals = property (first, "VALS"); vals != nullptr && vals->isArray())
            {
                for (std::size_t i = 0; i < vals->size(); ++i)
                    out.enumValues.push_back (asText (*vals->at (i)));
            }

            if (const auto* minimum = property (first, "MIN"); minimum != nullptr)
            {
                out.hasMinimum = true;
                out.minimum = minimum->asNumber();
            }

            if (const auto* maximum = property (first, "MAX"); maximum != nullptr)
            {
                out.hasMaximum = true;
                out.maximum = maximum->asNumber();
            }
        }

        void applyUnit (const json::Value& node, Node& out)
        {
            const auto* unit = property (node, "UNIT");

            if (unit == nullptr)
                return;

            if (unit->isArray() && unit->size() > 0)
                out.unit = asText (*unit->at (0));
            else if (unit->isString())
                out.unit = unit->asString();
        }

        /*  One element of a PANIC array as the OSC value it spells, before it
            is read as the node's own type: a JSON number is a double, as every
            number in a description is. A null, an object or an array has no
            value to be. */
        std::optional<osc::Value> literalOf (const json::Value& element)
        {
            if (element.isNumber()) return osc::Value::float64 (element.asNumber());
            if (element.isBool())   return osc::Value::boolean (element.asBool());
            if (element.isString()) return osc::Value::string (element.asString());

            return std::nullopt;
        }

        /*  Whether a JSON number is a whole number an `i` or `h` argument can
            hold. Checked BEFORE the coercion, which truncates: 2.5 would rest
            at 2, a value nobody declared, and a number past the type's width
            would be a conversion with no defined answer. Spelled with `modf`
            and two comparisons, never `==` on a double. */
        bool fitsAWholeNumber (double number, char typeTag)
        {
            if (! std::isfinite (number))
                return false;

            double whole = 0.0;
            const auto fraction = std::modf (number, &whole);

            if (fraction < 0.0 || fraction > 0.0)
                return false;

            const auto least = typeTag == 'i' ? static_cast<double> (std::numeric_limits<std::int32_t>::min())
                                              : static_cast<double> (std::numeric_limits<std::int64_t>::min());
            const auto most = typeTag == 'i' ? static_cast<double> (std::numeric_limits<std::int32_t>::max())
                                             : static_cast<double> (std::numeric_limits<std::int64_t>::max());

            /*  `most` for 64 bits rounds UP to 2^63 as a double, which is
                already past the type - hence `<` there and not `<=`. */
            return number >= least && (typeTag == 'i' ? number <= most : number < most);
        }

        /*  WHAT THE NODE RESTS AT (PRD §4.6), from the file's GODOT.PANIC:
            "park", "snap", or a JSON array holding the declared safe VALUE
            (namespace draft §3) - one element per type tag, read as the node's
            own types, the first inside the node's range or among its VALS.

            ON A STATE NODE, ANYTHING ELSE IS A WARNING, and the device loads
            (2026-10-02, K1, decision JT, the author's: "stay flexible"; until
            then it refused the namespace, decision JC, namespace draft §23.11).
            A GODOT key is written by whoever wrote the description - a template
            by hand, or a device that describes itself as Go.dot does, whose own
            OSCQuery reply carries one on every node - and a PANIC the node could
            never hold is a mistake in that file. But one wrong word in somebody
            else's description must not unmount the whole device and fail every
            cue aimed at it. So the PANIC is ignored, the node keeps the mount's
            policy as a container or an event does, and the mistake is SAID - a
            warning naming the address and PANIC - because falling back in
            silence would hide a resting state nobody got.

            ON A CONTAINER OR AN EVENT, ANYTHING BUT A POLICY IS IGNORED IN
            SILENCE, and the mount's policy stays. Neither has a value to rest
            at, neither is ever published with a PANIC, and the kind is often
            inferred rather than declared: a node that is write-only with no
            VALUE reads as an event, so a hand-written
            `{"TYPE": "f", "ACCESS": 2, "GODOT": {"PANIC": [0]}}` says nothing
            wrong about anything it has.

            Nothing APPLIES the value yet (devplan Phase 10). It is read, so the
            tree can publish it back as the array it was. */
        void applyPanic (const json::Value& panic, Node& out, std::vector<std::string>& warnings)
        {
            /*  `out.panic` is the mount's policy already (`applyGodot`), and
                stays it: nothing below changes the node until every element
                has been read. */
            const auto ignore = [&out, &warnings] (const std::string& why)
            {
                warnings.push_back (out.address + ": PANIC " + why + " - ignored; the node rests as the"
                                    " device does (" + out.panic + ")");
            };

            if (panic.isString() && (panic.asString() == "park" || panic.asString() == "snap"))
            {
                out.panic = panic.asString();
                return;
            }

            if (out.kind != Kind::state)
                return;

            if (panic.isString())
            {
                ignore ("says \"" + panic.asString() + "\"; it must be \"park\", \"snap\""
                        " or an array holding the value the node rests at");
                return;
            }

            if (! panic.isArray())
            {
                ignore ("must be \"park\", \"snap\" or an array holding the value the node rests at");
                return;
            }

            if (out.typeTags.empty() || panic.size() != out.typeTags.size())
            {
                ignore ("holds " + std::to_string (panic.size()) + " value(s) for a node that takes "
                        + std::to_string (out.typeTags.size()) + " (TYPE \"" + out.typeTags + "\")");
                return;
            }

            std::vector<osc::Value> values;

            for (std::size_t i = 0; i < panic.size(); ++i)
            {
                const auto& element = *panic.at (i);
                const auto tag = out.typeTags[i];
                const auto where = "element " + std::to_string (i) + " ";

                const auto raw = literalOf (element);

                if (! raw.has_value())
                {
                    ignore (where + "is not a value");
                    return;
                }

                if ((tag == 'i' || tag == 'h') && element.isNumber()
                      && ! fitsAWholeNumber (element.asNumber(), tag))
                {
                    ignore (where + "is not a whole number an `" + std::string (1, tag) + "` holds");
                    return;
                }

                /*  COERCED AS A WRITE TO THIS NODE WOULD BE (`coerceToTag`: a
                    number into a numeric tag, a string into `s`, a boolean into
                    `T`), and held to MORE than a write is: a whole number for
                    `i` and `h`, checked above before the coercion truncates it;
                    the first element inside RANGE and among VALS, below; and no
                    number for `T`, which a write takes as an int 0 or 1 but a
                    JSON number never is - it arrives a double, and is ignored,
                    with a warning.
                    That is also why an `h` value past 2^53 has already been
                    rounded by the JSON reader before anything here sees it. */
                auto coerced = CommandRegistry::coerceToTag (tag, *raw);

                if (! coerced.has_value() || coerced->isNonFinite())
                {
                    ignore (where + "is not a value of type `" + std::string (1, tag) + "`");
                    return;
                }

                /*  EACH ARGUMENT AGAINST ITS OWN RANGE (namespace draft §45:
                    `applyRange` keeps one per argument now), compared against
                    the number the file wrote, not its float, so a bound of 0.1
                    holds a PANIC of 0.1 on an `f` node. The VALS are the first
                    argument's alone. */
                const auto bounds = out.rangeOf (i);

                if (element.isNumber()
                      && ((bounds.hasMinimum && element.asNumber() < bounds.minimum)
                          || (bounds.hasMaximum && element.asNumber() > bounds.maximum)))
                {
                    ignore (where + "is outside the node's RANGE");
                    return;
                }

                if (i == 0)
                {
                    if (! out.enumValues.empty()
                          && std::find (out.enumValues.begin(), out.enumValues.end(), asText (element))
                               == out.enumValues.end())
                    {
                        ignore (where + "is not one of the node's VALS");
                        return;
                    }
                }

                values.push_back (*coerced);
            }

            out.panic = "value";
            out.panicValues = std::move (values);
        }

        /*  The four declarations of PRD §3.3, taken from the mount unless the
            file overrides them.

            A namespace file MAY carry a GODOT key, and that is the whole point
            of §3.22: a hand-written template can declare what a captured one
            can only imply, and the engine cannot tell the two apart.

            After `applyRange` and the kind, which a PANIC holding a value is
            checked against. */
        void applyGodot (const json::Value& node, const MountDeclaration& mount, Node& out,
                         std::vector<std::string>& warnings)
        {
            out.rateCap = mount.rateCap;
            out.anticipatable = mount.anticipatable;
            out.panic = mount.panic;

            const auto* godot = property (node, "GODOT");

            if (godot == nullptr)
                return;

            if (const auto* rateCap = property (*godot, "RATE_CAP"); rateCap != nullptr)
                out.rateCap = rateCap->asNumber();

            if (const auto* anticipatable = property (*godot, "ANTICIPATABLE"); anticipatable != nullptr)
                out.anticipatable = anticipatable->asBool();

            /*  Until 2026-10-02 (H5) this took the key's text, whatever it was,
                so an array read as "" and was published as `"PANIC": ""`. */
            if (const auto* panic = property (*godot, "PANIC"); panic != nullptr)
                applyPanic (*panic, out, warnings);

            //  The role, a word; anything else there is not one (AFM).
            out.role = stringProperty (*godot, "ROLE");

            //  And the RCP spelling, where the file gives one (AFH, DP.7).
            if (const auto* rcp = property (*godot, "RCP"); rcp != nullptr)
            {
                out.rcpVerb = stringProperty (*rcp, "VERB");

                if (const auto* xy = property (*rcp, "XY"); xy != nullptr)
                    out.rcpIndexes = static_cast<int> (xy->asNumber());
            }

            //  And the command line it renders to on the line wire (DP.8).
            out.lineTemplate = stringProperty (*godot, "LINE");

            //  And its shape on the MIDI wire (DP.9), fixed keys.
            if (const auto* shape = property (*godot, "MIDI"); shape != nullptr)
            {
                auto& m = out.midi;
                const auto number = [shape] (const char* key, int otherwise)
                {
                    const auto* found = property (*shape, key);
                    return found != nullptr && found->isNumber() ? static_cast<int> (found->asNumber()) : otherwise;
                };
                const auto flag = [shape] (const char* key, bool otherwise)
                {
                    const auto* found = property (*shape, key);
                    return found != nullptr && found->isBool() ? found->asBool() : otherwise;
                };

                m.kind = stringProperty (*shape, "KIND");
                m.channel = number ("CHANNEL", 0);
                m.offset = number ("OFFSET", 0);
                m.program = number ("PROGRAM", -1);
                m.start = number ("START", 0);
                m.banked = flag ("BANKED", false);
                m.note = number ("NOTE", 0);
                m.hasOnOff = property (*shape, "ON") != nullptr;
                m.on = number ("ON", 127);
                m.off = number ("OFF", 0);
                m.release = flag ("RELEASE", true);
                m.cc = number ("CC", 0);
                m.msb = number ("MSB", 0);
                m.lsb = number ("LSB", 0);
                m.bits = number ("BITS", 7);
                m.fine = number ("FINE", -1);
                m.command = number ("COMMAND", 0);

                if (const auto* bytes = property (*shape, "BYTES"); bytes != nullptr && bytes->isArray())
                    for (const auto& token : bytes->asArray())
                        m.bytes.push_back (token.isString() ? token.asString() : std::string {});
            }
        }

        /*  Container, state or event.

            The file's own GODOT.KIND wins. Failing that: no type and some
            children is a container; write-only with no VALUE is an event,
            because a node you can only write and never read has nothing to
            report at a given time; everything else is state. */
        Kind kindFrom (const json::Value& node, const std::string& typeTags, bool hasChildren)
        {
            if (const auto* godot = property (node, "GODOT"); godot != nullptr)
            {
                const auto declared = stringProperty (*godot, "KIND");

                if (declared == "container") return Kind::container;
                if (declared == "state")     return Kind::state;
                if (declared == "event")     return Kind::event;
            }

            if (typeTags.empty() && hasChildren)
                return Kind::container;

            const auto* access = property (node, "ACCESS");
            const auto writeOnly = access != nullptr && access->asInt() == 2;

            return (writeOnly && property (node, "VALUE") == nullptr) ? Kind::event : Kind::state;
        }

        //======================================================================
        /*  Joins a captured root path with a path inside the capture. The root
            is "/" for a whole-namespace capture and something like "/wfs" for a
            subtree one; both are ordinary. */
        std::string joinPath (const std::string& rootPath, const std::string& inside)
        {
            if (inside.empty())
                return rootPath;

            return rootPath == "/" ? inside : rootPath + inside;
        }

        /*  "/" and each name under a description's CONTENTS: the roots of a
            device whose file is rooted at "/" (AFK). */
        std::vector<std::string> rootsUnderJson (const json::Value& root)
        {
            std::vector<std::string> roots;

            if (const auto* contents = property (root, "CONTENTS"); contents != nullptr && contents->isObject())
                for (const auto& member : contents->asObject())
                    roots.push_back ("/" + member.first);

            return roots;
        }

        std::string spaceJoined (const std::vector<std::string>& words)
        {
            std::string text;

            for (const auto& word : words)
                text += (text.empty() ? "" : " ") + word;

            return text;
        }

        void collect (const json::Value& node, const std::string& inside,
                      const std::string& rootPath, const std::string& mountAt,
                      const MountDeclaration& mount,
                      std::vector<Node>& out, std::vector<std::string>& problems,
                      std::vector<std::string>& warnings)
        {
            /*  WHERE IT IS MOUNTED: the device's one prefix - or nothing, when
                the device's roots are the file's own first-level names (AFK):
                the file is rooted at "/" then, and every node sits at its
                own FULL_PATH, which is the address in the manual. */
            const auto address = mountAt + inside;

            if (! node.isObject())
            {
                problems.push_back (address + ": not a JSON object");
                return;
            }

            const auto* contents = property (node, "CONTENTS");
            const auto hasChildren = contents != nullptr && contents->isObject();
            const auto typeTags = stringProperty (node, "TYPE");

            /*  FULL_PATH is checked against where the node actually sits rather
                than trusted. They agree in any well-formed description; when
                they do not, one of them is a lie and the nesting is the one
                that cannot be. */
            if (const auto declared = stringProperty (node, "FULL_PATH"); ! declared.empty())
            {
                const auto expected = joinPath (rootPath, inside);

                if (declared != expected)
                    problems.push_back (address + ": FULL_PATH says \"" + declared
                                        + "\", but it is nested at \"" + expected + "\"");
            }

            Node built;
            built.address = address;
            built.typeTags = typeTags;
            built.kind = kindFrom (node, typeTags, hasChildren);
            built.access = accessFrom (node, built.kind == Kind::container);
            built.description = stringProperty (node, "DESCRIPTION");

            applyRange (node, built);
            applyUnit (node, built);
            applyGodot (node, mount, built, warnings);

            /*  VALUE is read for the kind inference above and then dropped.
                A captured description says what the target happened to be doing
                when somebody pointed a browser at it, and PRD §4.10 keeps that
                out of anything Go.dot treats as known. A mounted node has no
                value until something writes one. */
            built.values.clear();

            /*  THE FILE'S "/" IS NOT A NODE when its entries are the roots:
                there is no address "", and nothing is ever aimed at it. */
            if (! (mountAt.empty() && inside.empty()))
                out.push_back (std::move (built));

            if (! hasChildren)
                return;

            for (const auto& member : contents->asObject())
            {
                const auto& name = member.first;

                if (name.empty() || name.find ('/') != std::string::npos)
                {
                    problems.push_back ((address.empty() ? std::string ("/") : address)
                                        + ": \"" + name + "\" is not a usable node name");
                    continue;
                }

                collect (member.second, inside + "/" + name, rootPath, mountAt, mount, out, problems, warnings);
            }
        }
    }

    //==============================================================================
    std::vector<std::string> prefixesOf (const std::string& prefixRow)
    {
        std::vector<std::string> out;
        std::size_t at = 0;

        while (at < prefixRow.size())
        {
            const auto start = prefixRow.find_first_not_of (' ', at);

            if (start == std::string::npos)
                break;

            const auto end = prefixRow.find (' ', start);
            out.push_back (prefixRow.substr (start, end == std::string::npos
                                                      ? std::string::npos : end - start));
            at = end == std::string::npos ? prefixRow.size() : end + 1;
        }

        return out;
    }

    std::size_t prefixMatchLength (const std::string& address, const std::string& prefixRow)
    {
        std::size_t best = 0;

        for (const auto& prefix : prefixesOf (prefixRow))
        {
            /*  THE BOUNDARY IS A SEPARATOR. Without the last test `/desktop`
                would be under `/desk`, and a device whose prefix happens to
                begin another's would quietly take its cues. */
            if (address.size() > prefix.size()
                  && address.compare (0, prefix.size(), prefix) == 0
                  && address[prefix.size()] == '/'
                  && prefix.size() > best)
                best = prefix.size();
        }

        return best;
    }

    //==============================================================================
    MountResult MountResult::failed (std::string problem)
    {
        MountResult result;
        result.problems.push_back (std::move (problem));
        return result;
    }

    //==============================================================================
    std::vector<std::string> rootsOfNamespace (std::string_view jsonText)
    {
        const auto parsed = json::parse (jsonText);

        if (! parsed.ok() || ! parsed.value->isObject())
            return {};

        const auto rootPath = stringProperty (*parsed.value, "FULL_PATH");

        if (! rootPath.empty() && rootPath != "/")
            return { rootPath };

        return rootsUnderJson (*parsed.value);
    }

    MountResult readNamespace (const MountDeclaration& mount, std::string_view jsonText)
    {
        std::string why;

        if (! prefixesAreUsable (mount, why))
            return MountResult::failed (mount.id + ": " + why);

        const auto parsed = json::parse (jsonText);

        if (! parsed.ok())
            return MountResult::failed (mount.id + ": " + mount.namespaceFile
                                        + " is not valid JSON at line "
                                        + std::to_string (parsed.line) + ": " + parsed.error);

        /*  The capture's own root path. "/" for a whole namespace; something
            like "/wfs" when somebody captured a subtree, which is the normal
            thing to do when a target's namespace already sits under a container
            of its own - GET /wfs rather than GET / avoids mounting `/wfs` at
            `/wfs` and getting `/wfs/wfs`. Either way the MOUNTED address is the
            prefix plus the nesting, and this is only used to check FULL_PATH. */
        auto rootPath = stringProperty (*parsed.value, "FULL_PATH");

        if (rootPath.empty())
            rootPath = "/";

        /*  SEVERAL ROOTS (namespace draft §57, AFK): a description rooted at
            "/" whose first-level entries are the device's roots - an X32's
            /ch, /bus and /dca, the S21's /channel, /console and /digico -
            mounts each entry at its own name, so the addresses in the cues
            are the manual's and `mountOf` finds the device by any of them.
            The prefix row has to name exactly those: a root the file lacks
            would take cues to a box whose nodes are published nowhere, and
            a root the row lacks would publish nodes no cue can reach. A
            file rooted anywhere else is one tree and mounts in one place,
            as it always did. */
        std::string mountAt = mount.prefix;

        /*  AND A FILE ROOTED AT "/" WHOSE ONE ENTRY IS THE ONE PREFIX mounts it
            at its own name too (found 2026-10-11 by the wires driver, DP.11):
            read as one tree under the prefix, such a file published every
            node twice under it - /track/track/1/gain - and the Holophonix,
            DiGiCo SD and Yamaha OSC presets aimed every cue at nothing. A
            capture of a whole namespace - "GET /" - mounted under a prefix of
            its own keeps nesting as it always did: its entries are not the
            prefix, and that is how the two are told apart. */
        const auto prefixes = prefixesOf (mount.prefix);
        auto fileRoots = rootPath == "/" ? rootsUnderJson (*parsed.value) : std::vector<std::string> {};
        auto rowRoots = prefixes;
        std::sort (fileRoots.begin(), fileRoots.end());
        std::sort (rowRoots.begin(), rowRoots.end());

        if (prefixes.size() > 1 || (rootPath == "/" && fileRoots == rowRoots))
        {
            if (rootPath != "/")
                return MountResult::failed (mount.id + ": a device with several roots needs a description"
                                                       " whose root is \"/\" and whose entries are those"
                                                       " roots; this file's root is \"" + rootPath + "\"");

            if (fileRoots != rowRoots)
                return MountResult::failed (mount.id + ": the description's roots are " + spaceJoined (fileRoots)
                                            + " and the prefix row says " + spaceJoined (rowRoots)
                                            + "; a device with several roots names exactly its file's");

            mountAt.clear();
        }

        MountResult result;
        /*  HOW THE DEVICE IS ASKED AND SUBSCRIBED (namespace draft §57, AFL;
            DP.10), the file's own words at its root. */
        if (const auto* godot = property (*parsed.value, "GODOT"); godot != nullptr)
        {
            result.getTemplate = stringProperty (*godot, "GET");
            result.subscribeTemplate = stringProperty (*godot, "SUBSCRIBE");
        }

        collect (*parsed.value, {}, rootPath, mountAt, mount, result.nodes, result.problems, result.warnings);

        /*  Sorted by address, like every other part of the tree: lookup is a
            binary search and merging is linear. */
        std::sort (result.nodes.begin(), result.nodes.end(),
                   [] (const Node& a, const Node& b) { return a.address < b.address; });

        /*  Two nodes at one address would make a lookup depend on which was
            reached first, so it is a refusal rather than a warning. */
        for (std::size_t i = 1; i < result.nodes.size(); ++i)
            if (result.nodes[i - 1].address == result.nodes[i].address)
                result.problems.push_back (result.nodes[i].address + ": declared twice");

        result.ok = result.problems.empty() && ! result.nodes.empty();

        if (result.nodes.empty())
            result.problems.push_back (mount.id + ": " + mount.namespaceFile
                                       + " describes no nodes");

        return result;
    }

    //==============================================================================
    MountResult MountTable::load (const MountDeclaration& mount, std::string_view json)
    {
        auto result = readNamespace (mount, json);

        if (! result.ok)
        {
            /*  A failed reload forgets what was there. A half-loaded mount
                would publish a namespace nobody has, and the operator would be
                looking at nodes that are no longer described. */
            mounts.erase (mount.id);
            bumpShape();
            return result;
        }

        mounts[mount.id] = Entry { mount, result.nodes, result.warnings, result.getTemplate, result.subscribeTemplate };
        bumpShape();
        return result;
    }

    MountResult MountTable::declare (const MountDeclaration& mount)
    {
        /*  The prefix is checked here exactly as `load` checks it through
            `readNamespace`, and for the same reasons: a prefix of "/" would
            mount over the whole tree, and one under `/godot` would put a
            device where the engine already answers. An opaque device writes
            nothing into the tree, but its prefix is still what `mountOf`
            matches every outgoing address against, so a bad one would silently
            claim cues meant for somebody else. */
        std::string why;

        if (! prefixesAreUsable (mount, why))
            return MountResult::failed (mount.id + ": " + why);

        MountResult result;
        result.ok = true;

        mounts[mount.id] = Entry { mount, {}, {}, {}, {} };
        bumpShape();

        return result;
    }

    bool MountTable::updateDeclaration (const MountDeclaration& mount)
    {
        const auto found = mounts.find (mount.id);

        if (found == mounts.end())
            return false;

        /*  THE NODES STAY. What changed is where the box is or how it is
            spoken to, and neither is a fact about what it has: re-reading the
            namespace here would throw away every value the tree holds for this
            device, and every read-back a verified cue is waiting on, to arrive
            at the same list of nodes. The caller decides that a changed prefix
            or namespace file needs a reload; this is for everything else. */
        found->second.declaration = mount;
        bumpShape();

        return true;
    }

    void MountTable::setProblem (const std::string& mountId, std::string problem)
    {
        /*  Kept even for a mount that has no entry - a device refused for
            having no port never became one, and this sentence is the only
            thing anybody can act on. Cleared with an empty string rather than
            by a second method, so the caller that succeeds says so the same
            way the caller that fails does. */
        if (problem.empty())
            problems.erase (mountId);
        else
            problems[mountId] = std::move (problem);

        bumpShape();
    }

    std::string MountTable::problemOf (const std::string& mountId) const
    {
        const auto found = problems.find (mountId);
        return found == problems.end() ? std::string {} : found->second;
    }

    std::vector<std::string> MountTable::warnings() const
    {
        std::vector<std::string> all;

        for (const auto& entry : mounts)
            all.insert (all.end(), entry.second.warnings.begin(), entry.second.warnings.end());

        return all;
    }

    bool MountTable::unload (const std::string& mountId)
    {
        bumpShape();
        problems.erase (mountId);
        return mounts.erase (mountId) > 0;
    }

    void MountTable::clear()
    {
        bumpShape();
        mounts.clear();
        problems.clear();
    }

    bool MountTable::isLoaded (const std::string& mountId) const
    {
        return mounts.find (mountId) != mounts.end();
    }

    std::size_t MountTable::nodeCount (const std::string& mountId) const
    {
        const auto it = mounts.find (mountId);
        return it == mounts.end() ? 0 : it->second.nodes.size();
    }

    std::vector<Node> MountTable::allNodes() const
    {
        std::vector<Node> all;

        for (const auto& entry : mounts)
            all.insert (all.end(), entry.second.nodes.begin(), entry.second.nodes.end());

        std::sort (all.begin(), all.end(),
                   [] (const Node& a, const Node& b) { return a.address < b.address; });

        return all;
    }

    //==============================================================================
    const Node* MountTable::nodeAt (const std::string& address) const
    {
        return const_cast<MountTable*> (this)->findNode (address);
    }

    void MountTable::noteReadback (const std::string& address, const osc::Values& values)
    {
        readbacks[address] = values;
    }

    void MountTable::noteReply (const std::string& mountId, const std::string& word, const std::string& line)
    {
        replies[mountId] = word.empty() ? line : line;
    }

    std::string MountTable::lastReplyOf (const std::string& mountId) const
    {
        const auto found = replies.find (mountId);
        return found == replies.end() ? std::string {} : found->second;
    }

    std::string MountTable::getTemplateOf (const std::string& mountId) const
    {
        const auto found = mounts.find (mountId);
        return found == mounts.end() ? std::string {} : found->second.getTemplate;
    }

    std::string MountTable::subscribeTemplateOf (const std::string& mountId) const
    {
        const auto found = mounts.find (mountId);
        return found == mounts.end() ? std::string {} : found->second.subscribeTemplate;
    }

    std::vector<const Node*> MountTable::nodesOf (const std::string& mountId) const
    {
        std::vector<const Node*> out;
        const auto found = mounts.find (mountId);

        if (found == mounts.end())
            return out;

        out.reserve (found->second.nodes.size());

        for (const auto& node : found->second.nodes)
            out.push_back (&node);

        return out;
    }

    const osc::Values* MountTable::readbackOf (const std::string& address) const
    {
        const auto found = readbacks.find (address);
        return found == readbacks.end() ? nullptr : &found->second;
    }

    void MountTable::noteObservation (const std::string& address, const osc::Values& values,
                                      std::int64_t tick, std::int64_t writesWhenAsked)
    {
        /*  ASKED BEFORE GO.DOT'S LAST WRITE HERE (2026-10-03, §24.13, OU): an
            account of the desk from before that write, however late it came
            back - dropped, as the write itself would have forgotten it. */
        if (writesWhenAsked >= 0 && writesWhenAsked != writesOf (address))
            return;

        observations.insert_or_assign (address, values);
        observedTicks.insert_or_assign (address, tick);

        /*  THE FIRST SINCE GO.DOT LAST WROTE THE ADDRESS, kept apart (2026-10-03,
            Doh! D3, namespace draft §24.13): what a desk made of that write - a
            motor fader's step, a dB-mapped float - rather than what a hand did
            to it after. Set only while unset; the write below forgets it. */
        firstObservations.emplace (address, values);
    }

    std::int64_t MountTable::writesOf (const std::string& address) const
    {
        const auto found = writeCounts.find (address);
        return found == writeCounts.end() ? 0 : found->second;
    }

    std::int64_t MountTable::observedAtTick (const std::string& address) const
    {
        const auto found = observedTicks.find (address);
        return found == observedTicks.end() ? -1 : found->second;
    }

    const osc::Values* MountTable::observedOf (const std::string& address) const
    {
        const auto found = observations.find (address);
        return found == observations.end() ? nullptr : &found->second;
    }

    void MountTable::forgetObservation (const std::string& address)
    {
        observations.erase (address);
        observedTicks.erase (address);
        firstObservations.erase (address);
        heardTicks.erase (address);
    }

    const osc::Values* MountTable::firstObservedOf (const std::string& address) const
    {
        const auto found = firstObservations.find (address);
        return found == firstObservations.end() ? nullptr : &found->second;
    }

    void MountTable::forgetReadback (const std::string& address)
    {
        readbacks.erase (address);
    }

    const MountDeclaration* MountTable::declarationOf (const std::string& mountId) const
    {
        const auto found = mounts.find (mountId);
        return found == mounts.end() ? nullptr : &found->second.declaration;
    }

    std::string MountTable::mountOf (const std::string& address) const
    {
        /*  By PREFIX rather than by searching the nodes, so an address that is
            under a mount but names a node it does not have still says which box
            it was aimed at. That is what lets a cue pointing at a mistyped node
            be reported against the mount somebody meant.

            THE LONGEST MATCH WINS, and until 2026-09-22 it did not: this loop
            returned the first device whose prefix fitted, walking a map keyed
            by identifier - so with `/desk` and `/desk/aux` both declared, which
            one got the cue depended on the alphabetical order of two random
            eight-character identifiers. Nesting was always legal and nothing
            made it reachable until a window let somebody type a prefix, at
            which point the panel and the engine could name different devices
            for one address. `prefixMatchLength` is the one rule both ask. */
        std::string best;
        std::size_t covered = 0;

        for (const auto& [id, entry] : mounts)
        {
            const auto length = prefixMatchLength (address, entry.declaration.prefix);

            if (length > covered)
            {
                covered = length;
                best = id;
            }
        }

        return best;
    }

    std::vector<std::string> MountTable::ids() const
    {
        std::vector<std::string> out;
        out.reserve (mounts.size());

        for (const auto& [id, entry] : mounts)
            out.push_back (id);

        return out;
    }

    Node* MountTable::findNode (const std::string& address)
    {
        for (auto& entry : mounts)
        {
            auto& nodes = entry.second.nodes;

            const auto it = std::lower_bound (nodes.begin(), nodes.end(), address,
                                              [] (const Node& node, const std::string& target)
                                              {
                                                  return node.address < target;
                                              });

            if (it != nodes.end() && it->address == address)
                return &*it;
        }

        return nullptr;
    }

    MountTable::WriteResult MountTable::write (const std::string& address, const osc::Values& values)
    {
        auto* node = findNode (address);

        if (node == nullptr)
        {
            /*  NO NODE HERE, AND THAT IS EITHER A MISTAKE OR THE WHOLE POINT.

                Against a DESCRIBED device it is a mistake, and the refusal is
                the reason to describe one at all: the show said what that box
                has, this address is not among it, and saying so now is better
                than a datagram that leaves and is ignored. UDP will never tell
                anybody it was wrong.

                Against an OPAQUE device there is nothing to be wrong about.
                Nobody said what the desk has; somebody said where it is and
                what to send it. So the value goes out exactly as the cue
                spells it - no coercion, because there is no declared type to
                coerce to - and nothing is stored, because a value nobody can
                read back is not a fact about the device, only about what was
                sent. `sent` on the mount is what a rehearsal reads instead.

                An address under no device at all is still `bad-address`: it
                is a cue aimed at nothing, which is what the target menu and
                validate both exist to catch. */
            const auto owner = mountOf (address);
            const auto* declaration = declarationOf (owner);

            if (declaration == nullptr || ! declaration->opaque())
                return { false, reason::badAddress, {}, {} };

            return { true, {}, owner, values };
        }

        if (node->access != Access::write && node->access != Access::readWrite)
            return { false, reason::readOnly, {}, {} };

        /*  AS MANY VALUES AS THE NODE HAS TYPE TAGS, each coerced to its own
            (namespace draft §45). One value to a node of two was sent as one
            argument until this was written - `/wfs/input/positionX` takes the
            channel and the metres, and a message carrying only the first is
            one the device reads as something else, or refuses. */
        /*  AND A NODE THAT TAKES NO ARGUMENT - no TYPE at all, or only `N` and
            `I` - takes none (namespace draft §56): a command such as a desk's
            or a processor's GO, which a description says exists and says
            carries nothing. It goes out as a bare message. Until 2026-10-10
            every write to one was `type-mismatch`, so a described device's GO
            could be listed and never sent. */
        const auto takesNothing = node->typeTags.find_first_not_of ("NI") == std::string::npos;

        if (takesNothing ? ! values.empty() : values.size() != node->typeTags.size())
            return { false, reason::typeMismatch, {}, {} };

        osc::Values coerced;
        coerced.reserve (values.size());

        for (std::size_t i = 0; i < values.size(); ++i)
        {
            auto one = CommandRegistry::coerceToTag (node->typeTags[i], values[i]);

            if (! one.has_value())
                return { false, reason::typeMismatch, {}, {} };

            coerced.push_back (std::move (*one));
        }

        /*  It lands here and goes no further. There is no transport in Phase 1,
            and that is the whole extent of what a stub does NOT do - the value
            is in the tree, the event is in the log, and a replay reproduces
            both. Phase 2 puts a socket after this line. */
        node->values = coerced;

        /*  A VALUE, NOT A SHAPE (ZC): the tree copies this node over its cached
            half rather than rebuilding all of it. */
        ++valueVersion;
        written.insert (address);

        /*  AND WHAT THE TARGET WAS SEEN TO HOLD IS NOW HISTORY. The next
            observation will say what it holds after this write; until then the
            written value is the best account there is, and an observation from
            before it would make a jump believe the desk still holds what it
            held a minute ago. */
        observations.erase (address);
        observedTicks.erase (address);
        firstObservations.erase (address);
        heardTicks.erase (address);

        /*  AND AN ANSWER STILL ON ITS WAY is history too (OU): counted, so a
            sweep asked before this write is known for one when it lands. */
        ++writeCounts[address];

        /*  IT LANDS HERE AND STOPS HERE, still. What goes on the wire is a
            MountSender's business and the caller's to arrange - this class
            names no socket, which is what lets every rule above be tested
            against a string literal. The mount id and the coerced value are
            handed back so the caller has both without looking anything up. */
        return { true, {}, mountOf (address), std::move (coerced) };
    }

    void MountTable::bumpShape()
    {
        ++version;

        /*  The tree rebuilds the whole half from the values the nodes hold now,
            so what was written before is in it. */
        written.clear();
    }

    const osc::Values* MountTable::valueOf (const std::string& address) const
    {
        auto* self = const_cast<MountTable*> (this);
        const auto* node = self->findNode (address);

        return (node != nullptr && ! node->values.empty()) ? &node->values : nullptr;
    }
}
