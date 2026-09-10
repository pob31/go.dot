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

#include <cstdint>
#include <optional>
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

    class ShowDocument : private juce::ValueTree::Listener
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

        /*  Adds a destination to a media cue: which bus it feeds, and nothing
            else. The coefficients are written afterwards through the ordinary
            node path, because that is how every other value is written and a
            second way in would be a second thing to keep correct.

            Fails when the identifier names no cue, or names a cue that plays
            nothing - a memo cue with a destination is a statement about a sound
            that does not exist. */
        EditResult createRoute (const std::string& cueId, const std::string& busId,
                                const std::string& id = {});

        /*  Gives a group its header or its footer, and answers with the one it
            already had if it has one.

            §3.6 makes these ordinary cue lists: a header runs before the
            group's members and a footer runs at group exit and BLOCKS, so the
            group is not done until the footer's cues are. They hold cues, which
            is why they are containers rather than a word on each cue - and they
            carry an identifier, because `cue.create` and `object.move` address
            a parent by one and without it there would be no way to put a cue in
            a footer.

            `role` is "header" or "footer". At most one of each, which is why
            asking twice answers with the first rather than making a second: two
            headers is not a thing a group can have, and the friendliest moment
            to say so is before it exists. */
        /*  Adds a trigger to a cue - any kind of cue, because §3.7 gives the
            list to "a cue or a group". The kind is fixed at creation for the
            same reason a cue's is: a trigger that could be turned from a note
            into a time of day by writing a word would carry the fields of the
            kind it used to be. */
        EditResult createTrigger (const std::string& cueId, const std::string& kind,
                                  const std::string& id = {});

        /*  Adds a range to a media cue - a region of the cue's own file, and
            one more clip in a launcher slot when the cue is armed (§3.24).

            `in` and `out` are given at creation rather than defaulted, because
            a range whose bounds are both nought is not a shorter way of saying
            anything: it is a range of no length, and every path that would then
            have to tolerate one is a path that could have refused it here. */
        /*  PHASE 4'S SLOTS (PRD §3.9e). A slot is one position in a pool of
            fixed size declared at load; typed; exclusive; held for a live
            range. These declare the pool.

            `createSlot` adds a processor input to a mount: the show says which
            of a processor's inputs it is using and at what width (decision P).
            `createRackChannel` adds a channel to the rack, making the `Rack`
            container on demand the way `createRole` makes a `Header`.

            `createFeed` and `createInsert` are what a CUE says about them - a
            destination that is a slot rather than a bus, and a channel the cue
            processes through. Destinations are a list and not a choice
            (§3.9b), so both sit beside a cue's routes rather than instead of
            them. */
        EditResult createSlot (const std::string& mountId, const std::string& address,
                               const std::string& id = {});

        EditResult createRackChannel (const std::string& channelClass,
                                      const std::string& id = {});

        EditResult createFeed (const std::string& cueId, const std::string& slotId,
                               const std::string& id = {});

        EditResult createInsert (const std::string& cueId, const std::string& channelId,
                                 const std::string& id = {});

        EditResult createRange (const std::string& cueId, double in, double out,
                                const std::string& id = {});

        /*  A list's persistent section (§3.29), made once: asking twice answers
            with the one it has, as `createRole` does for a header. */
        EditResult createPersistent (const std::string& listId, const std::string& id = {});

        EditResult createRole (const std::string& groupId, const std::string& role,
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
        // The edit lock
        //======================================================================

        /*  WHETHER THE SHOW IS LOCKED (decision W): `/godot/document/locked`,
            an attribute of the root written by an ordinary `node.set` like
            any other writable node.

            A LOCK ON THE SHOW AND NOT A MODE IN A CLIENT. The tablet in the
            house, an MCP client and somebody's renumbering script all reach
            `object.delete` through the same socket, and a client that hides
            its own buttons has locked exactly one of them. So the refusal is
            here, in the document's four doors - `setAttribute`,
            `insertObject`, `remove` and `move` - because every edit in the
            engine goes through one of them, and a predicate at the doors
            cannot be got past by a command that forgot to ask. (`adopt` is
            the one writer that is not a door, because it replaces the show
            rather than editing it; its definition says what that means for
            the lock.)

            WHAT IT LOCKS IS THE SHOW HALF, as a rule rather than as a
            property of this one row: a write to a `persist == show` value is
            refused, and a write to a `persist == state` value is not. The
            state half is where the operator is standing - the standby a GO
            moves, the focus, and this lock, whose own release has to get
            through or nobody could ever lift it. It is also the half a
            bundle's state.xml is restored through, so a lock that refused
            state writes would make a locked bundle refuse to load its own
            `locked="true"`. Namespace draft §14.11 has the whole argument.

            Persisted in state.xml, so a show locked at 20:40 whose engine was
            restarted at 20:44 comes back locked. */
        bool isLocked() const;

        //======================================================================
        // Lookup
        //======================================================================

        juce::ValueTree findById (std::string_view id) const;
        juce::ValueTree root() const noexcept { return showNode; }

        IdRegistry& ids() noexcept { return registry; }
        const IdRegistry& ids() const noexcept { return registry; }

        /** Replaces the whole document, taking over its identifiers. Used by
            the reader; nothing else should need it.

            NOT A DOOR, and the edit lock does not guard it: it replaces the
            show rather than editing it, and the lock with it. See the
            definition for why the refusal belongs to whoever opens it. */
        void adopt (juce::ValueTree newRoot, IdRegistry newRegistry);

        /*  HOW MANY TIMES THE SHOW HAS CHANGED, so that a derived answer can be
            a cache ASKED rather than a flag somebody has to remember to set.

            The mounted half of the parameter tree was moved onto exactly this
            shape in PR 3.2 and for exactly this reason: a `markStale` call is a
            line every future write path has to remember, and the one that
            forgets produces a stale reading that looks like a correct one. A
            counter on the thing itself cannot be forgotten.

            It counts CHANGES AND NOT EDITS, and the difference matters when
            reading it: one `object.move` is a remove and an add, so it may
            advance by more than one. Nothing should read the DIFFERENCE - only
            whether it differs from the number a cached answer was built at.

            It is bumped by a listener on the tree rather than by the write
            doors, which is the same argument one level down: `setAttribute`,
            `createCue` and `remove` are today's doors, the tests write through
            `ValueTree::setProperty` directly, and a phase that adds a fourth
            door would have to remember this one. The tree cannot forget.

            Never zero: a fresh document is at 1, so `0` is available to a cache
            as "never built". */
        std::uint64_t revision() const noexcept { return changeCount; }

        /*  HOW MANY TIMES THE SHOW HALF HAS CHANGED - what `show.xml` would
            say, and not where the engine had got to. `/godot/document/dirty` is
            this compared with the number the last save stamped
            (DocumentSession.h), so this is the counter that decides whether
            the operator is told there is something to save.

            THE SAME LISTENER AS `revision()`, ASKING ONE MORE QUESTION, and
            for the same reason that one is a listener: a door that forgot to
            bump it would be a dot that stayed out over an unsaved edit. A
            structural change - a child added, removed or moved, a load - bumps
            both counters unconditionally, since every one of them is something
            `show.xml` records. A PROPERTY change looks the attribute's row up in
            the schema and bumps this one only when that row is `persist ==
            show`. An attribute the schema does not know bumps it too: nothing
            but a test can write one, and when this has to guess it guesses
            dirty, because a false "unsaved" costs a save and a false "saved"
            costs a show.

            SO A GO DOES NOT LIGHT THE DOT. It writes `list/@standby` - a
            `persist == state` row - through the same choke point every edit
            uses, which moves `revision()` and not this. That is plan decision
            4, taken early so it can be overruled early, and not a law: it
            follows §3.20's line, which puts the playhead and the focus in
            state.xml precisely because losing them is not losing work, and it
            is the difference between a light that means something and one an
            operator has learned by the second act to ignore. The price is that
            state.xml can be behind with the dot out, and a standby a crash
            loses is a standby, not a show.

            TWO THINGS THAT WOULD OTHERWISE ARRIVE AS BUG REPORTS. An object's
            `id` never reaches the listener at all: `insertObject` writes it on
            a node that has not yet joined the tree, so the only change heard is
            the child being added - which counts, so nothing is lost. And the
            count is MONOTONIC: once undo exists (PR 5.4), undoing an edit will
            be a second change rather than the first one taken back, so the dot
            will not go out by undoing. What it means is "the file on disk is
            not this document's history", not "this document differs from the
            file", and namespace draft §14.15 records the second question as
            deliberately not asked.

            Starts at 1, like `revision()`, so that nought is free to mean
            "never saved". */
        std::uint64_t showRevision() const noexcept { return showChangeCount; }

        //======================================================================
        /*  Checks the whole tree against the schema: unknown elements and
            attributes, values that do not parse, duplicate or malformed
            identifiers, children where they are not allowed. Returns one
            message per problem, in document order, each naming the element it
            is about.

            Used by the reader on load and available on demand; a document that
            fails this is never handed to the engine. */
        std::vector<std::string> validate() const;

        /*  What is WRONG WITH THE SHOW but does not stop it opening.

            TWO LISTS AND NOT ONE, and the difference is the whole rule. What
            `validate` returns is a refusal: a trigger listening inside /godot,
            a start offset beside a range, a MIDI cue asking to be verified -
            things with no reading under which the file does what it says. What
            this returns is a pointer at something that is not there, and PRD
            §3.8 is explicit that such a thing is a silent no-op during tech
            rather than a broken show. `object.delete` repairs nothing
            referential by design, because repairing it would mean deciding
            what somebody meant.

            So yesterday's saved show opens tomorrow, `wfg validate` prints
            these, and a cue that is actually fired fails its run. */
        std::vector<std::string> warnings() const;

        //======================================================================
        /** The element name for a `kind` value, or empty. "memo" is a Cue,
            "group" is a Group. */
        static std::string_view elementForKind (std::string_view kind);

        /** The parameter table's owner word for an element, for addressing:
            Cue and Group are both `cue`. */
        static std::string_view ownerForElement (std::string_view element);

        /*  The container an object is ADDRESSED under, which is usually its
            owner word and is not always.

            A rack `Channel` is addressed at `/godot/slot/<id>` beside a
            processor input, because §1's first rule is that objects are
            identity-addressed and a client holding an identifier should not
            have to know which container it came out of. What it answers to the
            `refers` column is `rackChannel`, because THAT question is about
            kind - a feed naming a rack channel is a mistake the document can
            catch before the show runs. Two questions, two answers. */
        static std::string_view addressOwnerFor (std::string_view element);

        /*  The address segment an element without an identifier is reached by,
            or empty for one that is reached by id. `Show` is `document`,
            `Audio` is `audio`, `Lists` is `list` - three collections, each with
            facts of its own and nothing to look them up by. */
        static std::string_view containerSegmentFor (std::string_view element);

        /** The other direction: the element a container segment names. */
        juce::ValueTree containerElementFor (std::string_view segment) const;

    private:
        /*  The lock's refusal, or nothing when the show is not locked. Asked
            by every door that changes the show half, and at the top of
            `createRackChannel`, which is the one create that changes the
            document before it reaches its door. */
        std::optional<EditResult> refuseIfLocked() const;

        EditResult insertObject (juce::ValueTree parent, int index,
                                 std::string_view elementName,
                                 const std::string& id,
                                 const std::vector<std::pair<std::string_view, std::string>>& attributes);

        void collectIds (const juce::ValueTree& node, std::vector<std::string>& out) const;

        /*  The listener half of `revision()` and `showRevision()`. Every
            structural callback bumps both counters and does nothing else;
            `valueTreeRedirected` is included because a redirect replaces the
            content wholesale, which is the biggest change of all. The property
            callback is the one that has to ask which half of the document it
            touched, so it lives in the .cpp beside the schema lookup. */
        void valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& property) override;

        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override    { bumpStructure(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { bumpStructure(); }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override     { bumpStructure(); }
        void valueTreeParentChanged (juce::ValueTree&) override                   { bumpStructure(); }
        void valueTreeRedirected (juce::ValueTree&) override                      { bumpStructure(); }

        void bumpStructure() noexcept
        {
            ++changeCount;
            ++showChangeCount;
        }

        juce::ValueTree showNode;
        IdRegistry registry;

        /*  Starts at 1 so that nought means "no cache has ever been built".
            See `revision()`. */
        std::uint64_t changeCount = 1;

        /*  Starts at 1 so that nought means "never saved". See
            `showRevision()`, and the move below, which has to carry it. */
        std::uint64_t showChangeCount = 1;

    public:
        /*  MOVED WITH CARE AND NEVER COPIED, because the listener behind
            `revision()` is registered with the tree BY ADDRESS.

            A defaulted move would carry the listener registration of the object
            being moved from, and the moved-to document would hear nothing: its
            revision would stand still while its show changed underneath it,
            which is the one failure a revision counter exists to prevent. So
            the move deregisters there and registers here.

            And it moves every member BY NAME, which is the cost of writing it
            by hand: a counter added to the class and not to the move is a
            moved document that silently restarts its count. `showChangeCount`
            is moved there beside `changeCount`, and a moved document that
            restarted it would disagree with every session that had stamped the
            old number: dirty with nothing to save, and then, some edits later,
            clean with everything to save.

            A copy is refused outright. `juce::ValueTree` is a reference type, so
            a copied document would not be a second show but a second handle on
            one - two objects that could be edited through either and would
            disagree about their identifier registries. Nothing wants that;
            saying so at compile time is cheaper than finding out. */
        ShowDocument (const ShowDocument&) = delete;
        ShowDocument& operator= (const ShowDocument&) = delete;

        ShowDocument (ShowDocument&&);
        ShowDocument& operator= (ShowDocument&&);

        ~ShowDocument() override;
    };
}
