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

#pragma once

/*
    The show, in memory: what someone decided (PRD §4.10).

    A juce::ValueTree, because PRD §3.20 says so and means it — document, undo
    and diff then share one substrate, and Phase 5's per-domain undo histories
    port from WFS-DIY rather than being invented. What this class adds around it
    is the discipline the raw type does not have:

      * every value is TYPED. juce::var will happily hold the string "1" where
        an integer belongs, and ValueTree's own comparison then says the two are
        equal — so a typed write over a string-typed property is silently
        dropped. That is a live defect in WFS-DIY (its harness lists it), and it
        is why nothing here goes near ValueTree::fromXml and why every write
        lands through one function.
      * every value is CHECKED against the schema before it lands. There is no
        path that writes an unknown attribute or an out-of-range number.
      * every object is IDENTIFIED, and the registry knows which identifiers are
        in use.
      * a child is built completely before it is added, so a listener never sees
        a half-made object with no identifier.

    NO UNDO MANAGER, deliberately, until Phase 5. Two reasons, both measured:
    with one attached, ValueTree::setProperty compares via var::equals, where
    "1" == 1, which reintroduces the silent-drop above; and UndoManager stamps
    each transaction with Time::getCurrentTime(), which would put a wall-clock
    read inside the tick thread's apply path and make a replay depend on when it
    ran. The write choke point takes an UndoManager* from the start and is
    handed nullptr until that phase arrives.

    THREADING: none of its own. The engine's tick thread owns this object and is
    its only writer and only direct reader; server threads read a published
    snapshot instead. See Engine.h.
*/

#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Schema.h>

#include <juce_data_structures/juce_data_structures.h>

#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    /*  The outcome of anything that changes the document. `reason` is a code
        from wfg::reason when it failed, so a command can return it unchanged
        and the log records the same word every time. */
    struct EditResult
    {
        bool ok = false;
        std::string reason;
        std::string id;          ///< the object created or touched, when there is one

        static EditResult failed (std::string reasonCode)
        {
            EditResult r;
            r.reason = std::move (reasonCode);
            return r;
        }

        static EditResult succeeded (std::string objectId = {})
        {
            EditResult r;
            r.ok = true;
            r.id = std::move (objectId);
            return r;
        }
    };

    /*  Where an address points. Produced by resolve(); the writer and the reader
        both go through it so that "/godot/cue/K7Q2M9X4/name" means exactly one
        thing. */
    struct Resolved
    {
        juce::ValueTree node;
        const Attribute* attribute = nullptr;

        /*  The address names a real node that the document does not hold: a
            derived value, computed from the structure and published read-only
            by the parameter tree. Writing to one is refused as read-only,
            because the address is not what is wrong with the request; reading
            one from the document gives nothing, because the document genuinely
            does not have it. */
        bool isDerived = false;

        bool isValid() const noexcept { return node.isValid() && attribute != nullptr; }
    };

    class ShowDocument
    {
    public:
        /** An empty show: a root, an empty Lists and an empty Mounts. Every
            attribute at its default, which the canonical writer then omits. */
        ShowDocument();

        //======================================================================
        // Structure
        //======================================================================

        /*  `id` may be empty, in which case one is generated and returned in the
            result. That is what makes replay work without randomness: the engine
            logs the identifier it produced, and replaying the log supplies it. */
        EditResult createList (const std::string& name, const std::string& id = {});

        /** `kind` is "memo" or "group"; a group is a cue that holds cues. */
        EditResult createCue (const std::string& parentId, int index,
                              const std::string& kind, const std::string& name,
                              const std::string& id = {});

        EditResult createMount (const std::string& prefix, const std::string& namespaceFile,
                                const std::string& id = {});

        /** Removes the object and everything under it, releasing identifiers. */
        EditResult remove (const std::string& id);

        /** Moves an object to a new parent and index. An index past the end
            appends; a negative index is refused rather than clamped, because it
            usually means the caller computed it wrong. */
        EditResult move (const std::string& id, const std::string& newParentId, int newIndex);

        //======================================================================
        // Values
        //======================================================================

        /** Parses `text` against the schema and writes it. The single write
            path: nothing else in the engine touches a property. */
        EditResult setAttribute (const std::string& address, std::string_view text);

        /** The attribute's value as canonical text, or nullopt if the address
            does not resolve. Returns the default when the attribute is absent,
            because an absent attribute IS its default — that is what lets the
            writer omit it. */
        std::optional<std::string> getAttribute (const std::string& address) const;

        /*  `/godot/<owner>/<id>/<attribute>`, or `/godot/document/<attribute>`
            for the root. Owner words are the parameter table's: document, list,
            cue, mount. `cue` resolves to a Cue or a Group, since a Group is a
            Cue. */
        Resolved resolve (const std::string& address) const;

        //======================================================================
        // Lookup
        //======================================================================

        juce::ValueTree findById (std::string_view id) const;
        juce::ValueTree root() const noexcept { return showNode; }

        IdRegistry& ids() noexcept { return registry; }
        const IdRegistry& ids() const noexcept { return registry; }

        /** Replaces the whole document, taking over its identifiers. Used by
            the reader; nothing else should need it. */
        void adopt (juce::ValueTree newRoot, IdRegistry newRegistry);

        //======================================================================
        /*  Checks the whole tree against the schema: unknown elements and
            attributes, values that do not parse, duplicate or malformed
            identifiers, children where they are not allowed. Returns one
            message per problem, in document order, each naming the element it
            is about.

            Used by the reader on load and available on demand; a document that
            fails this is never handed to the engine. */
        std::vector<std::string> validate() const;

        //======================================================================
        /** The element name for a `kind` value, or empty. "memo" is a Cue,
            "group" is a Group. */
        static std::string_view elementForKind (std::string_view kind);

        /** The parameter table's owner word for an element, for addressing:
            Cue and Group are both `cue`. */
        static std::string_view ownerForElement (std::string_view element);

    private:
        EditResult insertObject (juce::ValueTree parent, int index,
                                 std::string_view elementName,
                                 const std::string& id,
                                 const std::vector<std::pair<std::string_view, std::string>>& attributes);

        void collectIds (const juce::ValueTree& node, std::vector<std::string>& out) const;

        juce::ValueTree showNode;
        IdRegistry registry;
    };
}
