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
        return {};
    }

    std::string_view ShowDocument::ownerForElement (std::string_view element)
    {
        /*  A Group is a Cue (PRD §3.6), so both are addressed as `cue` — a
            client that has an identifier does not have to know which it got,
            and a cue that becomes a group keeps its address. */
        if (element == "Cue" || element == "Group") return "cue";
        if (element == "List")                      return "list";
        if (element == "Mount")                     return "mount";
        if (element == "Show")                      return "document";
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

        if (owner == "document")
        {
            if (parts.size() != 3)
                return out;

            node = showNode;
            attributeName = parts[2];
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

        parent.removeChild (node, nullptr);

        for (const auto& released_id : released)
            registry.release (released_id);

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
