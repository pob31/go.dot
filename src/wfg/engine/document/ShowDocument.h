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

    AN UNDO MANAGER PER DOMAIN since PR 5.4, and the two measured hazards that
    kept one out until then are answered rather than gone.

    With a manager attached, ValueTree::setProperty compares through var::equals,
    where "1" == 1, so a typed write over a string-typed property would be
    dropped with no refusal and no log record - the silent drop above,
    reintroduced by the phase that attaches the manager. What answers it is that
    every var in this document is built by `toVar` from a value the schema has
    already parsed, so the type is the row's and never the text's. That is only
    true while the READER agrees, and CanonicalXml writes properties through a
    hand-written copy of the same switch, so UndoTests pins the two against each
    other: it is the one hazard here that no reviewer can see in a diff.

    And UndoManager stamps each transaction with Time::getCurrentTime(), which
    is a wall-clock read on the apply path. It is the one this engine sanctions,
    and it is named rather than hidden: it happens once per non-empty
    transaction, the stamp is stored and read by nothing here, and the
    alternative is re-implementing JUCE's three actions and their coalescing
    arithmetic in the subsystem PRD §4.3 says trust rests on.

    THE WRITE CHOKE POINT DOES NOT TAKE AN UNDO MANAGER, and never did - the
    sentence that said it did was stale for four phases and is corrected here.
    `setAttribute`, `insertObject`, `remove` and `move` ASK for the history a
    write belongs on, which is what lets a row's `persist` column decide whether
    the write is undoable at all: the show half goes on the stack and the state
    half does not (namespace draft §14.9).

    THREADING: none of its own. The engine's tick thread owns this object and is
    its only writer and only direct reader; server threads read a published
    snapshot instead. See Engine.h.
*/

#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/OutputLayout.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/audio/AudioSettings.h>

