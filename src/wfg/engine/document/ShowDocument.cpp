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

#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/osc/OscValue.h>

#include <map>
#include <cctype>
#include <algorithm>
#include <unordered_set>

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idProperty { "id" };
        const juce::Identifier standbyProperty { "standby" };
        const juce::Identifier lockedProperty { "locked" };

        /*  Whether a value is a legal standby for this list: somewhere on its
            MANUAL PATH, or nothing at all.

            Named for the question rather than for the shape, because
            cue::isOnManualPath answers the shape question and answers it
            differently on one input - the empty string is nowhere at all, and
            it IS a legal standby, because an empty pointer is a resting state
            (§3.5) rather than a failure. Two same-named predicates disagreeing
            on the empty string is a trap, so only one of them carries the name. */
        bool isLegalStandbyFor (const juce::ValueTree& list, const std::string& cueId)
        {
            if (cueId.empty())
                return true;             // empty is the resting value, always legal

            /*  THE MANUAL PATH, not the top level, since PR 3.4.

                PRD §3.6 puts the pointer INSIDE a manual sequence group - "the
                operator is the parent" - so a member of one is a legal place to
                stand. A member of a timeline or an automatic group is not: the
                machine advances those, and a pointer the machine also moves is
                how an operator presses GO expecting cue 12 and gets 14 (§3.5).

                It asks `cue::isOnManualPath`, which is the same walk the cursor
                takes, so the pointer cannot be PUT anywhere `next` could not
                have carried it. Two answers to that question would eventually
                be two different answers. */
            return cue::isOnManualPath (list, cueId);
        }

        /*  The identifier after `cueId` among a container's children, or empty
            when it is the last one or is not there. */
        /*  The list a cue belongs to, however deep it is - or an invalid tree
            when it is not in one. The standby repairs below need it because a
            cue can now be several levels down. */
        juce::ValueTree listContaining (juce::ValueTree node)
        {
            while (node.isValid() && node.getType().toString() != "List")
                node = node.getParent();

            return node;
        }

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
        if (kind == "midi")  return "Midi";
        return {};
    }

    std::string_view ShowDocument::containerSegmentFor (std::string_view element)
    {
        /*  THE ELEMENTS THAT ARE ADDRESSED WITHOUT AN IDENTIFIER, because there
            is only one of each: `/godot/document/name`, `/godot/audio/tracks`,
            `/godot/list/focus`. A container carries facts about the collection
            rather than about any member of it, and a collection has no id to
            look it up by.

            THE SEGMENT STAYS SINGULAR for `Lists`, and that is deliberate
            rather than an inconsistency: `/godot/list/<id>/standby` and
            `/godot/list/focus` are one container read two ways, and a client
            walking the tree should not have to learn that one of them is
            spelled differently. The parameter table's owner token is `lists`
            because a table row belongs to an element; the address is what a
            person types. */
        if (element == "Show")  return "document";
        if (element == "Audio") return "audio";
        if (element == "Lists") return "list";
        return {};
    }

    std::string_view ShowDocument::ownerForElement (std::string_view element)
    {
        /*  A Group is a Cue (PRD §3.6), so both are addressed as `cue` — a
            client that has an identifier does not have to know which it got,
            and a cue that becomes a group keeps its address. */
        if (element == "Cue" || element == "Group" || element == "Media"
              || element == "Fade" || element == "Stop"
              || element == "Osc" || element == "Midi")             return "cue";
        if (element == "Lists")                     return "lists";
        /*  A header and a footer are addressed by nothing: they carry no
            attribute but their identifier, and the cues inside them are
            addressed as cues like any other. Naming an owner for them would be
            promising a `/godot/header/<id>/…` that has nothing in it. */
        if (element == "Header" || element == "Footer" || element == "Persistent")
            return {};
        if (element == "Route")                     return "route";
        if (element == "Range")                     return "range";
        if (element == "Port")                      return "port";
        if (element == "Trigger")                   return "trigger";
        if (element == "List")                      return "list";
        if (element == "Mount")                     return "mount";
        if (element == "Bus")                       return "bus";
        if (element == "Show")                      return "document";
        if (element == "Audio")                     return "audio";

        /*  A SLOT AND A RACK CHANNEL ANSWER DIFFERENTLY, and they have to: this
            is the by-kind half of the `refers` check, so `feed/@slot` naming a
            rack channel and `insert/@channel` naming a processor input are both
            caught here rather than at run time. They share the owner `slot` for
            the rows they have in common; what they do not share is the word
            this returns. */
        if (element == "Slot")                      return "slot";
        if (element == "Channel")                   return "rackChannel";
        if (element == "Feed")                      return "feed";
        if (element == "Insert")                    return "insert";

        /*  `Rack` is addressed by nothing, like `Mounts` and like a header: a
            container that holds channels and carries nothing of its own. */
        if (element == "Rack")                      return {};

        return {};
    }

    std::string_view ShowDocument::addressOwnerFor (std::string_view element)
    {
        /*  The one place the two answers differ, and the reason is in the
            header: a rack channel and a processor input share an address space
            and not a kind. Everything else is addressed under its own owner. */
        if (element == "Channel")                   return "slot";

        return ownerForElement (element);
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

        makeHistories();

        /*  LAST, so that building the empty containers above does not count as
            three changes to a document nobody has opened yet. A fresh document
            is at revision 1 and stays there until somebody writes something. */
        showNode.addListener (this);
    }

    ShowDocument::ShowDocument (ShowDocument&& other)
        : showNode (juce::ValueTree()), registry (IdRegistry::withSystemEntropy())
    {
        *this = std::move (other);
    }

    ShowDocument& ShowDocument::operator= (ShowDocument&& other)
    {
        if (this == &other)
            return *this;

        showNode.removeListener (this);
        other.showNode.removeListener (&other);

        showNode = std::move (other.showNode);
        registry = std::move (other.registry);
        changeCount = other.changeCount;
        showChangeCount = other.showChangeCount;

        /*  REBUILT EMPTY, NOT CARRIED. The header argues it; what it costs here
            is one line, and what a carried history would have cost is a stack of
            actions holding ref-counted handles into a tree that has just changed
            owner. The coalescing key goes with them: a window measured against a
            write made by the document this one was moved from would be a window
            about somebody else's gesture. */
        makeHistories();
        forgetCoalescing();
        undoSuppressions = 0;

        showNode.addListener (this);
        return *this;
    }

    void ShowDocument::makeHistories()
    {
        for (auto& domain : histories)
            domain = std::make_unique<juce::UndoManager>();
    }

    void ShowDocument::forgetCoalescing() noexcept
    {
        lastWriteAddress.clear();
        lastWriteOrigin.clear();
        lastWriteTick = 0;
    }

    ShowDocument::~ShowDocument()
    {
        showNode.removeListener (this);
    }

    void ShowDocument::adopt (juce::ValueTree newRoot, IdRegistry newRegistry)
    {
        /*  A HATCH, NOT A DOOR, and the edit lock is deliberately not asked
            here.

            This is the fifth writer of the document and it goes through none
            of the four doors: it is how `Bundle::open` loads a show, and it
            will be how `document.revert` and `document.recover` replace one.
            It does not edit the show, it swaps it - and because `locked` is an
            attribute of the root it swaps THE LOCK too, for whatever the
            loaded state.xml says. A revert of a bundle saved unlocked would
            silently unlock a locked session.

            Guarding it here would be wrong twice. The load that opens a
            locked bundle comes through here with nothing locked yet (the lock
            arrives a moment later, restored from state.xml through
            `setAttribute`), so a guard would never fire when it mattered and
            would refuse the loads that are not edits at all. And a guard that
            did fire would leave the caller holding a half-opened bundle. So
            the refusal belongs to the COMMANDS that open this hatch on a live
            show - revert and recover check `isLocked()` in their handlers,
            before they read a byte - and the hatch itself stays what it is: a
            replacement of the show, lock included (namespace draft §14.11). */

        /*  The listener follows the document. A listener left on the tree that
            was just replaced would report nothing (nobody writes to it any
            more) and would outlive nothing, but it would also mean the NEW tree
            is unwatched - so every edit after a load would be invisible to
            `revision()`, and a cache built before the load would look current
            for ever. */
        /*  AND THE ONE PLACE A SUPPRESSION EARNS ITS KEEP, today. Nothing under
            here writes through a door, so what the scope buys is the rule rather
            than a behaviour: a hatch is not an edit, and anything a later phase
            adds inside it is not one either. */
        const ScopedUndoSuppression suppressed { *this };

        /*  THE HISTORY GOES WITH THE SHOW IT IS ABOUT. Every action on the stack
            holds a ref-counted handle into the graph that is about to be
            replaced, so an uncleared stack would keep the previous show alive in
            memory and undo into a tree nobody can see. Cleared BEFORE the swap,
            so the old show's last handles are dropped while it is still the
            show. */
        for (auto& domain : histories)
            domain->clearUndoHistory();

        forgetCoalescing();

        showNode.removeListener (this);

        showNode = std::move (newRoot);
        registry = std::move (newRegistry);

        showNode.addListener (this);

        /*  A load is the largest change there is - to the show half as much as
            to the whole, which is why a verb that has just opened a bundle
            stamps its session AFTER the load rather than before it. */
        bumpStructure();
    }

    void ShowDocument::valueTreePropertyChanged (juce::ValueTree& node,
                                                 const juce::Identifier& property)
    {
        ++changeCount;

        /*  WHICH FILE THIS VALUE LIVES IN, asked of the same column the two
            writers ask (CanonicalXml.cpp and EphemeralState.cpp both filter on
            `persist`), so the dot and the files cannot disagree about what a
            save would write. Looked up by the element the property landed on,
            which is what makes a Group's inherited Cue rows answer as Cue rows
            do - Schema applies that rule once, and this reads its result.

            A row the schema does not know counts as show: see `showRevision()`
            for why guessing dirty is the safe way to be wrong. */
        const auto* attribute = Schema::instance().attribute (node.getType().toString().toStdString(),
                                                              property.toString().toStdString());

        if (attribute == nullptr || attribute->persist() == Persist::show)
            ++showChangeCount;
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
    juce::ValueTree ShowDocument::containerElementFor (std::string_view segment) const
    {
        if (segment == "document") return showNode;
        if (segment == "audio")    return showNode.getChildWithName ("Audio");
        if (segment == "list")     return showNode.getChildWithName ("Lists");
        return {};
    }

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

        /*  A CONTAINER ADDRESS HAS THREE PARTS, an object address has four, and
            the count is what tells them apart rather than the word. `list` is
            now BOTH - `/godot/list/focus` names the collection and
            `/godot/list/<id>/standby` names one of its members - so a branch on
            the owner word alone would have had to choose, and choosing would
            have meant spelling the container differently for no reason a client
            could see. */
        const auto container = parts.size() == 3 ? containerElementFor (owner)
                                                 : juce::ValueTree {};

        if (container.isValid())
        {
            /*  The elements that carry facts about a COLLECTION rather than
                about any member: the show itself, the audio rig's track count,
                and which list has the focus. None has an identifier, because
                there is only one of each. */
            node = container;
            attributeName = parts[2];
        }
        else
        {
            if (parts.size() == 3)
                return out;

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
            if (addressOwnerFor (node.getType().toString().toStdString()) != owner)
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
    bool ShowDocument::isLocked() const
    {
        /*  An absent attribute IS its default, and the row's default is
            false: a show nobody has locked is not locked.

            Read as the stored var rather than through `getAttribute`, because
            there is exactly one way a lock reaches the tree - `setAttribute`,
            from a client's write or from state.xml; the reader refuses a
            show.xml carrying one as engine state - and that door typed it as
            a boolean on the way in, never as the text "true". And because
            this is asked by every door, and need not parse an address to
            answer. */
        return static_cast<bool> (showNode[lockedProperty]);
    }

    std::optional<EditResult> ShowDocument::refuseIfLocked() const
    {
        if (! isLocked())
            return std::nullopt;

        return EditResult::failed (reason::locked);
    }

    //==============================================================================
    std::string_view undoDomainWord (UndoDomain domain) noexcept
    {
        /*  Spelled out rather than defaulted, so that adding Phase 6's domain
            to the enum and forgetting its word is a build failure here rather
            than an empty string a client cannot address. */
        switch (domain)
        {
            case UndoDomain::document: return "document";
        }

        return {};
    }

    std::optional<UndoDomain> undoDomainForWord (std::string_view word) noexcept
    {
        if (word == undoDomainWord (UndoDomain::document))
            return UndoDomain::document;

        return std::nullopt;
    }

    void ShowDocument::beginTransaction (const std::string& commandName,
                                         std::int64_t tick,
                                         const std::string& writeOrigin,
                                         const std::vector<osc::Value>& args)
    {
        /*  ONLY A `node.set` HAS AN ADDRESS TO COALESCE ON, and everything else
            leaves this empty - which is how a create, a delete, a move and an
            `undo` each break a run without needing a rule of their own. */
        const auto address = (commandName == "node.set" && ! args.empty() && args[0].isString())
                               ? args[0].getString()
                               : std::string {};

        /*  The address is tested FIRST and the arithmetic is behind it, so the
            tick difference is only ever computed against a tick a real write
            stamped. `tick >= lastWriteTick` is there for the caller that hands
            them out of order: a window measured backwards is not a window. */
        const auto joinsOpenTransaction = ! address.empty()
                                            && address == lastWriteAddress
                                            && writeOrigin == lastWriteOrigin
                                            && tick >= lastWriteTick
                                            && tick - lastWriteTick <= coalescingWindowTicks;

        if (! joinsOpenTransaction)
        {
            /*  EVERY DOMAIN, because which one a command writes to is the
                command's business and not this hook's, and naming a transaction
                on a history the command turns out not to touch costs nothing:
                `beginNewTransaction` sets a flag and a name and allocates
                nothing at all. An empty transaction never becomes a step. */
            for (auto& domain : histories)
                domain->beginNewTransaction (juce::String (commandName));
        }

        lastWriteAddress = address;
        lastWriteOrigin = writeOrigin;
        lastWriteTick = tick;
    }

    std::optional<std::string> ShowDocument::undo (UndoDomain domain)
    {
        auto& manager = *histories[static_cast<std::size_t> (domain)];

        if (! manager.canUndo())
            return std::nullopt;

        /*  READ BEFORE IT IS POPPED, because afterwards it names the transaction
            BEFORE the one that was taken back - and what the record has to carry
            is the name of the one this command actually unmade. */
        const auto name = manager.getUndoDescription().toStdString();

        if (! manager.undo())
            return std::nullopt;

        rebuildRegistry();
        forgetCoalescing();

        return name;
    }

    std::optional<std::string> ShowDocument::redo (UndoDomain domain)
    {
        auto& manager = *histories[static_cast<std::size_t> (domain)];

        if (! manager.canRedo())
            return std::nullopt;

        const auto name = manager.getRedoDescription().toStdString();

        if (! manager.redo())
            return std::nullopt;

        /*  A REDO REBUILDS THE REGISTRY TOO, and for the mirrored reason: redoing
            a delete takes a subtree out again, and the identifiers under it have
            to go back to being free or the show slowly runs out of names it is
            allowed to draw. */
        rebuildRegistry();
        forgetCoalescing();

        return name;
    }

    const juce::UndoManager& ShowDocument::history (UndoDomain domain) const noexcept
    {
        return *histories[static_cast<std::size_t> (domain)];
    }

    juce::UndoManager* ShowDocument::structuralHistory() noexcept
    {
        if (undoSuppressions > 0)
            return nullptr;

        return histories[static_cast<std::size_t> (UndoDomain::document)].get();
    }

    juce::UndoManager* ShowDocument::historyFor (const Attribute& attribute) noexcept
    {
        if (attribute.persist() != Persist::show)
            return nullptr;

        return structuralHistory();
    }

    void ShowDocument::rebuildRegistry()
    {
        std::vector<std::string> present;
        collectIds (showNode, present);

        registry.clear();

        for (const auto& id : present)
            registry.reserve (id);
    }

    ShowDocument::ScopedUndoSuppression::ScopedUndoSuppression (ShowDocument& documentToSuppress) noexcept
        : target (documentToSuppress)
    {
        ++target.undoSuppressions;
    }

    ShowDocument::ScopedUndoSuppression::~ScopedUndoSuppression()
    {
        --target.undoSuppressions;
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

        /*  THE EDIT LOCK, for the show half and only the show half.

            `persist == show` AND NOTHING ELSE - not `!= state`. A value that
            reaches this line is `show` or `state` (a `none` row is derived and
            was refused above), so today the two spellings agree; they stop
            agreeing the day a writable `none` row is resolved here, and the
            question this line asks is "would show.xml record it?", which is
            the one that stays right. The state half is where the operator is
            standing: the standby GO moves, the focus, and the lock itself,
            whose release has to get through or the lock could never be lifted.
            It is also what EphemeralState restores a bundle through, so a lock
            that refused it would make a locked bundle refuse its own lock.

            HERE, BESIDE THE READ-ONLY CHECK AND BEFORE THE PARSE, and the
            position is the point. Asked after the parse, a locked show would
            answer `type-mismatch` for a badly typed value and `locked` only
            for a well-typed one, sending an operator to look at their encoder
            when the answer was that the show is fixed. The address has to be
            resolved first, because which half a value lives in is a property
            of its row; everything after that is a question a locked show
            would refuse anyway. */
        if (target.attribute->persist() == Persist::show)
        {
            if (auto refusal = refuseIfLocked())
                return *refusal;
        }

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
        {
            /*  TWO REFUSALS, because they send somebody somewhere different -
                and this door has to give the same answer `standby.set` gives,
                or a client would learn one thing from the command and another
                from the node.

                `not-in-list`: that cue belongs somewhere else, or is not a cue.
                `not-manual-path`: it is in THIS list, inside a chain the
                MACHINE advances, and the remedy is to make the group manual or
                to park on the group instead. */
            const auto elsewhere = ! cue::isInList (target.node, value.getString());

            return EditResult::failed (elsewhere ? reason::notInList
                                                 : reason::notManualPath);
        }

        /*  THE HISTORY THE ROW BELONGS ON, which is the document's for a
            `persist == show` value and nothing at all for a state one: undo
            restores what someone decided, and where the operator is standing is
            not among those things (PRD §4.10). The manager is the THIRD
            argument here and the SECOND to the `removeChild` overload `remove`
            and `move` use, which is the detail a reader who learns "third" gets
            wrong in exactly one place. */
        target.node.setProperty (juce::Identifier (juce::String (std::string (target.attribute->name()))),
                                 toVar (value), historyFor (*target.attribute));

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
        /*  THE DOOR EVERY CREATE COMES THROUGH - the ten creates, and the
            header, footer and persistent section that are made on first ask.
            Asked before an identifier is drawn or reserved, so a refused
            create leaves the registry exactly as it found it.

            Two things follow from its being here and not in each create. The
            creates that answer with an existing child (`createRole`,
            `createPersistent`) return before they reach this line and are
            applied under the lock, which is right: they change nothing. And
            one create that changes the document on its way here has to ask
            for itself - see `createRackChannel`. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

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
            valueTreeChildAdded in Phase 1.5.

            AND THAT IS WHY THE WRITES BELOW STAY ON nullptr while the add
            further down takes the history. The node is not in the tree when they
            run, so an action recording them would hold a handle on an object
            nobody could reach; the one action at the add carries the whole
            finished object, identifier and all, and undoing it takes the object
            away entire. */
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

        parent.addChild (node, std::min (index, parent.getNumChildren()), structuralHistory());

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

    EditResult ShowDocument::createSlot (const std::string& mountId,
                                         const std::string& address,
                                         const std::string& id)
    {
        auto mount = findById (mountId);

        if (! mount.isValid())
            return EditResult::failed (reason::unknownId);

        /*  A slot belongs to the processor it is an input of. Declared on the
            mount rather than in a pool of its own, because "which of WFS-DIY's
            inputs this show uses" is a fact about that mount and moves with it
            when somebody re-points the show at another box. */
        if (mount.getType().toString() != "Mount")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (mount, mount.getNumChildren(), "Slot", id,
                             { { "address", address } });
    }

    EditResult ShowDocument::createRackChannel (const std::string& channelClass,
                                                const std::string& id)
    {
        /*  ASKED HERE AS WELL AS AT THE DOOR, because this is the one create
            that changes the document before it reaches `insertObject`: the
            `<Rack/>` below is made on demand and added to the tree first. A
            lock asked only at the door would let a locked show gain an empty
            rack, move `revision()` and `showRevision()` through the listener,
            light the dirty dot - and THEN refuse. A document changed by a
            refusal, in the phase whose subject is trusting the save.

            No other create has that shape: `createRole` and
            `createPersistent` answer with an existing child before they
            insert anything, and every other create only reads on its way to
            the door. A create added later that makes a container on demand
            needs this line too. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto audio = showNode.getChildWithName ("Audio");

        if (! audio.isValid())
            return EditResult::failed (reason::unknownId);

        /*  THE RACK IS MADE ON DEMAND, the way a group's header is: there is at
            most one, it carries nothing itself, and a show with no rack should
            not have an empty one in the file for the sake of it. */
        auto rack = audio.getChildWithName ("Rack");

        if (! rack.isValid())
        {
            rack = juce::ValueTree ("Rack");
            audio.addChild (rack, -1, nullptr);
        }

        return insertObject (rack, rack.getNumChildren(), "Channel", id,
                             { { "class", channelClass } });
    }

    EditResult ShowDocument::createFeed (const std::string& cueId,
                                         const std::string& slotId,
                                         const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (cue, cue.getNumChildren(), "Feed", id,
                             { { "slot", slotId } });
    }

    EditResult ShowDocument::createInsert (const std::string& cueId,
                                           const std::string& channelId,
                                           const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        return insertObject (cue, cue.getNumChildren(), "Insert", id,
                             { { "channel", channelId } });
    }

    EditResult ShowDocument::createRange (const std::string& cueId, double in, double out,
                                          const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  ONLY A MEDIA CUE, because a range is a region of the cue's own file
            (§3.24) and a cue that plays nothing has no file to cut up. The
            element name is asked directly here rather than through the owner
            word, unlike a trigger: `cue` is the answer for every kind, and this
            is the one kind. */
        if (cue.getType().toString() != "Media")
            return EditResult::failed (reason::typeMismatch);

        /*  A RANGE THAT ENDS BEFORE IT BEGINS is not a range, and this is the
            one thing about one that can be judged without reading the file.
            Whether `out` is past the end of the media is answered when the cue
            is armed, which is when the file is opened - a show whose media has
            not arrived yet still has to open. */
        if (in < 0.0 || ! (out > in))
            return EditResult::failed (reason::badValue);

        return insertObject (cue, cue.getNumChildren(), "Range", id,
                             { { "in", osc::formatDouble (in) },
                               { "out", osc::formatDouble (out) } });
    }

    EditResult ShowDocument::createTrigger (const std::string& cueId, const std::string& kind,
                                            const std::string& id)
    {
        auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  ON ANY CUE, which is what §3.7 says: "a cue or a group carries a
            trigger list". So the question asked is whether the parent IS a cue,
            through the owner word the parameter table already answers it with -
            not whether its element name is one of a list. That list has grown
            twice since Phase 1 and both times something was forgotten. */
        if (ownerForElement (cue.getType().toString().toStdString()) != "cue")
            return EditResult::failed (reason::typeMismatch);

        if (kind != "osc" && kind != "midi" && kind != "clock")
            return EditResult::failed (reason::badValue);

        return insertObject (cue, cue.getNumChildren(), "Trigger", id,
                             { { "kind", kind } });
    }

    EditResult ShowDocument::createRole (const std::string& groupId, const std::string& role,
                                         const std::string& id)
    {
        auto group = findById (groupId);

        if (! group.isValid())
            return EditResult::failed (reason::unknownId);

        /*  Only a group has members to run before or after. A header on a media
            cue would be a statement about an order that does not exist. */
        if (group.getType().toString() != "Group")
            return EditResult::failed (reason::typeMismatch);

        const auto element = role == "header" ? "Header"
                           : role == "footer" ? "Footer"
                                              : "";

        if (*element == '\0')
            return EditResult::failed (reason::typeMismatch);

        /*  ASKING TWICE ANSWERS WITH THE FIRST. A group has at most one of
            each, so a second is not a thing to refuse OR to create - the caller
            wanted the group's footer and there it is. It also makes the command
            idempotent, which is what a replay needs from anything that can
            arrive more than once. */
        if (const auto existing = group.getChildWithName (element); existing.isValid())
            return EditResult::succeeded (existing[idProperty].toString().toStdString());

        /*  AT THE END, whatever it is. Where a header sits among the members is
            not what makes it a header - the element is - and inserting it at
            the top would reorder the members of every group that gained one
            later. */
        return insertObject (group, group.getNumChildren(), element, id, {});
    }

    EditResult ShowDocument::createPersistent (const std::string& listId, const std::string& id)
    {
        auto list = findById (listId);

        if (! list.isValid())
            return EditResult::failed (reason::unknownId);

        if (list.getType().toString() != "List")
            return EditResult::failed (reason::typeMismatch);

        /*  One per list, and the second ask is the first's answer - the same
            idempotence `createRole` has, for the same replay reason. */
        if (const auto existing = list.getChildWithName ("Persistent"); existing.isValid())
            return EditResult::succeeded (existing[idProperty].toString().toStdString());

        return insertObject (list, list.getNumChildren(), "Persistent", id, {});
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
        /*  First, before the identifier is looked up. A locked show refuses
            the corrected command as well, so of the two things that could be
            wrong with a delete on a locked show the lock is the one worth
            reading first. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

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
        /*  DEEPER THAN A LIST'S TOP LEVEL, since PR 3.4. The pointer can stand
            inside a manual sequence group, so the list that is parked on this
            cue may be several levels above it - and asking only the immediate
            parent would have left a standby pointing at a cue that had gone.

            Where it goes is the next remaining SIBLING, which is the same
            answer one level down as it was at the top: during tech, deleting
            the cue you are parked on should leave you parked on the next one.
            When it was the last member of a group, that is empty and the
            walk below falls back to the group's own successor - which is where
            `next` would have taken the pointer anyway.

            Worked out BEFORE the cue disappears, because afterwards there is no
            sequence left to ask, and done inside the applied command so a
            replay reproduces it with no repair record in the log. */
        std::string repairList, repairStandby;

        const auto list = listContaining (parent);
        const auto standby = list.isValid()
                               ? list[standbyProperty].toString().toStdString()
                               : std::string {};

        /*  PARKED ON IT, OR ON SOMETHING INSIDE IT - and the second is the half
            that was missing.

            PR 3.4 widened the LOOKUP (a list several levels above the cue) and
            left the MATCH asking only whether the deleted node WAS the standby.
            Deleting a group the pointer was standing inside therefore released
            the pointer's cue and left the list still naming it, which is the
            invariant broken by the very route the widening was for.

            It does not heal: `standby.next` finds nothing on the path, answers
            with the identifier it was given, and the write-back is then refused
            because that cue is no longer in the list - so the pointer is frozen.
            GO is worse. The standby is not empty, so it passes the early return;
            the advance fails and its result is discarded; and firing a cue that
            does not exist does nothing. The operator's GO key goes quietly dead
            until somebody thinks to move standby by hand.

            `released` is already every identifier under the node, collected
            above because they all go back to the registry - so asking whether
            the pointer is one of them is the same question in one test, and
            cannot come apart from the release the way a second walk could. */
        const auto parked = ! standby.empty()
                              && std::find (released.begin(), released.end(), standby)
                                   != released.end();

        if (parked)
        {
            repairList = list[idProperty].toString().toStdString();

            /*  WHERE THE POINTER GOES IS MEASURED FROM THE NODE BEING DELETED,
                not from the cue it was on: the cue may be three levels inside
                the thing that is disappearing, and its own siblings are going
                with it. What survives is whatever follows the deleted node,
                which is where `next` would have carried the pointer once the
                subtree was gone. */
            repairStandby = siblingAfter (parent, id);

            if (repairStandby.empty())
                repairStandby = cue::nextStandby (list, id);

            /*  `nextStandby` answers with the identifier it was given when
                there is nowhere to go, and that one is about to stop existing.
                It can also answer with something else inside the doomed
                subtree, so the whole released set is the test rather than just
                the node's own identifier. */
            if (std::find (released.begin(), released.end(), repairStandby) != released.end())
                repairStandby.clear();
        }

        /*  THE HISTORY IS THE SECOND ARGUMENT TO THIS OVERLOAD, not the third:
            `removeChild (const ValueTree&, UndoManager*)`. The action it makes
            holds a ref-counted handle on the whole subtree, which is what lets
            an undo put the cues back with the identifiers they had - and is why
            the registry has to be told afterwards rather than the document. */
        parent.removeChild (node, structuralHistory());

        for (const auto& released_id : released)
            registry.release (released_id);

        if (! repairList.empty())
            setAttribute ("/godot/list/" + repairList + "/standby", repairStandby);

        return EditResult::succeeded (id);
    }

    EditResult ShowDocument::move (const std::string& id, const std::string& newParentId, int newIndex)
    {
        /*  First, for the reason `remove` gives. A reorder is an edit to the
            show like any other - the order of the rows IS the show - so a
            locked show refuses it, and the standby repair below, which writes
            a state row, is never reached. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

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
        /*  WIDENED IN PR 3.4 from "leaving a list's top level" to "leaving the
            manual path", because the pointer can now stand inside a manual
            sequence group. Moving a cue from one place on that path to another
            leaves the pointer alone - it stores an identifier, and §3.5 is
            explicit that it does not follow the shape of the list around. What
            clears it is the cue landing somewhere the pointer is not allowed to
            be: inside an automatic group, inside a header, or in another list. */
        const auto vacated = listContaining (oldParent);

        const auto wasParkedOnIt = vacated.isValid()
                                     && vacated[standbyProperty].toString().toStdString() == id;

        const auto vacatedList = wasParkedOnIt
                                   ? vacated[idProperty].toString().toStdString()
                                   : std::string {};

        /*  BOTH LIMBS TAKE THE HISTORY, or an undo of a cross-parent move would
            put the cue back without taking it out of where it went.

            A within-parent move is one action, which JUCE coalesces with the
            next move of the same parent on its own - so it is the transaction
            rule above, and not this line, that keeps two ▲ presses from
            collapsing into one step. A cross-parent move is two actions in one
            transaction, undone in reverse. */
        if (oldParent == newParent)
        {
            const auto from = newParent.indexOf (node);
            const auto to = std::min (newIndex, newParent.getNumChildren() - 1);
            newParent.moveChild (from, to, structuralHistory());
        }
        else
        {
            oldParent.removeChild (node, structuralHistory());
            newParent.addChild (node, std::min (newIndex, newParent.getNumChildren()),
                                structuralHistory());
        }

        /*  Asked AFTER the move, because whether the cue is still somewhere the
            pointer may be is a question about where it has landed. A cue that
            moved within the manual path of the same list keeps the pointer. */
        if (! vacatedList.empty()
              && ! cue::isOnManualPath (findById (vacatedList), id))
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

        /*  AN OSC TRIGGER MAY NOT LISTEN WHERE THE ENGINE ANSWERS.

            §3.7's triggers arrive on Go.dot's own OSC port, which is the same
            port `/godot/...` is written on and the same port a mounted
            namespace is proxied through. An address under either is a message
            that would both write a value and fire a cue, and nobody reading the
            log afterwards could say which had been meant - nor which the sender
            intended, because the sender wrote one message.

            REFUSED WHEN THE SHOW IS READ rather than discovered during it,
            which is the rule the whole document layer follows for things that
            cannot be honoured at all. A dangling reference is a warning, and
            this is not one: there is no reading of the file under which such a
            trigger does what it says.

            A SECOND PASS, because it needs the mounts and the walk above meets
            them in document order - mounts are written after the lists. */
        std::vector<std::string> prefixes;

        /*  The same pass also collects what each mount can be ASKED, which the
            verified-cue check below needs. One traversal, because the two
            questions are about the same elements and a second walk would be a
            second thing to keep in step. */
        struct MountFacts
        {
            std::string id;
            std::string prefix;
            bool canBeAsked = false;
        };

        std::vector<MountFacts> mountFacts;

        for (const auto& mounts : showNode)
        {
            if (mounts.getType().toString() != "Mounts")
                continue;

            for (const auto& mount : mounts)
            {
                if (! mount.hasProperty (juce::Identifier ("prefix")))
                    continue;

                const auto prefix = mount[juce::Identifier ("prefix")].toString().toStdString();
                prefixes.push_back (prefix);

                /*  `canBeAsked`, restated in document terms: readback names the
                    MECHANISM and a query port says where to use it. The same
                    rule lives on `tree::MountDeclaration`, which is what the
                    engine asks at run time; this is the read-time half, and the
                    two say the same sentence because question K's answer is one
                    sentence. */
                const auto readback = mount[juce::Identifier ("readback")].toString();
                const auto queryPort = static_cast<int> (mount[juce::Identifier ("queryPort")]);

                mountFacts.push_back ({ mount[juce::Identifier ("id")].toString().toStdString(),
                                        prefix,
                                        readback == "oscquery" && queryPort > 0 });
            }
        }

        struct Triggers
        {
            std::vector<std::string>& problems;
            const std::vector<std::string>& prefixes;

            static bool under (const std::string& address, const std::string& prefix)
            {
                return address == prefix
                         || address.rfind (prefix + "/", 0) == 0;
            }

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Trigger")
                {
                    const auto kind = node[juce::Identifier ("kind")].toString().toStdString();
                    const auto address = node[juce::Identifier ("address")]
                                           .toString().toStdString();

                    if ((kind.empty() || kind == "osc") && ! address.empty())
                    {
                        const auto here = "/Show/.../Trigger[" + node[juce::Identifier ("id")]
                                            .toString().toStdString() + "]";

                        if (under (address, "/godot"))
                            problems.push_back (here + ": an OSC trigger may not listen at \""
                                                + address + "\", which is inside the engine's own"
                                                " namespace");

                        for (const auto& prefix : prefixes)
                            if (! prefix.empty() && under (address, prefix))
                                problems.push_back (here + ": an OSC trigger may not listen at \""
                                                    + address + "\", which is inside the mount"
                                                    " prefix \"" + prefix + "\"");
                    }
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Triggers { problems, prefixes }.visit (showNode);

        /*  A START OFFSET AND A LIST OF RANGES ARE TWO ANSWERS TO ONE QUESTION.

            `startOffset` says where in the file playback begins. A range says
            the same thing and says where it ends and how many times, and a cue
            with ranges plays the list rather than the file (§3.24). Honouring
            both would mean choosing between them - offsetting the first range,
            or ignoring the offset - and either choice is a rule nobody wrote
            down that somebody would find out about during a show.

            REFUSED WHEN THE SHOW IS READ, like the trigger address above and
            for the same reason: there is no reading of the file under which the
            cue does what both attributes say. Zero is the resting value and
            says nothing, so it is only a non-zero offset that collides. */
        struct Offsets
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Media")
                {
                    const auto offset = static_cast<double> (
                        node[juce::Identifier ("startOffset")]);

                    bool hasRange = false;

                    for (const auto& child : node)
                        if (child.getType().toString() == "Range")
                            hasRange = true;

                    /*  `> 0` rather than `!= 0`: the row's range is 0.. so a
                        negative offset is the Walk pass's problem, and an
                        equality on a double is a warning in the strict build. */
                    if (hasRange && offset > 0.0)
                        problems.push_back ("/Show/.../Media["
                                             + node[juce::Identifier ("id")].toString().toStdString()
                                             + "]: a cue with ranges plays its ranges, so it cannot"
                                               " also have a startOffset - the offset belongs in the"
                                               " first range's `in`");
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Offsets { problems }.visit (showNode);

        /*  A MIDI CUE CANNOT WAIT TO BE VERIFIED, because nothing will ever
            answer.

            §3.11's `verified` asks the target for the value back and compares
            it, which is a thing an OSCQuery node can do and a MIDI cable
            cannot: there is no read-back, no address to ask about, and no
            protocol to ask in. A cue that asked for one would wait for its
            timeout and then fail, every time, at half past seven.

            REFUSED WHEN THE SHOW IS READ, like the trigger address inside
            /godot and the start offset beside a range, and for the same reason:
            there is no reading of the file under which the cue does what it
            says. The row's own enum already excludes it, so this catches the
            hand-edited file rather than the one a client wrote. */
        struct MidiWaits
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Midi"
                      && node[juce::Identifier ("wait")].toString() == "verified")
                    problems.push_back ("/Show/.../Midi["
                                         + node[juce::Identifier ("id")].toString().toStdString()
                                         + "]: a MIDI cue cannot wait to be verified - there is"
                                           " no read-back on a MIDI cable, so nothing would ever"
                                           " answer");

                for (const auto& child : node)
                    visit (child);
            }
        };

        MidiWaits { problems }.visit (showNode);

        /*  A CUE THAT WAITS FOR AN ANSWER NOBODY CAN GIVE - question K, and
            the refusal decision K actually asked for.

            §3.11's `verified` writes a value and then asks the target what it
            holds. That needs a target that can be asked, which a mount says by
            declaring `readback` and a `queryPort` (§9, decision K). Aimed at a
            mount that declares neither - or at an address under no mount at all
            - the cue writes, waits its whole timeout, and fails. Every time,
            and only ever during the show.

            REFUSED WHEN THE SHOW IS READ, which is what decision K settled and
            what was NOT built: PR 2.6 put the check in the mount loader, where
            every verb printed it to stderr and opened the show anyway, so the
            only thing it changed was `wfg validate`'s exit code. It sits here
            now, beside the MIDI rule it is the twin of, so that the refusal
            arrives the way the other refusals do - a trigger listening inside
            /godot, a start offset beside a range, a MIDI cue asking to be
            verified. The check is on the document alone and needs no mount
            table, no socket and no device, so it still runs on a laptop with
            nothing plugged in, which is the machine somebody is sitting at when
            they have time to fix it. */
        struct VerifiedCues
        {
            std::vector<std::string>& problems;
            const std::vector<MountFacts>& mounts;

            void visit (const juce::ValueTree& node)
            {
                for (const auto& child : node)
                    visit (child);

                if (node.getType().toString() != "Osc"
                      || node[juce::Identifier ("wait")].toString() != "verified")
                    return;

                const auto id = node[juce::Identifier ("id")].toString().toStdString();
                const auto address = node[juce::Identifier ("address")].toString().toStdString();

                const MountFacts* owner = nullptr;

                for (const auto& mount : mounts)
                    if (address.size() > mount.prefix.size()
                          && address.compare (0, mount.prefix.size(), mount.prefix) == 0
                          && address[mount.prefix.size()] == '/')
                        owner = &mount;

                if (owner == nullptr)
                {
                    problems.push_back ("/Show/.../Osc[" + id + "]: \"" + address
                                          + "\" is under no mounted namespace, so nothing can be"
                                            " asked about it");
                    return;
                }

                if (! owner->canBeAsked)
                    problems.push_back ("/Show/.../Osc[" + id + "]: waits for verification from "
                                          + owner->id + ", which declares no readback. A cue that"
                                            " cannot succeed is worse than one that fails, because"
                                            " it holds the list");
            }
        };

        VerifiedCues { problems, mountFacts }.visit (showNode);

        //----------------------------------------------------------------------
        /*  PHASE 4'S SLOTS, and the destinations that name them (PRD §3.9b,
            §3.9e). Four rules, all of them about numbers the document can
            check itself.

            The widths first, so the walks below are lookups rather than a
            traversal per destination. */
        std::map<std::string, int> busWidth;
        std::map<std::string, int> slotWidth;

        const auto intAttribute = [] (const juce::ValueTree& node, const char* name, int fallback)
        {
            const juce::Identifier key { name };
            return node.hasProperty (key) ? static_cast<int> (node[key]) : fallback;
        };

        for (const auto& audio : showNode)
        {
            if (audio.getType().toString() != "Audio")
                continue;

            for (const auto& bus : audio)
                if (bus.getType().toString() == "Bus")
                    busWidth[bus[idProperty].toString().toStdString()]
                        = intAttribute (bus, "width", 1);
        }

        for (const auto& mounts : showNode)
        {
            if (mounts.getType().toString() != "Mounts")
                continue;

            for (const auto& mount : mounts)
            {
                const auto prefix = mount[juce::Identifier ("prefix")].toString().toStdString();

                for (const auto& slot : mount)
                {
                    if (slot.getType().toString() != "Slot")
                        continue;

                    const auto slotId = slot[idProperty].toString().toStdString();
                    const auto width = intAttribute (slot, "width", 1);
                    const auto firstChannel = intAttribute (slot, "firstChannel", 0);
                    const auto address = slot[juce::Identifier ("address")].toString().toStdString();
                    const auto busId = slot[juce::Identifier ("bus")].toString().toStdString();

                    slotWidth[slotId] = width;

                    const auto where = "/Show/.../Slot[" + slotId + "]: ";

                    /*  AN INPUT OF THAT PROCESSOR, and not of some other one. A
                        slot whose address falls outside its own mount's prefix
                        would claim exclusivity over parameters the mount does
                        not carry - and the osc cues that write those parameters
                        would go somewhere else entirely. */
                    if (! prefix.empty()
                          && ! (address.size() > prefix.size()
                                  && address.compare (0, prefix.size(), prefix) == 0
                                  && address[prefix.size()] == '/'))
                        problems.push_back (where + "\"" + address + "\" is not under \"" + prefix
                                              + "\", which is the prefix of the mount that"
                                                " declares it");

                    /*  AND IT HAS TO FIT IN ITS BUS. `firstChannel` is an offset
                        into the bus, exactly as a bus's own is an offset into
                        the hardware outputs, so a slot that ran off the end
                        would send part of a source into channels nobody
                        declared. A bus that is not there at all is the `refers`
                        column's warning rather than this refusal - deleting a
                        bus must not stop yesterday's show opening. */
                    if (const auto bus = busWidth.find (busId); bus != busWidth.end())
                        if (firstChannel + width > bus->second)
                            problems.push_back (where + "channels " + std::to_string (firstChannel)
                                                  + " to " + std::to_string (firstChannel + width - 1)
                                                  + " do not fit in a bus " + std::to_string (bus->second)
                                                  + " channels wide");
                }
            }
        }

        /*  AND THE COEFFICIENTS, which the parameter table has said were
            refused when the show loads since Phase 2 and which nothing has ever
            checked: the only check was at arm, where it failed the run.

            WHAT IS CHECKABLE HERE is that the list is a whole number of input
            channels wide. How many channels the cue HAS is the file's, and the
            file arrives on a different machine from the one the show was
            written on - so `gains.size() % width` is the question the document
            can answer, and "eight gains into a bus three wide" is a matrix that
            is not a matrix whoever plays it. */
        struct Destinations
        {
            std::vector<std::string>& problems;
            const std::map<std::string, int>& busWidth;
            const std::map<std::string, int>& slotWidth;

            static std::size_t countTokens (const std::string& text)
            {
                std::size_t count = 0, i = 0;

                while (i < text.size())
                {
                    while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                        ++i;

                    if (i >= text.size())
                        break;

                    ++count;

                    while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                        ++i;
                }

                return count;
            }

            void check (const juce::ValueTree& node, const char* element, const char* target,
                        const std::map<std::string, int>& widths)
            {
                const auto id = node[idProperty].toString().toStdString();
                const auto targetId = node[juce::Identifier (target)].toString().toStdString();
                const auto found = widths.find (targetId);

                if (found == widths.end())
                    return;                 // dangling: the `refers` column's warning

                const auto width = static_cast<std::size_t> (std::max (1, found->second));
                const auto gains = countTokens (node[juce::Identifier ("gains")]
                                                  .toString().toStdString());

                /*  A cue routed nowhere YET is an ordinary state for a show
                    being written, and it is silent rather than wrong -
                    `resolveRouting` says so in as many words at arm time. Only
                    a list that exists and does not divide is a mistake. */
                if (gains == 0)
                    return;

                if (gains % width != 0)
                    problems.push_back (std::string ("/Show/.../") + element + "[" + id + "]: "
                                          + std::to_string (gains) + " coefficients do not divide"
                                            " into a destination " + std::to_string (width)
                                          + " channels wide - a routing matrix is one row per"
                                            " channel the cue has");
            }

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString();

                if (element == "Route")       check (node, "Route", "bus", busWidth);
                else if (element == "Feed")   check (node, "Feed", "slot", slotWidth);

                for (const auto& child : node)
                    visit (child);
            }
        };

        Destinations { problems, busWidth, slotWidth }.visit (showNode);

        return problems;
    }

    std::vector<std::string> ShowDocument::warnings() const
    {
        std::vector<std::string> problems;

        /*  EVERY REFERENCE THE TABLE DECLARES, checked in one place.

            An identifier in a document is a pointer at another object, and
            until the `refers` column there was no one place that knew it: the
            standby pointer had a function of its own, `fade/@target` and
            `stop/@target` had nothing at all, and this file carried a note
            saying the generalisation would be worth writing when there was a
            second case. Phase 3 brought four more - a range's cue, a MIDI
            cue's port, a run's parent, a trigger's cue - and this is it.

            A WARNING AND NEVER A LOAD REFUSAL, which is the whole shape of it.
            §3.8 makes a stop aimed at a cue that is not there a silent no-op
            during tech; `object.delete` repairs nothing referential, by
            design, because repairing it would mean deciding what somebody
            meant; and yesterday's saved show has to open tomorrow. So the file
            loads, `wfg validate` says which pointer is dangling, and a cue
            that is actually fired fails its run.

            THE TARGET IS LOOKED UP BY IDENTIFIER AND THEN BY KIND, both,
            because half a check is worse than none: a fade whose target is a
            BUS would otherwise pass, and would fail at half past seven with a
            message about a run rather than about a show. */
        struct References
        {
            std::vector<std::string>& problems;
            const ShowDocument& document;

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString().toStdString();

                if (const auto* described = Schema::instance().element (element))
                {
                    for (const auto& attribute : described->attributes)
                    {
                        const auto refers = attribute.refers();

                        if (refers.empty())
                            continue;

                        const juce::Identifier name { juce::String (std::string (attribute.name())) };

                        if (! node.hasProperty (name))
                            continue;

                        const auto value = node[name].toString().toStdString();

                        /*  Empty is a pointer at nothing on purpose - a list
                            with no standby, a cue with no target yet - and is a
                            resting state rather than a dangling reference. */
                        if (value.empty())
                            continue;

                        const auto target = document.findById (value);

                        const auto here = "/Show/.../" + element + "["
                                            + node[idProperty].toString().toStdString() + "]/@"
                                            + std::string (attribute.name());

                        if (! target.isValid())
                        {
                            problems.push_back (here + ": names \"" + value
                                                 + "\", which is not in this show");
                            continue;
                        }

                        const auto found = ownerForElement (
                            target.getType().toString().toStdString());

                        if (found != refers)
                            problems.push_back (here + ": names \"" + value + "\", which is a "
                                                 + (found.empty() ? std::string ("thing of no kind")
                                                                  : std::string (found))
                                                 + " and not a " + std::string (refers));
                    }
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        References { problems, *this }.visit (showNode);

        /*  A PRESET THAT NAMES A GROUP THE CUE IS NOT INSIDE.

            §13.7: `preset` names the ANCESTOR group whose header prepares this
            cue, and naming an ancestor is what the gesture means - dragging a
            cue onto the header of one of the groups it is in. A value naming
            anything else is a mark nothing can act on: the horizon prepares a
            block by walking its own subtree, so a cue outside that subtree is
            never reached however early the pointer arrives.

            A WARNING AND NOT A REFUSAL, for the reason every referential
            mistake in this file is one: the repair is somebody dragging it
            somewhere sensible, and yesterday's saved show has to open tomorrow.
            The `refers` column above has already said whether the identifier
            names a cue at all; this says whether it names one that could ever
            do the preparing. */
        struct Persistents
        {
            std::vector<std::string>& problems;

            void visit (const juce::ValueTree& node)
            {
                if (node.getType().toString() == "Persistent")
                {
                    /*  MEDIA, OSC AND MIDI ARE WHAT A SECTION CAN ASSERT (§13.11).
                        A fade asserts nothing, a stop is the thing that
                        SUSPENDS an assertion, and a group is a lifetime rather
                        than a state. Each is left where it is and ignored, and
                        this is where somebody finds out why nothing happens. */
                    for (const auto& child : node)
                    {
                        const auto element = child.getType().toString().toStdString();

                        if (element != "Fade" && element != "Stop" && element != "Group")
                            continue;

                        problems.push_back (
                            "/Show/.../Persistent/" + element + "["
                              + child[idProperty].toString().toStdString()
                              + "]: a persistent section asserts media, osc and midi cues and"
                                " nothing else - a fade asserts nothing, a stop is what suspends"
                                " an assertion, a group is a lifetime rather than a state - so"
                                " this one is ignored");
                    }
                }

                for (const auto& child : node)
                    visit (child);
            }
        };

        Persistents { problems }.visit (showNode);

        struct Presets
        {
            std::vector<std::string>& problems;
            std::vector<std::string> ancestors;

            void visit (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString().toStdString();
                const auto isGroup = element == "Group";

                if (ownerForElement (element) == "cue")
                {
                    const auto preset = node[juce::Identifier ("preset")]
                                            .toString().toStdString();

                    if (! preset.empty()
                         && std::find (ancestors.begin(), ancestors.end(), preset)
                              == ancestors.end())
                        problems.push_back (
                            "/Show/.../" + element + "["
                              + node[idProperty].toString().toStdString()
                              + "]/@preset: names \"" + preset + "\", which is not a group this"
                                " cue is inside - so no header will prepare it, and it will run"
                                " at its own moment as if the mark were not there");
                }

                if (isGroup)
                    ancestors.push_back (node[idProperty].toString().toStdString());

                for (const auto& child : node)
                    visit (child);

                if (isGroup)
                    ancestors.pop_back();
            }
        };

        Presets { problems, {} }.visit (showNode);

        /*  AND A MOUNT THAT SAYS ITS NODES MAY BE WRITTEN EARLY BUT CANNOT BE
            ASKED WHAT THEY HELD.

            §13.1 makes anticipation conditional on being able to take it back,
            and taking a write back means putting the old value there - which
            needs the old value, which needs a read. A mount marked
            `anticipatable` with `readback` of `none` has told Go.dot that an
            early write is safe and given it no way to undo one, so nothing on
            it is ever pre-sent and every cue aimed at it runs at entry.

            A WARNING AND NOT A REFUSAL, because the show is complete and
            correct: what it loses is a saved moment, not a sound. It is worth
            saying out loud because the two attributes are set in different
            places and one of them is doing nothing. */
        const auto mounts = showNode.getChildWithName (juce::Identifier ("Mounts"));

        for (const auto& mount : mounts)
        {
            if (mount.getType().toString() != "Mount")
                continue;

            const auto anticipatable =
                mount.hasProperty (juce::Identifier ("anticipatable"))
                  && static_cast<bool> (mount[juce::Identifier ("anticipatable")]);

            if (! anticipatable)
                continue;

            const auto readback = mount.hasProperty (juce::Identifier ("readback"))
                                    ? mount[juce::Identifier ("readback")].toString().toStdString()
                                    : std::string ("none");

            if (readback == "oscquery")
                continue;

            problems.push_back ("/Show/.../Mount["
                                  + mount[idProperty].toString().toStdString()
                                  + "]/@anticipatable: says its nodes may be written ahead of a"
                                    " GO, but @readback is \"" + readback
                                  + "\" - so nothing can be read back to restore, and nothing"
                                    " will be pre-sent");
        }

        return problems;
    }
}
