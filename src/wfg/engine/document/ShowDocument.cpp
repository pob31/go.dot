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

#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <unordered_set>

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idProperty { "id" };
        const juce::Identifier standbyProperty { "standby" };

        /*  Whether a value is a legal standby for this list: one of THAT list's
            own immediate children, or nothing at all.

            Named for the question rather than for the shape, because
            cue::isTopLevelChild answers the shape question and answers it
            differently - the empty string is not a top-level child of anything,
            but it IS a legal standby. Two same-named predicates disagreeing on
            the empty string is a trap, so only one of them carries that name. */
        bool isLegalStandbyFor (const juce::ValueTree& list, const std::string& cueId)
        {
            if (cueId.empty())
                return true;             // empty is the resting value, always legal

            for (const auto& child : list)
                if (child.hasProperty (idProperty)
                    && child[idProperty].toString().toStdString() == cueId)
                    return true;

            return false;
        }

        /*  The identifier after `cueId` among a container's children, or empty
            when it is the last one or is not there. */
        std::string siblingAfter (const juce::ValueTree& container, const std::string& cueId)
        {
            bool found = false;

            for (const auto& child : container)
            {
                if (! child.hasProperty (idProperty))
                    continue;

                const auto childId = child[idProperty].toString().toStdString();

                if (found)
                    return childId;

                if (childId == cueId)
                    found = true;
            }

            return {};
        }

        /*  THE ONE PLACE a typed value becomes a juce::var, and the reason this
            function exists rather than being inlined at three call sites.

            juce::var has no separate integer and string identity as far as
            ValueTree's change detection is concerned: var("1") == var(1) is
            true, so writing a typed 1 over a string "1" looks like "no change"
            and is dropped. Every value in this document is therefore written
            through here, from text that the schema has already parsed, so the
            var's type is decided by the schema and never by what the text
            happened to look like. */
        juce::var toVar (const Value& value)
        {
            switch (value.type())
            {
                case ValueType::string:    return juce::var (juce::String (value.getString()));
                case ValueType::integer:
                case ValueType::integer64: return juce::var (static_cast<juce::int64> (value.getInteger()));
                case ValueType::number:    return juce::var (value.getNumber());
                case ValueType::boolean:   return juce::var (value.getBoolean());
                case ValueType::blob:      break;
            }

            return {};
        }

        /*  A var back to canonical text. Not var::toString(): that would write a
            double through JUCE's formatter, which loses 46% of doubles to a
            round trip (measured; see osc/OscValue.cpp). Numbers go through the
            schema's formatter, the same one the event log uses. */
        std::string toText (const Attribute& attribute, const juce::var& value)
        {
            switch (attribute.type())
            {
                case ValueType::string:
                    return value.toString().toStdString();

                case ValueType::integer:
                case ValueType::integer64:
                    return std::to_string (static_cast<long long> (value));

                case ValueType::boolean:
                    return static_cast<bool> (value) ? "true" : "false";

                case ValueType::number:
                    return osc::formatDouble (static_cast<double> (value));

                case ValueType::blob:
                    break;
            }

            return {};
        }

        std::vector<std::string> splitAddress (const std::string& address)
        {
            std::vector<std::string> parts;
            std::size_t i = 0;

            while (i < address.size())
            {
                while (i < address.size() && address[i] == '/')
                    ++i;

                const auto start = i;

                while (i < address.size() && address[i] != '/')
                    ++i;

                if (i > start)
                    parts.push_back (address.substr (start, i - start));
            }

            return parts;
        }
    }

    //==============================================================================
    std::string_view ShowDocument::elementForKind (std::string_view kind)
    {
        if (kind == "memo")  return "Cue";
        if (kind == "group") return "Group";
        if (kind == "media") return "Media";
        if (kind == "fade")  return "Fade";
        if (kind == "stop")  return "Stop";
        if (kind == "osc")   return "Osc";
        return {};
    }

    std::string_view ShowDocument::ownerForElement (std::string_view element)
    {
        /*  A Group is a Cue (PRD §3.6), so both are addressed as `cue` — a
            client that has an identifier does not have to know which it got,
            and a cue that becomes a group keeps its address. */
        if (element == "Cue" || element == "Group" || element == "Media"
              || element == "Fade" || element == "Stop"
              || element == "Osc")                                  return "cue";
        if (element == "Route")                     return "route";
        if (element == "List")                      return "list";
        if (element == "Mount")                     return "mount";
        if (element == "Bus")                       return "bus";
        if (element == "Show")                      return "document";
        if (element == "Audio")                     return "audio";
        return {};
    }

    //==============================================================================
    ShowDocument::ShowDocument()
        : showNode (juce::Identifier (juce::String (std::string (Schema::rootElement)))),
          registry (IdRegistry::withSystemEntropy())
    {
        /*  The containers exist from the start, empty. A show with no lists
            still has a Lists element, so the file has an obvious place to put
            the first one and a diff that adds a list touches one line rather
            than three. */
        showNode.addChild (juce::ValueTree ("Lists"), -1, nullptr);
        showNode.addChild (juce::ValueTree ("Mounts"), -1, nullptr);

        /*  Audio is a container like the other two, but unlike them it carries
            a value of its own, and that value has no default: `tracks` is the
            polyphony ceiling and every show has to state it. A fresh document
            says zero, which is a real answer - a show with no audio - and not
            a placeholder standing in for one. */
        juce::ValueTree audio { "Audio" };
        audio.setProperty ("tracks", 0, nullptr);
        showNode.addChild (audio, -1, nullptr);
    }

    void ShowDocument::adopt (juce::ValueTree newRoot, IdRegistry newRegistry)
    {
        showNode = std::move (newRoot);
        registry = std::move (newRegistry);
    }

    //==============================================================================
    juce::ValueTree ShowDocument::findById (std::string_view id) const
    {
        if (id.empty())
            return {};

        const juce::String wanted { juce::String (std::string (id)) };

        /*  Depth-first over the whole tree. Linear, and deliberately so: a show
            holds thousands of objects, not millions, and an index would be a
            second source of truth to keep correct across every structural edit.
            If this ever shows up in a profile, the fix is a cache invalidated in
            one place, not a second map maintained in five. */
        struct Search
        {
            static juce::ValueTree find (const juce::ValueTree& node, const juce::String& target)
            {
                if (node.hasProperty (idProperty) && node[idProperty].toString() == target)
                    return node;

                for (const auto& child : node)
                    if (auto found = find (child, target); found.isValid())
                        return found;

                return {};
            }
        };

        return Search::find (showNode, wanted);
    }

    //==============================================================================
    Resolved ShowDocument::resolve (const std::string& address) const
    {
        Resolved out;

        const auto parts = splitAddress (address);

        // /godot/<owner>/<id>/<attribute>, or /godot/document/<attribute>.
        if (parts.size() < 3 || parts[0] != "godot")
            return out;

        const auto& owner = parts[1];

        juce::ValueTree node;
        std::string attributeName;

        if (owner == "document" || owner == "audio")
        {
            if (parts.size() != 3)
                return out;

            /*  The two owners that name an element rather than an object.
                `document` is the root itself; `audio` is the one container
                that carries attributes, because the track count is a fact
                about the whole show and not about any bus in it. Neither has
                an identifier to look up, so neither takes the id path. */
            node = owner == "document" ? showNode : showNode.getChildWithName ("Audio");
            attributeName = parts[2];

            if (! node.isValid())
                return out;
        }
        else
        {
            if (parts.size() != 4)
                return out;

            node = findById (parts[2]);
            attributeName = parts[3];

            if (! node.isValid())
                return out;

            /*  The owner word has to match what was actually found, so that
                /godot/list/<a cue's id>/name does not quietly address the cue.
                An identifier alone is unambiguous; the word is there for the
                reader, and a reader that can be wrong is worse than no reader. */
            if (ownerForElement (node.getType().toString().toStdString()) != owner)
                return out;
        }

        const auto element = node.getType().toString().toStdString();
        out.attribute = Schema::instance().attribute (element, attributeName);

        if (out.attribute == nullptr)
        {
            /*  Not an attribute of the document - but it may still be a node
                the parameter tree publishes, computed from the structure. Say
                which, so a refusal can name the real reason. */
            if (const auto* schemaElement = Schema::instance().element (element))
            {
                out.attribute = schemaElement->derivedAttribute (attributeName);
                out.isDerived = out.attribute != nullptr;
            }
        }

        if (out.attribute != nullptr)
            out.node = node;

        return out;
    }

    //==============================================================================
    EditResult ShowDocument::setAttribute (const std::string& address, std::string_view text)
    {
        auto target = resolve (address);

        if (! target.isValid())
            return EditResult::failed (reason::badAddress);

        /*  A derived value is read-only by construction, whatever its row
            says: it is computed from the structure, so there is nowhere to put
            a written one and the structure is what would have to change. */
        if (target.isDerived || target.attribute->access() == Access::read)
            return EditResult::failed (reason::readOnly);

        Value value;
        const auto parsed = Schema::parseValue (*target.attribute, text, value);

        if (! parsed.ok)
            return EditResult::failed (reason::typeMismatch);

        /*  ONE REFERENTIAL INVARIANT, and it is named rather than generalised.

            A list's standby must name one of that list's own top-level children
            or be empty. The schema can say a value is a string in range; it has
            no way to say a value must be the identifier of a child of the
            element carrying it, and adding a referential column to the
            parameter table for a single attribute would be building the
            generalisation before there are two cases to generalise. Phase 3's
            run pointer is the second case; that is when the column earns
            itself.

            IT LIVES HERE because here is the only door. The standby commands,
            a client's node.set and EphemeralState restoring a saved show all
            arrive through setAttribute, and a check anywhere else would guard
            one of those three and miss the other two - the load path in
            particular, which is where a state file written against a different
            show shows up. */
        if (target.attribute->name() == "standby"
            && target.node.getType().toString() == "List"
            && ! isLegalStandbyFor (target.node, value.getString()))
            return EditResult::failed (reason::notInList);

        /*  nullptr is the UndoManager, and it stays nullptr until Phase 5 — see
            the header for the two measured reasons. */
        target.node.setProperty (juce::Identifier (juce::String (std::string (target.attribute->name()))),
                                 toVar (value), nullptr);

        return EditResult::succeeded (target.node[idProperty].toString().toStdString());
    }

    std::optional<std::string> ShowDocument::getAttribute (const std::string& address) const
    {
        const auto target = resolve (address);

        if (! target.isValid())
            return std::nullopt;

        /*  The document does not hold a derived value - that is what derived
            means - so it says so rather than handing back a default that would
            look like an answer. The parameter tree computes these. */
        if (target.isDerived)
            return std::nullopt;

        const juce::Identifier property { juce::String (std::string (target.attribute->name())) };

        /*  An absent attribute IS its default. That equivalence is what lets the
            writer omit defaults and still round-trip, so it has to hold here
            too, not only in the writer. */
        if (! target.node.hasProperty (property))
            return std::string (target.attribute->defaultText());

        return toText (*target.attribute, target.node[property]);
    }

    //==============================================================================
    EditResult ShowDocument::insertObject (juce::ValueTree parent, int index,
                                           std::string_view elementName,
                                           const std::string& id,
                                           const std::vector<std::pair<std::string_view, std::string>>& attributes)
    {
        const auto* element = Schema::instance().element (elementName);

        if (element == nullptr || ! parent.isValid())
            return EditResult::failed (reason::badAddress);

        const auto* parentElement = Schema::instance().element (parent.getType().toString().toStdString());

        if (parentElement == nullptr || ! parentElement->mayContain (elementName))
            return EditResult::failed (reason::badAddress);

        if (index < 0)
            return EditResult::failed (reason::badAddress);

        std::string objectId = id;

        if (objectId.empty())
        {
            objectId = registry.generate();

            if (objectId.empty())
                return EditResult::failed (reason::unknownId);
        }
        else if (! Id::isValid (objectId) || ! registry.reserve (objectId))
        {
            /*  A supplied identifier that is malformed or already taken. Both
                happen during replay if a log has been edited, and both must be
                refusals rather than a second object wearing the same name. */
            return EditResult::failed (reason::unknownId);
        }

        /*  Built complete, THEN added. A listener that saw the child appear and
            the identifier arrive afterwards would publish an object with no
            address — which is exactly what the parameter tree does on
            valueTreeChildAdded in Phase 1.5. */
        juce::ValueTree node { juce::Identifier (juce::String (std::string (elementName))) };
        node.setProperty (idProperty, juce::String (objectId), nullptr);

        for (const auto& [name, text] : attributes)
        {
            const auto* attribute = element->attribute (name);

            if (attribute == nullptr)
            {
                registry.release (objectId);
                return EditResult::failed (reason::badAddress);
            }

            Value value;

            if (! Schema::parseValue (*attribute, text, value).ok)
            {
                registry.release (objectId);
                return EditResult::failed (reason::typeMismatch);
            }

            node.setProperty (juce::Identifier (juce::String (std::string (name))),
                              toVar (value), nullptr);
        }

        parent.addChild (node, std::min (index, parent.getNumChildren()), nullptr);

        return EditResult::succeeded (objectId);
    }

    EditResult ShowDocument::createList (const std::string& name, const std::string& id)
    {
        return insertObject (showNode.getChildWithName ("Lists"),
                             showNode.getChildWithName ("Lists").getNumChildren(),
                             "List", id, { { "name", name } });
    }

    EditResult ShowDocument::createCue (const std::string& parentId, int index,
                                        const std::string& kind, const std::string& name,
                                        const std::string& id)
    {
        const auto elementName = elementForKind (kind);

        if (elementName.empty())
            return EditResult::failed (reason::typeMismatch);

        auto parent = findById (parentId);

        if (! parent.isValid())
            return EditResult::failed (reason::unknownId);

        /*  `kind` is a read-only attribute derived from the element, so it is
            not written: /godot/cue/<id>/kind reports "group" because the element
            is a Group, and a client cannot turn one into the other by writing
            to it. */
        return insertObject (parent, index, elementName, id, { { "name", name } });
    }

    EditResult ShowDocument::createRoute (const std::string& cueId,
                                          const std::string& busId,
                                          const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  Only a media cue has anywhere for a sound to go. Refusing here
            rather than in the grammar means the client is told which of its two
            identifiers was wrong, and told it at the moment it asked. */
        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (cue, cue.getNumChildren(), "Route", id,
                             { { "bus", busId } });
    }

    EditResult ShowDocument::createMount (const std::string& prefix,
                                          const std::string& namespaceFile,
                                          const std::string& id)
    {
        auto mounts = showNode.getChildWithName ("Mounts");

        return insertObject (mounts, mounts.getNumChildren(), "Mount", id,
                             { { "prefix", prefix }, { "namespace", namespaceFile } });
    }

    //==============================================================================
    void ShowDocument::collectIds (const juce::ValueTree& node, std::vector<std::string>& out) const
    {
        if (node.hasProperty (idProperty))
            out.push_back (node[idProperty].toString().toStdString());

        for (const auto& child : node)
            collectIds (child, out);
    }

    EditResult ShowDocument::remove (const std::string& id)
    {
        auto node = findById (id);

        if (! node.isValid())
            return EditResult::failed (reason::unknownId);

        auto parent = node.getParent();

        if (! parent.isValid())
            return EditResult::failed (reason::unknownId);

        /*  Every identifier under it comes back, not just its own — deleting a
            group deletes its cues, and leaving their identifiers reserved would
            slowly poison the registry over a long editing session. */
        std::vector<std::string> released;
        collectIds (node, released);

        /*  If the list this cue belongs to is parked on it, work out where the
            standby goes BEFORE the cue disappears - afterwards there is no
            sequence left to ask.

            It advances to the next remaining sibling rather than clearing
            (author, 2026-09-06): during tech, deleting the cue you are parked
            on should leave you parked on the next one. Done inside the applied
            command rather than as a second event, so a replay reproduces it for
            free and the log does not need a repair record nobody sent. */
        std::string repairList, repairStandby;

        if (parent.getType().toString() == "List"
            && parent[standbyProperty].toString().toStdString() == id)
        {
            repairList = parent[idProperty].toString().toStdString();
            repairStandby = siblingAfter (parent, id);
        }

        parent.removeChild (node, nullptr);

        for (const auto& released_id : released)
            registry.release (released_id);

        if (! repairList.empty())
            setAttribute ("/godot/list/" + repairList + "/standby", repairStandby);

        return EditResult::succeeded (id);
    }

    EditResult ShowDocument::move (const std::string& id, const std::string& newParentId, int newIndex)
    {
        auto node = findById (id);

        if (! node.isValid())
            return EditResult::failed (reason::unknownId);

        auto newParent = findById (newParentId);

        if (! newParent.isValid())
            return EditResult::failed (reason::unknownId);

        if (newIndex < 0)
            return EditResult::failed (reason::badAddress);

        const auto* parentElement =
            Schema::instance().element (newParent.getType().toString().toStdString());

        if (parentElement == nullptr
            || ! parentElement->mayContain (node.getType().toString().toStdString()))
            return EditResult::failed (reason::badAddress);

        /*  A group cannot be moved inside itself. Without this the tree stops
            being a tree: the subtree detaches with the node and is never seen
            again, and the identifiers in it stay reserved forever. */
        for (auto ancestor = newParent; ancestor.isValid(); ancestor = ancestor.getParent())
            if (ancestor == node)
                return EditResult::failed (reason::badAddress);

        auto oldParent = node.getParent();

        /*  A move WITHIN one list's top level is a reorder, and a reorder never
            moves the standby: it still names the same cue, which is still a
            top-level child, and PRD §3.5 is explicit that the pointer does not
            follow the shape of the list around.

            A move OUT of that top level is different. The cue is no longer
            somewhere the standby is allowed to point - into a group, or into
            another list entirely - so the list it left is cleared rather than
            advanced. Advancing would be guessing that the operator meant to
            stay where they were; clearing says plainly that what they were
            parked on has gone somewhere else. */
        const auto leavingTopLevel = oldParent != newParent
                                       && oldParent.getType().toString() == "List"
                                       && oldParent[standbyProperty].toString().toStdString() == id;

        const auto vacatedList = leavingTopLevel
                                   ? oldParent[idProperty].toString().toStdString()
                                   : std::string {};

        if (oldParent == newParent)
        {
            const auto from = newParent.indexOf (node);
            const auto to = std::min (newIndex, newParent.getNumChildren() - 1);
            newParent.moveChild (from, to, nullptr);
        }
        else
        {
            oldParent.removeChild (node, nullptr);
            newParent.addChild (node, std::min (newIndex, newParent.getNumChildren()), nullptr);
        }

        if (! vacatedList.empty())
            setAttribute ("/godot/list/" + vacatedList + "/standby", "");

        return EditResult::succeeded (id);
    }

    //==============================================================================
    std::vector<std::string> ShowDocument::validate() const
    {
        std::vector<std::string> problems;
        std::unordered_set<std::string> seenIds;

        const auto& schema = Schema::instance();

        struct Walk
        {
            const Schema& schema;
            std::vector<std::string>& problems;
            std::unordered_set<std::string>& seenIds;

            void visit (const juce::ValueTree& node, const std::string& path)
            {
                const auto elementName = node.getType().toString().toStdString();
                const auto* element = schema.element (elementName);

                if (element == nullptr)
                {
                    problems.push_back (path + ": unknown element <" + elementName + ">");
                    return;
                }

                std::string here = path + "/" + elementName;

                if (element->hasIdentity)
                {
                    if (! node.hasProperty (idProperty))
                    {
                        problems.push_back (here + ": missing id");
                    }
                    else
                    {
                        const auto id = node[idProperty].toString().toStdString();
                        here += "[" + id + "]";

                        if (! Id::isValid (id))
                            problems.push_back (here + ": malformed id");
                        else if (! seenIds.insert (id).second)
                            problems.push_back (here + ": duplicate id");
                    }
                }
                else if (node.hasProperty (idProperty))
                {
                    problems.push_back (here + ": <" + elementName + "> may not carry an id");
                }

                for (int i = 0; i < node.getNumProperties(); ++i)
                {
                    const auto name = node.getPropertyName (i);

                    if (name == idProperty)
                        continue;

                    const auto attributeName = name.toString().toStdString();
                    const auto* attribute = element->attribute (attributeName);

                    if (attribute == nullptr)
                    {
                        problems.push_back (here + ": unknown attribute \"" + attributeName + "\"");
                        continue;
                    }

                    /*  Re-parse the value as text. That catches a property whose
                        var carries the wrong type as well as one out of range -
                        which is the whole point, because a string-typed "1" is
                        exactly what a careless loader leaves behind. */
                    Value parsedValue;
                    const auto text = toText (*attribute, node[name]);
                    const auto parsed = Schema::parseValue (*attribute, text, parsedValue);

                    if (! parsed.ok)
                        problems.push_back (here + ": \"" + attributeName + "\" " + parsed.error);
                }

                for (const auto& child : node)
                {
                    const auto childName = child.getType().toString().toStdString();

                    if (! element->mayContain (childName))
                        problems.push_back (here + ": <" + elementName + "> may not contain <"
                                            + childName + ">");

                    visit (child, here);
                }
            }
        };

        if (showNode.getType().toString() != juce::String (std::string (Schema::rootElement)))
        {
            problems.push_back ("the root element is <" + showNode.getType().toString().toStdString()
                                + ">, expected <" + std::string (Schema::rootElement) + ">");
            return problems;
        }

        Walk { schema, problems, seenIds }.visit (showNode, "");
        return problems;
    }
}
