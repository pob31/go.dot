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

#include <wfg/engine/tree/OscQueryJson.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        /*  JSON string escaping, and no more of it than JSON requires: the
            two characters that would end or continue the literal, and the
            control range, which has to be spelled \u00XX because a raw control
            character is not legal inside a JSON string. Everything above is
            UTF-8 and passes through untouched. */
        std::string escape (std::string_view text)
        {
            std::string out;
            out.reserve (text.size() + 8);

            for (const char raw : text)
            {
                const auto c = static_cast<unsigned char> (raw);

                switch (c)
                {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;

                    default:
                        if (c < 0x20)
                        {
                            static constexpr char hex[] = "0123456789abcdef";
                            out += "\\u00";
                            out += hex[(c >> 4) & 0x0f];
                            out += hex[c & 0x0f];
                        }
                        else
                        {
                            out += static_cast<char> (c);
                        }

                        break;
                }
            }

            return out;
        }

        std::string quoted (std::string_view text)
        {
            return "\"" + escape (text) + "\"";
        }

        std::string indent (int depth)
        {
            return std::string (static_cast<std::size_t> (depth) * 2, ' ');
        }

        /** The last segment of an address, which is its key inside CONTENTS. */
        std::string leafName (const std::string& address)
        {
            const auto slash = address.rfind ('/');
            return slash == std::string::npos ? address : address.substr (slash + 1);
        }

        std::string parentOf (const std::string& address)
        {
            const auto slash = address.rfind ('/');

            if (slash == std::string::npos)
                return {};

            return slash == 0 ? std::string ("/") : address.substr (0, slash);
        }

        /*  One OSC value as a JSON scalar. Numbers through the project's own
            formatter, never through a stream or std::to_string on a double -
            see the header. */
        std::string valueLiteral (const osc::Value& value)
        {
            if (value.isString())  return quoted (value.getString());
            if (value.isBool())    return value.getBool() ? "true" : "false";
            if (value.isInt32())   return std::to_string (value.getInt32());
            if (value.isInt64())   return std::to_string (value.getInt64());
            if (value.isFloat32()) return osc::formatFloat (value.getFloat32());
            if (value.isFloat64()) return osc::formatDouble (value.getFloat64());

            /*  A blob, a nil, an impulse or a time tag. No node in Phase 1's
                table is any of those; null says "the engine has no value for
                this" rather than inventing one. */
            return "null";
        }

        std::string kindName (Kind kind)
        {
            switch (kind)
            {
                case Kind::container: return "container";
                case Kind::state:     return "state";
                case Kind::event:     return "event";
            }

            return "state";
        }

        //======================================================================
        /*  The GODOT key: PRD §3.3's four declarations, in a fixed order.

            ONLY A STATE NODE DECLARES ALL FOUR. RATE_CAP, ANTICIPATABLE and
            PANIC are statements about a VALUE - how often it may be pushed,
            whether it may be sent early, what it rests at - and a container
            has no value while an event has none at any given time. Writing
            `"PANIC": "park"` on `cue.create` would be filling in a form rather
            than saying something, and a client reading it would be entitled to
            believe it. So those two declare what they are and stop. */
        void writeGodot (const Node& node, int depth, std::string& out)
        {
            out += indent (depth) + "\"GODOT\": {";

            if (node.kind != Kind::state)
            {
                out += "\"KIND\": " + quoted (kindName (node.kind)) + "}";
                return;
            }

            out += "\"KIND\": " + quoted (kindName (node.kind));
            out += ", \"RATE_CAP\": " + osc::formatDouble (node.rateCap);
            out += ", \"ANTICIPATABLE\": " + std::string (node.anticipatable ? "true" : "false");
            out += ", \"PANIC\": " + quoted (node.panic);
            out += "}";
        }

        /*  RANGE, as the proposal spells it: one entry per argument. Every node
            here carries one value, so there is one entry.

            An enumerated node gets VALS and no bounds - the closed set IS the
            range, and a MIN alongside it would be a second, weaker statement of
            the same thing. */
        bool writeRange (const Node& node, int depth, std::string& out)
        {
            if (node.enumValues.empty() && ! node.hasMinimum && ! node.hasMaximum)
                return false;

            out += indent (depth) + "\"RANGE\": [{";

            if (! node.enumValues.empty())
            {
                out += "\"VALS\": [";

                for (std::size_t i = 0; i < node.enumValues.size(); ++i)
                {
                    if (i > 0)
                        out += ", ";

                    out += quoted (node.enumValues[i]);
                }

                out += "]";
            }
            else
            {
                if (node.hasMinimum)
                    out += "\"MIN\": " + osc::formatDouble (node.minimum);

                if (node.hasMaximum)
                {
                    if (node.hasMinimum)
                        out += ", ";

                    out += "\"MAX\": " + osc::formatDouble (node.maximum);
                }
            }

            out += "}]";
            return true;
        }

        //======================================================================
        using ChildMap = std::map<std::string, std::vector<const Node*>>;

        /*  Keys in a fixed order, and children sorted by name, so two identical
            trees serialise to identical bytes. That is what makes a committed
            golden worth having. */
        void writeNode (const Node& node, const ChildMap& children, int depth, std::string& out)
        {
            const auto pad = indent (depth);
            const auto inner = indent (depth + 1);

            out += "{\n";

            std::vector<std::string> lines;

            lines.push_back (inner + "\"FULL_PATH\": " + quoted (node.address));

            if (! node.description.empty())
                lines.push_back (inner + "\"DESCRIPTION\": " + quoted (node.description));

            if (! node.typeTags.empty())
                lines.push_back (inner + "\"TYPE\": " + quoted (node.typeTags));

            lines.push_back (inner + "\"ACCESS\": " + std::to_string (static_cast<int> (node.access)));

            if (node.value.has_value())
                lines.push_back (inner + "\"VALUE\": [" + valueLiteral (*node.value) + "]");

            {
                std::string range;

                if (writeRange (node, depth + 1, range))
                    lines.push_back (range);
            }

            if (! node.unit.empty())
                lines.push_back (inner + "\"UNIT\": [" + quoted (node.unit) + "]");

            {
                std::string godot;
                writeGodot (node, depth + 1, godot);
                lines.push_back (godot);
            }

            const auto found = children.find (node.address);

            if (found != children.end() && ! found->second.empty())
            {
                std::string contents = inner + "\"CONTENTS\": {\n";

                for (std::size_t i = 0; i < found->second.size(); ++i)
                {
                    const auto* child = found->second[i];

                    contents += indent (depth + 2) + quoted (leafName (child->address)) + ": ";
                    writeNode (*child, children, depth + 2, contents);

                    if (i + 1 < found->second.size())
                        contents += ",";

                    contents += "\n";
                }

                contents += inner + "}";
                lines.push_back (contents);
            }

            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                out += lines[i];

                if (i + 1 < lines.size())
                    out += ",";

                out += "\n";
            }

            out += pad + "}";
        }
    }

    //==============================================================================
    std::string OscQueryJson::describe (const TreeSnapshot& snapshot, std::string_view address)
    {
        const auto* root = snapshot.find (address);

        if (root == nullptr)
            return {};

        /*  The parent-to-children map is built once for the whole snapshot
            rather than asked for per node. childrenOf() walks everything, so
            recursing through it would be quadratic - fine on a fixture, and
            not fine on the four thousand nodes a real show has. */
        ChildMap children;

        for (const auto* node : snapshot.all())
        {
            const auto parent = parentOf (node->address);

            if (! parent.empty() && parent != node->address)
                children[parent].push_back (node);
        }

        /*  all() is already in address order, so each parent's children arrive
            sorted by their full address - and since they share a prefix, that
            is sorted by name. No second sort. */
        std::string out;
        writeNode (*root, children, 0, out);
        out += "\n";

        return out;
    }

    //==========================================================================
    OscQueryJson::Attribute OscQueryJson::attribute (const TreeSnapshot& snapshot,
                                                     std::string_view address,
                                                     std::string_view key)
    {
        /*  The attribute names OSCQuery defines and Go.dot answers. A name
            outside this set is a client asking for something the protocol does
            not have, which is a 400 and not a 404: the node may well exist. */
        static const std::set<std::string> known {
            "VALUE", "TYPE", "RANGE", "ACCESS", "DESCRIPTION", "CLIPMODE", "UNIT", "HOST_INFO"
        };

        Attribute out;
        const std::string name { key };

        if (known.find (name) == known.end())
        {
            out.result = AttributeResult::noSuchAttribute;
            return out;
        }

        const auto* node = snapshot.find (address);

        if (node == nullptr)
        {
            out.result = AttributeResult::noSuchNode;
            return out;
        }

        /*  Built through the SAME helpers describe() uses, deliberately. Two
            code paths writing the same attribute two ways is how `GET /` and
            `GET /x?VALUE` come to disagree about a number, and the locale test
            only covers whichever one it happens to call. */
        std::string body;

        if (name == "VALUE")
        {
            if (! node->value.has_value())
            {
                out.result = AttributeResult::notPresent;
                return out;
            }

            body = "\"VALUE\": [" + valueLiteral (*node->value) + "]";
        }
        else if (name == "TYPE")
        {
            if (node->typeTags.empty())
            {
                out.result = AttributeResult::notPresent;
                return out;
            }

            body = "\"TYPE\": " + quoted (node->typeTags);
        }
        else if (name == "ACCESS")
        {
            //  Every node has one, including a container, which is why this is
            //  the one attribute that can never answer 204.
            body = "\"ACCESS\": " + std::to_string (static_cast<int> (node->access));
        }
        else if (name == "DESCRIPTION")
        {
            if (node->description.empty())
            {
                out.result = AttributeResult::notPresent;
                return out;
            }

            body = "\"DESCRIPTION\": " + quoted (node->description);
        }
        else if (name == "UNIT")
        {
            if (node->unit.empty())
            {
                out.result = AttributeResult::notPresent;
                return out;
            }

            body = "\"UNIT\": [" + quoted (node->unit) + "]";
        }
        else if (name == "RANGE")
        {
            std::string range;

            if (! writeRange (*node, 1, range))
            {
                out.result = AttributeResult::notPresent;
                return out;
            }

            //  writeRange indents for the tree writer; this reply has no tree.
            body = range.substr (range.find_first_not_of (' '));
        }
        else if (name == "CLIPMODE")
        {
            /*  Deliberately never present. CLIPMODE says what a target does to
                a value outside its RANGE, and Go.dot does not clip: a write out
                of range is REJECTED and logged as an `R`, because a cue that
                silently became a different cue is worse than one that refused.
                Answering 204 says "this node has no clip mode", which is true.
                Answering "none" would be a claim about clipping behaviour that
                a client might then rely on. */
            out.result = AttributeResult::notPresent;
            return out;
        }
        else    // HOST_INFO, which is a query about the SERVER, not about a node
        {
            out.result = AttributeResult::noSuchAttribute;
            return out;
        }

        out.result = AttributeResult::found;
        out.json = "{\n  " + body + "\n}\n";
        return out;
    }

    //==========================================================================
    std::string OscQueryJson::hostInfo (const HostInfo& info)
    {
        /*  EXTENSIONS lists what this server ACTUALLY implements, and the
            absent ones are as load-bearing as the present ones - a client reads
            this to decide what not to try.

            `CRITICAL` is false: it means guaranteed delivery over TCP, and
            Phase 1 speaks OSC over UDP only. `PATH_RENAMED` is false because
            Go.dot never renames a path - objects are identity-addressed, so a
            rename changes a `name` VALUE and the address is unaffected, which
            is the whole reason a client's LISTEN survives an edit. */
        std::string out;

        out += "{\n";
        out += "  \"NAME\": " + quoted (info.name) + ",\n";
        out += "  \"OSC_PORT\": " + std::to_string (info.oscPort) + ",\n";
        out += "  \"OSC_TRANSPORT\": \"UDP\",\n";
        out += "  \"WS_PORT\": " + std::to_string (info.wsPort) + ",\n";
        out += "  \"EXTENSIONS\": {\n";
        out += "    \"ACCESS\": true,\n";
        out += "    \"CLIPMODE\": false,\n";
        out += "    \"CRITICAL\": false,\n";
        out += "    \"DESCRIPTION\": true,\n";
        out += "    \"LISTEN\": true,\n";
        out += "    \"PATH_ADDED\": true,\n";
        out += "    \"PATH_CHANGED\": true,\n";
        out += "    \"PATH_REMOVED\": true,\n";
        out += "    \"PATH_RENAMED\": false,\n";
        out += "    \"RANGE\": true,\n";
        out += "    \"TAGS\": false,\n";
        out += "    \"TYPE\": true,\n";
        out += "    \"UNIT\": true,\n";
        out += "    \"VALUE\": true\n";
        out += "  }\n";
        out += "}\n";

        return out;
    }
}