#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    /*  WHICH HISTORY A WRITE GOES ON (namespace draft §14.9). One today, and
        the second is reserved so that its arrival is a row rather than a
        redesign.

        `document` holds everything a `persist == show` row or a structural door
        writes: names, kinds, order, routes, ranges, fades, mounts - what
        someone decided (PRD §4.10). Phase 6's parameter and binding writes take
        the second entry, and the reason they are separate is the reason there
        is an enum at all: a fader ridden through an act emits hundreds of
        writes, and folded into one history they would bury the three edits
        somebody actually made. An operator reaching for Undo after a mistyped
        cue name would get their level back instead - a fader jumping during a
        show, from a keystroke whose whole purpose was to undo a piece of
        typing. */
    enum class UndoDomain
    {
        document
    };

    /** How many histories a document holds. */
    inline constexpr std::size_t undoDomainCount = 1;

    /** The word a client spells a domain with, for `undo` and `redo`. */
    std::string_view undoDomainWord (UndoDomain domain) noexcept;

    /** The domain a word names, or nothing for one the enum does not carry -
        which is a `bad-value` and never a silent fall back to `document`. */
    std::optional<UndoDomain> undoDomainForWord (std::string_view word) noexcept;

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
        EditResult configureAudio (const audio::AudioSettings&);

        //======================================================================
        // Structure
        //======================================================================

        /*  `id` may be empty, in which case one is generated and returned in the
            result. That is what makes replay work without randomness: the engine
            logs the identifier it produced, and replaying the log supplies it. */
        EditResult createList (const std::string& name, const std::string& id = {});

        /** `kind` is "memo" or "group"; a group is a cue that holds cues.

            `index` is a MEMBER position, the same number `move` takes and the
            same one `order` publishes - see `document/Sequence.h`. */
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
        EditResult groupSelection (const std::vector<std::string>& ids, const std::string& id = {});
        /** Import convenience: explicitly route an unassigned cue to the first
            output bus. Existing routes and processor feeds are preserved. */
        EditResult defaultMediaRoute (const std::string& cueId, int channels,
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

        /*  THE OUTPUT LAYOUT (PRD §3.9b, §6.2). A show's outputs are a list
            somebody wrote - so many mono direct outs, so many stereo mix
            channels, in the order the interface is wired - and
            `Bus/@firstChannel` is the running sum of the widths before each one
            rather than a number anybody types. These four keep it that way, and
            keep `audio/@outputPatch` in step: `document/OutputLayout.h` holds
            the rule and says why a layout written by hand is preserved rather
            than repacked.

            `kind` is "direct" or "mix". `index` is a position in the list of
            buses, counted the way a client counts rows; -1 appends, and
            `moveBus`'s is a position in the list AS IT STANDS, exactly as
            `move`'s is, so one drag rule serves both lists a window draws.

            THE LIST IS `firstChannel` ORDER, and these put the document's own
            children into that order as they go. For a show written by these
            commands the two never differ; for one written by hand they can, and
            the first of these to run settles it - visibly, in one undo step,
            and leaving the invariant true from then on.

            `removeBus` also takes away every route that named the bus and
            clears every processor input that fed from it, in the same
            transaction. A dangling destination is not the tidier answer: it is
            a run that fails `bad-route` on a show night, months after the
            delete that caused it. Undo brings the bus and its routes back
            together. */
        EditResult createBus (const std::string& kind, int width, int index = -1,
                              const std::string& id = {});

        /*  WHAT A NEW SHOW ARRIVES WITH, which has to be enough to play
            something.

            A document built by the constructor is EMPTY - no list, no outputs,
            and `tracks` at nought - and that is right for a scratch document,
            a paste target and every test. It was also what File - New wrote to
            disk, and the result was a show that looked complete and could
            never make a sound: no command set the track count, so a dropped
            file was armed against a polyphony ceiling of nought and every GO
            ended `no-track`. The outputs being plainly listable since
            2026-09-21 made that stranger rather than better.

            So a new show starts with a list, somewhere for sound to go, and
            room for some of it: one stereo direct out on the first two
            interface channels, and THIRTY-TWO cues able to sound at once
            (author, 2026-09-25: "Can we default to 32 voices when starting a
            new project so people don't have a broken app until they figure
            out the polyphony?"). It was eight, and eight runs out sooner than
            it looks: a sampler bank holds a track for every member it arms,
            and the next standby cue holds one ready for GO, so one bank and a
            scene already crowd it - and a member with no track plays nothing.
            Thirty-two is what M11 measured affordable (a third of a 96 kHz,
            64-frame block), and it is still one number in a box on the
            Outputs tab the moment anybody disagrees.

            Both halves or neither: tracks with no output is worse than no
            tracks at all, because the engine refuses to start a show that has
            somewhere to play from and nowhere to play to. */
        EditResult startNewShow (int tracks = 32);

        EditResult removeBus (const std::string& id);

        EditResult moveBus (const std::string& id, int index);

        EditResult resizeBus (const std::string& id, int width);

        /*  THE NAMED INPUTS (Phase 9b, namespace draft §18.2): the buses' four
            for the other side of the interface. A name and a width, packed onto
            the logical inputs the input patch maps to hardware, and kept in the
            `<Audio><Inputs>` container, made on demand at a fixed place after
            the buses. The same arithmetic as the outputs (`OutputLayout.h`),
            the same patch rule with `inputPatch` and `inputPatchSettled`, and
            the same answers: a width outside one to eight is `bad-value`, an
            input the show does not have is `unknown-id`. */
        EditResult createInput (int width, int index = -1, const std::string& id = {});
        EditResult removeInput (const std::string& id);
        EditResult moveInput (const std::string& id, int index);
        EditResult resizeInput (const std::string& id, int width);

        EditResult createFeed (const std::string& cueId, const std::string& slotId,
                               const std::string& id = {});

        EditResult createInsert (const std::string& cueId, const std::string& channelId,
                                 const std::string& id = {});

        /*  Gives a media cue a send into one mix channel, at a level written
            afterwards through the ordinary node path - as a route's gains are,
            and for the same reason: one way in for values.

            A DIRECT OUT IS AN ATTRIBUTE AND A SEND IS A CHILD, which is the
            shape of the thing rather than a preference. A cue lands on one
            direct out or none, so that is a word on the cue; it sends into as
            many mix channels as it likes and each at its own level, so those
            are objects, identified, and deleted by `object.delete` like every
            other.

            Fails `unknownId` when no such cue, `typeMismatch` when the cue
            plays nothing, and `badValue` when this cue already sends into that
            bus. */
        /*  An insert on a media cue: one entry of the show's plugin set
            switched in (Phase 9a, PR 9a.8). Refuses `unknownId` for a cue or
            an entry that is not there, `typeMismatch` for a cue that plays
            nothing, and `badValue` for a second Fx naming an entry this cue
            already has. The values come afterwards, through the p<n> door. */
        EditResult createFx (const std::string& cueId, const std::string& pluginId,
                             const std::string& id);

        /*  `level` empty is the row's default; otherwise the send is born at
            it, checked like any written value (2026-09-25). */
        EditResult createSend (const std::string& cueId, const std::string& busId,
                               const std::string& id = {}, const std::string& level = {});

        /*  CUTS A RANGE IN TWO where the playhead is (author, 2026-09-21:
            *"even if the ranges amount to the full file, pressing the [+]
            range button will split the range where the cursor is. No split if
            the cursor is already on a cut or either the start or end of the
            file"*).

            ONE COMMAND AND NOT THREE, which is the whole reason it is here
            rather than assembled in the window. A split is one decision - a
            cut, at an instant - and doing it as a create, a shortening and a
            reorder would be three undo steps for one gesture, three records in
            the log, and a document that is briefly wrong between the first and
            the second. Here it is one transaction: the range keeps its place
            in the playlist and its other half is inserted directly after it.

            Refuses `badValue` when the instant is not strictly inside a range
            - which is the same answer for a cut, for the top of the file and
            for its end, because none of those is a place where there is a
            range to divide. */
        EditResult splitRange (const std::string& cueId, double at,
                               const std::string& id = {});

        EditResult createRange (const std::string& cueId, double in, double out,
                                const std::string& id = {});

        /*  A list's persistent section (§3.29), made once: asking twice answers
            with the one it has, as `createRole` does for a header. */
        EditResult createPersistent (const std::string& listId, const std::string& id = {});

        EditResult createRole (const std::string& groupId, const std::string& role,
                               const std::string& id = {});

        /*  A MIDI port the show declares, by the name a person reads -
            "Lights", "The desk". Which cable that is on this machine is said
            afterwards, with `node.set`, and is a separate fact (PRD 4.10). */
        EditResult createPort (const std::string& name, const std::string& id = {});

        EditResult createMount (const std::string& prefix, const std::string& namespaceFile,
                                const std::string& id = {});

        /*  A CONTROL SURFACE, and the strips its profile implies (PRD §3.16,
            2026-09-23): eight for a virtual panel or a Mackie unit, sixteen
            for the D700's two banks and for a pad controller. One command
            makes all of them, so the strips' identifiers are ALSO what a
            replay has to be handed: `stripIds` are used first, in order, and
            the rest are drawn; `madeStrips` answers every strip identifier the
            surface ended up with, which is what the applied record carries.
            An unknown profile is `bad-value`. */
        EditResult createSurface (const std::string& profile, const std::string& name,
                                  const std::string& id,
                                  const std::vector<std::string>& stripIds,
                                  std::vector<std::string>& madeStrips);

        /** One more strip at the end of a surface. Any profile: a Mackie unit
            with an extender has sixteen, and which of them the hardware has is
            the surface's ports, not a refusal here. */
        EditResult createStrip (const std::string& surfaceId, const std::string& id = {});

        /** A DCA (PRD §3.28), at the end of the show's DCAs. */
        EditResult createDca (const std::string& name, const std::string& id = {});

        /*  PHASE 9a: an entry in the show's plugin set - name, the scan's
            identifier, format and path, all four explicit so a replay on a
            machine that has never scanned needs no known list. <Plugins> is
            made on demand under <Audio>, after the last <Bus> and before
            <Rack>, so the canonical bytes do not depend on which of the two
            containers was asked for first. A format other than VST3, AU or
            LV2 (or none) is refused `bad-value`. */
        EditResult createPlugin (const std::string& name, const std::string& identifier,
                                 const std::string& format, const std::string& path,
                                 const std::string& id = {});

        /** How many strips a fresh surface of this profile is made with, or
            -1 for a word that is not a profile. */
        static int stripsForProfile (std::string_view profile);

        /** Removes the object and everything under it, releasing identifiers. */
        EditResult remove (const std::string& id);

        /** Moves an object to a new parent and position. An index past the end
            appends; a negative index is refused rather than clamped, because it
            usually means the caller computed it wrong.

            `newIndex` IS A MEMBER POSITION - a place in the sequence
            `/godot/cue/<id>/order` publishes, counted the way a client counts
            rows - and not a raw child index. A group's <Header>, <Footer> and
            <Persistent> children hold no position and are stepped over;
            `document/Sequence.h` is the one rule that says so, and says what
            reading the two numbers as one cost. */
        EditResult move (const std::string& id, const std::string& newParentId, int newIndex);

        /*  COPY AND PASTE (author, 2026-09-18), as two halves that never
            touch: `fragmentOf` is a READ - copies of these cues as canonical
            XML, nothing in the show moved - and `paste` is the write, the
            fragment's cues entering `parentId` at member position `index`
            under new names, in one transaction. The names are drawn here
            unless `ids` hands them back from a log; the result's `id` is
            every name drawn, space-separated, which is what the log records
            so a replay draws none.

            A fragment from ANOTHER show pastes the same way: its cues are
            cues, its file names are names the other bundle may not have, and
            its references to cues it did not bring stay as written and are
            validate()'s to name. The clipboard between two windows is the
            operating system's; what this class holds is the last thing copied
            HERE, for the tree to publish (`document/clipboard`). */
        std::string fragmentOf (const std::vector<std::string>& ids) const;
        EditResult paste (const std::string& parentId, int index, const std::string& fragment,
                          const std::vector<std::string>& ids);

        void copyToClipboard (const std::vector<std::string>& ids);
        const std::string& clipboardText() const noexcept { return clipboard; }

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
        // Undo
        //======================================================================

        /*  ONE TRANSACTION PER APPLIED COMMAND, opened here, and the caller is
            `Engine::setBeforeApply` rather than any door - so a command added
            next year is on the stack without knowing the stack exists.

            `commandName` names the transaction, because the name is what the
            operator reads: `undoName` publishes it, and "Undo object.delete" is
            a sentence a client writes without a lookup table (PRD §4.11 makes
            every gesture-reachable action carry a name already).

            THE COALESCING RULE IS SAME ADDRESS, SAME ORIGIN, WITHIN TWENTY-FIVE
            TICKS. Consecutive `node.set` matching all three join the transaction
            that is open; everything else opens a new one. It matters because of
            what a client emits: a number field dragged in the inspector sends
            one `node.set` per change event and a slider under a finger one per
            frame, and an undo that took back one of those is a keystroke that
            has to be held down - which is how an operator overshoots into the
            edit before the one they meant. Keyed on the ORIGIN as well, so two
            people editing one address from two tablets get two steps, which is
            honest when two hands were involved; a replay preserves the origin,
            so coalescing keyed on it reproduces exactly.

            `args` are the arguments AS COERCED, never as submitted, because the
            handler about to run sees the coerced ones and a window keyed on a
            value nobody applied is a window that splits differently on replay.

            Four things do not join, and each is a decision: a different address
            (one drag is one step, two fields are two edits); a different origin
            (two operators are two decisions, however close together); a gap of
            more than twenty-five ticks (a pause is where a person stopped and
            looked); and anything that is not `node.set` (every create, delete
            and move is a structural act somebody meant, and each gets a step
            named after itself). The first write after an `undo` or a `redo`
            does not join either, and nothing here has to arrange that: JUCE's
            `undo()` and `redo()` call `beginNewTransaction()` themselves before
            returning. */
        void beginTransaction (const std::string& commandName,
                               std::int64_t tick,
                               const std::string& writeOrigin,
                               const std::vector<osc::Value>& args);

        /** Half a second at 50 Hz. See `beginTransaction`. */
        static constexpr std::int64_t coalescingWindowTicks = 25;

        /*  Two and a half seconds at 50 Hz: how long after a turn of an
            insert's parameters its captured state still joins the turn's
            step - the helper's quiet moment (a second and a half) and the
            round trip, with room. A default the author may overturn. */
        static constexpr std::int64_t captureJoinWindowTicks = 125;

        /*  Takes back, or puts back, one transaction - and answers with its
            NAME, which the command logs as an applied argument so that a replay
            popping a differently named transaction fails on that record with
            both names on screen rather than diverging in silence.

            Nothing when there was nothing to take back, which the caller turns
            into `nothing-to-undo`. */
        std::optional<std::string> undo (UndoDomain domain);
        std::optional<std::string> redo (UndoDomain domain);

        /*  The named history, for reading only: `canUndo`, `canRedo`,
            `undoName` and `redoName` are published in the after-tick like every
            other readout.

            NOTHING MAY ATTACH A ChangeListener TO IT, and that is a rule rather
            than an accident. `UndoManager` is a `ChangeBroadcaster` whose
            `perform`, `undo`, `redo` and `clearUndoHistory` all send a change
            message, inert only while nobody has subscribed. The day a desktop
            client attaches one to grey out an Undo menu item, the tick thread
            posts to the message manager once per applied edit. */
        const juce::UndoManager& history (UndoDomain domain) const noexcept;

        /*  WRITES INSIDE THIS SCOPE GO ON NO HISTORY, counted so that scopes
            may nest.

            It guards exactly one call site today - `adopt`, which replaces the
            show wholesale - and is said plainly here so that a reviewer finding
            it used once does not think something is missing. The counted shape
            is for Phase 6, where a cue-driven recall will write parameter rows
            in a domain that does have a history and must not bury an operator's
            edit under a hundred of them. */
        class ScopedUndoSuppression
        {
        public:
            explicit ScopedUndoSuppression (ShowDocument& documentToSuppress) noexcept;
            ~ScopedUndoSuppression();

            ScopedUndoSuppression (const ScopedUndoSuppression&) = delete;
            ScopedUndoSuppression& operator= (const ScopedUndoSuppression&) = delete;

        private:
            ShowDocument& target;
        };

        //======================================================================
        // Lookup
        //======================================================================

        juce::ValueTree findById (std::string_view id) const;
        juce::ValueTree root() const noexcept { return showNode; }

        /*  Whether climbing from the DCA `start` through each one's `dca` -
            the DCA it sits inside - reaches `self`. The cycle test, asked by
            the write door before a DCA is put inside another and by
            `validate()` of every DCA in a file. Bounded by how many DCAs the
            show has, so a file that already carries a cycle cannot make it
            loop. */
        bool dcaChainReaches (const std::string& start, const std::string& self) const;

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

        /*  The door every create comes through. `index` is a member position,
            as it is everywhere else in this class; the creates that append
            rather than place pass `doc::endOfSequence` and say so in a word. */
        EditResult insertObject (juce::ValueTree parent, int index,
                                 std::string_view elementName,
                                 const std::string& id,
                                 const std::vector<std::pair<std::string_view, std::string>>& attributes);

        /*  The containers every show has, added to a root that is missing
            any of them. Called by the constructor and by `adopt`, so a show
            that arrived from a file is shaped like one that was just made.
            Silent: no history, no change count. */
        static void ensureContainers (juce::ValueTree& root);

        void collectIds (const juce::ValueTree& node, std::vector<std::string>& out) const;

        /*  THE ONE DOOR THAT MAY WRITE A READ-ONLY ROW, and it is private so
            that it stays one. `setAttribute` refuses `access == read`, which is
            what keeps a client from writing `Bus/@firstChannel` and leaving two
            outputs summing into one interface channel - but the engine itself
            has to write it, because repacking is exactly what the layout
            commands do. The refusal stays at the door a command comes through;
            this is the inside of the house.

            It writes through the schema, so a value that does not parse is
            still refused, and it takes the row's own history, so the write
            joins the transaction its command opened. It asks the lock nothing:
            the command above it already did. */
        EditResult writeOwned (juce::ValueTree node, std::string_view element,
                               std::string_view name, std::string_view text);

        /*  Every bus the show has, in the order the list is read - which is
            `firstChannel` order, ties broken by document order. */
        std::vector<juce::ValueTree> busNodes() const;

        /*  Every named input, read the same way: first logical input, ties
            broken by document order. */
        std::vector<juce::ValueTree> inputNodes() const;

        /*  The `<Inputs>` container, made when `make` asks and there is none:
            at a fixed place, after the last bus, so the canonical bytes do not
            depend on whether the inputs or the plugin set were asked for first
            (`createPlugin` places its own after the last bus and after this). */
        juce::ValueTree inputsContainer (bool make);

        /*  WHICH LIST A LAYOUT EDIT IS ABOUT. The arithmetic is one; what the
            two sides differ in is the element, where it lives, the patch kept
            in step and the flag that says the patch has settled. */
        enum class LayoutSide { outputs, inputs };

        /*  Applies a layout edit and writes everything that came out of it: the
            structural change, every repacked `firstChannel`, the document order
            and the patch. The eight layout commands are this and a
            `doc::LayoutEdit`. */
        EditResult applyLayout (const LayoutEdit& edit, const std::string& id,
                                const std::string& kind,
                                LayoutSide side = LayoutSide::outputs);

        /*  The histories, built empty. Called by the constructor and by the
            move, which is why it is a function rather than two loops that could
            drift apart. */
        void makeHistories();

        /*  Forgets what the last write was, so that the next one cannot join a
            transaction the document no longer holds. Said in one place because
            everything that discards a history has to say it: the move, `adopt`,
            an undo and a redo. */
        void forgetCoalescing() noexcept;

        /*  THE HISTORY A STRUCTURAL CHANGE GOES ON, or nullptr while a
            suppression is in scope.

            All three structural doors take it, or none of them do.
            `AddOrRemoveChildAction::undo` for an add removes the child BY INDEX
            with an assertion that the index is still in range, so it is correct
            only while every structural change to that parent is itself
            undoable. One door left on nullptr makes undo remove the wrong cue -
            silently, and only in shows where somebody used both doors, which is
            every show. */
        juce::UndoManager* structuralHistory() noexcept;

        /*  And the history a VALUE write goes on, which the row decides.

            `persist == show` and nothing else. The state half - a list's
            standby, the focus, the lock - is where the operator is standing
            rather than what they decided (PRD §4.10), and a pointer that jumped
            backwards on Ctrl-Z would be the machine moving standby, which PRD
            §3.5 forbids for the same reason it forbids a trigger doing it. So a
            delete's standby repair and a move's standby clearing stay where they
            went: the cue comes back, and the operator decides where to stand. */
        juce::UndoManager* historyFor (const Attribute& attribute) noexcept;

        /*  EVERY IDENTIFIER IN THE TREE, RESERVED AGAIN, after an undo or a
            redo. Undo touches the registry not at all - `removeChild` with a
            manager holds a ref-counted handle on the child and its undo re-adds
            that same object, `id` property and all - but `remove` released every
            identifier under the node on the way out, so `findById` answers and
            `isTaken` says no.

            The failure that makes it necessary is therefore not a redo handing
            out a different identifier (a redo returns the SAME one, always) but
            the NEXT create: `generate` draws from 2^40 and inserts whatever it
            finds free, so an identifier the registry has forgotten is free, and
            a cue created after an undone delete can be handed the one a restored
            cue is already using - two objects with one identity, failing not at
            the gesture but at the next save or the next GO.

            `registry.clear()` AND NOT A FRESH IdRegistry: clear empties the
            taken set and keeps the splitmix64 state, while a new registry would
            re-seed from the system entropy source and make a second entropy
            consumer inside the class whose comment says there is exactly one -
            the property that lets the log carry every drawn identifier and a
            replay re-supply it. */
        void rebuildRegistry();

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

        /** The last fragment `copyToClipboard` made; engine state, never saved. */
        std::string clipboard;

        /*  Starts at 1 so that nought means "no cache has ever been built".
            See `revision()`. */
        std::uint64_t changeCount = 1;

        /*  Starts at 1 so that nought means "never saved". See
            `showRevision()`, and the move below, which has to carry it. */
        std::uint64_t showChangeCount = 1;

        /*  ONE PER DOMAIN, HELD BY POINTER, and the indirection is forced
            rather than chosen: `juce::UndoManager` declares its copy members to
            delete them, which suppresses the implicit move members too, so it
            is neither copyable nor movable - and this class has a hand-written
            move. An array of managers is the first line that would fail to
            build. */
        std::array<std::unique_ptr<juce::UndoManager>, undoDomainCount> histories;

        /*  What the last write was, so the next one can be asked whether it
            belongs to the same gesture. Empty for anything that is not a
            `node.set`, which is how every create, delete and move breaks a
            coalescing run without a rule of its own. */
        std::string lastWriteAddress;
        std::string lastWriteOrigin;
        std::int64_t lastWriteTick = 0;

        /** Nought means writes are recorded. See `ScopedUndoSuppression`. */
        int undoSuppressions = 0;

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

            THE HISTORIES ARE THE ONE MEMBER THAT IS REBUILT EMPTY RATHER THAN
            CARRIED, and that is a decision and not an oversight. The only place
            a document is moved is a test helper returning one that has just been
            read, so there is no history worth keeping; and a move that silently
            carried actions holding ref-counted handles into a tree, across the
            one seam this class hand-writes, is the same class of bug as the
            listener registration that seam exists to fix.

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
