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

        /*  A prefix has to be an absolute OSC address with no trailing slash
            and no empty segment, because every mounted address is built by
            sticking it in front of one. A prefix of "/" would put somebody
            else's namespace at the root, on top of /godot. */
        bool prefixIsUsable (const std::string& prefix, std::string& why)
        {
            if (prefix.empty() || prefix.front() != '/')
            {
                why = "a mount prefix must start with '/'";
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

            if (prefix.find ("//") != std::string::npos)
            {
                why = "a mount prefix must not contain an empty segment";
                return false;
            }

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

            /*  The first entry only. RANGE carries one per argument and every
                node this engine publishes has one value; a multi-argument node
                keeps its type tags and loses the bounds of arguments two
                onwards, which is honest about what we can represent. */
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

        /*  The four declarations of PRD §3.3, taken from the mount unless the
            file overrides them.

            A namespace file MAY carry a GODOT key, and that is the whole point
            of §3.22: a hand-written template can declare what a captured one
            can only imply, and the engine cannot tell the two apart. */
        void applyGodot (const json::Value& node, const MountDeclaration& mount, Node& out)
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

            if (const auto* panic = property (*godot, "PANIC"); panic != nullptr)
                out.panic = panic->asString();
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

        void collect (const json::Value& node, const std::string& inside,
                      const std::string& rootPath, const MountDeclaration& mount,
                      std::vector<Node>& out, std::vector<std::string>& problems)
        {
            const auto address = mount.prefix + inside;

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
            applyGodot (node, mount, built);

            /*  VALUE is read for the kind inference above and then dropped.
                A captured description says what the target happened to be doing
                when somebody pointed a browser at it, and PRD §4.10 keeps that
                out of anything Go.dot treats as known. A mounted node has no
                value until something writes one. */
            built.values.clear();

            out.push_back (std::move (built));

            if (! hasChildren)
                return;

            for (const auto& member : contents->asObject())
            {
                const auto& name = member.first;

                if (name.empty() || name.find ('/') != std::string::npos)
                {
                    problems.push_back (address + ": \"" + name + "\" is not a usable node name");
                    continue;
                }

                collect (member.second, inside + "/" + name, rootPath, mount, out, problems);
            }
        }
    }

    //==============================================================================
    MountResult MountResult::failed (std::string problem)
    {
        MountResult result;
        result.problems.push_back (std::move (problem));
        return result;
    }

    //==============================================================================
    MountResult readNamespace (const MountDeclaration& mount, std::string_view jsonText)
    {
        std::string why;

        if (! prefixIsUsable (mount.prefix, why))
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

        MountResult result;
        collect (*parsed.value, {}, rootPath, mount, result.nodes, result.problems);

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
            return result;
        }

        mounts[mount.id] = Entry { mount, result.nodes };
        return result;
    }

    bool MountTable::unload (const std::string& mountId)
    {
        return mounts.erase (mountId) > 0;
    }

    void MountTable::clear()
    {
        mounts.clear();
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
            be reported against the mount somebody meant. */
        for (const auto& [id, entry] : mounts)
        {
            const auto& prefix = entry.declaration.prefix;

            if (address.size() > prefix.size()
                  && address.compare (0, prefix.size(), prefix) == 0
                  && address[prefix.size()] == '/')
                return id;
        }

        return {};
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

    MountTable::WriteResult MountTable::write (const std::string& address, const osc::Value& value)
    {
        auto* node = findNode (address);

        if (node == nullptr)
            return { false, reason::badAddress, {}, {} };

        if (node->access != Access::write && node->access != Access::readWrite)
            return { false, reason::readOnly, {}, {} };

        if (node->typeTags.empty())
            return { false, reason::typeMismatch, {}, {} };

        const auto coerced = CommandRegistry::coerceToTag (node->typeTags.front(), value);

        if (! coerced.has_value())
            return { false, reason::typeMismatch, {}, {} };

        /*  It lands here and goes no further. There is no transport in Phase 1,
            and that is the whole extent of what a stub does NOT do - the value
            is in the tree, the event is in the log, and a replay reproduces
            both. Phase 2 puts a socket after this line. */
        node->values = { *coerced };

        /*  IT LANDS HERE AND STOPS HERE, still. What goes on the wire is a
            MountSender's business and the caller's to arrange - this class
            names no socket, which is what lets every rule above be tested
            against a string literal. The mount id and the coerced value are
            handed back so the caller has both without looking anything up. */
        return { true, {}, mountOf (address), *coerced };
    }

    const osc::Value* MountTable::valueOf (const std::string& address) const
    {
        auto* self = const_cast<MountTable*> (this);
        const auto* node = self->findNode (address);

        return (node != nullptr && ! node->values.empty()) ? &node->values.front() : nullptr;
    }
}
