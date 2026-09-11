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
    What one session knows about the show's relationship with its disk: which
    folder it came from, and which revision of it that folder holds.

    NOT PART OF THE DOCUMENT, and the reason is the one Bundle.h gives for
    keeping `document.save` apart from the document commands: the document
    holds what someone decided (PRD §4.10), and neither where it lives nor when
    it was last written is a decision about the show. Two sessions opening the
    same bundle hold the same show and different sessions.

    OWNED BY THE VERB, CAPTURED BY REFERENCE. `wfg serve` declares one beside
    its document and hands it to `registerBundleCommands`; the handlers and
    `settle` write it and the verb's after-tick reads it. A handler that
    captured it by value would stamp its own copy, the after-tick would compare
    against the original for the life of the process, and the dot would never go
    out - which is the whole difference between a session record that works and
    one that quietly does not (namespace draft §14.10).

    EIGHT FIELDS SINCE PR 5.5's SECOND HALF, and every one of them is read by
    something. Two are Phase 1's; three are the autosave's arithmetic (§14.10);
    one says where an earlier session's unanswered recovery lives, which is
    `/godot/document/recovery`; and two are what the writer thread last failed
    to do, which is `/godot/document/writeError`. All of them are facts about
    this session's relationship with its disk and therefore this record's
    business rather than the document's. A field nobody reads is a field
    somebody will read wrongly, because the only thing it can hold until then
    is its initialiser, and an initialiser looks exactly like an answer.

    THREADING: the tick thread's, like the document beside it, AND NEVER THE
    WRITER'S. The handlers, the before-tick and the after-tick all run on the
    tick thread, which is why none of them needs a lock. The writer thread
    (DocumentWriter.h) never reads or writes a byte of this record: what it
    finishes comes back as a completion, and `settle` - called on the tick
    thread - is what turns a completion into a stamp here. That is the whole of
    the synchronisation argument for this struct, and it is why the struct has
    no mutex: it has one thread. The serve verb also touches it from its own
    thread, and only where that thread cannot overlap the tick thread: at open,
    before the clock starts, and at a clean exit, after `ticks.stop()` has
    joined it and the writer has been drained.
*/

#include <wfg/engine/document/ShowDocument.h>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>

namespace wfg::doc
{
    struct DocumentSession
    {
        /*  The bundle `document.save` writes to. For `wfg serve` it is the
            folder the show was opened from; for `wfg replay` it is `--out`,
            never the bundle being checked. */
        juce::File folder;

        /*  `ShowDocument::showRevision()` as it stood when the folder last held
            this document's show: stamped once when the verb first knows which
            folder it will write to - at open for `wfg serve`, when `--out` is
            wired for `wfg replay` - and again by every `document.save` that
            reached the disk, and only by one that did; and since PR 5.5 by
            `document.revert`, which reaches the same state from the other side
            by reading the folder back rather than writing it. Never by
            `document.recover`, whose whole point is that the folder does NOT
            hold what the document now does.

            THE REVISION A SAVE STAMPS IS THE ONE ITS SNAPSHOT WAS TAKEN AT, and
            never `showRevision()` at the moment the writer says it landed (PR
            5.5, second half). The bytes leave the tick thread, so an edit can
            land between the snapshot and the confirmation - and a stamp read
            off the document then would mark that edit saved when the file on
            disk has never heard of it. The revision rides with the snapshot to
            the writer and back (`Bundle::Snapshot`), and `settle` stamps what
            came back.

            Nought is "never", which no document ever reports (its count starts
            at 1), so a session nobody stamped reads as dirty rather than as
            saved. That is the safe way round to be wrong: a false "unsaved"
            costs somebody a save, and a false "saved" costs them a show. */
        std::uint64_t savedRevision = 0;

        /*  `showRevision()` as it stood when `recovery/show.xml` was last
            written - the autosave's own equivalent of the field above, and what
            stops a dirty show that nobody is touching from writing the same
            bytes fifty times a second.

            The two conditions §14.10 tabulates - two seconds of quiet, thirty
            seconds of ceiling - say WHEN to write and never WHETHER there is
            anything to write, and the quiet one stays true for as long as the
            quiet lasts. Without this field the first autosave after a pause
            would be followed by another on the very next tick, and by another
            after that, for as long as the document stayed unsaved.

            Nought is "the recovery folder holds nothing this session wrote",
            which is what a fresh session leaves, because it has written none,
            and what a `document.save` or a `document.revert` leaves when it has
            just deleted the folder.

            Stamped, like `savedRevision`, from the revision the autosave's
            snapshot was taken at, when the writer confirms the bytes landed -
            so between the two it still names the previous autosave, which is
            the truth about what `recovery/` holds in that interval. */
        std::uint64_t autosavedRevision = 0;

        /*  The tick at which the SHOW half last moved, stamped by the serve
            verb's after-tick and by nothing else, because that is the one place
            that sees `showRevision()` before and after a tick's commands.

            It is a tick and not a wall clock for the reason everything else in
            this engine counts ticks: a replay re-injects the record carrying
            the tick it was decided at, and a session that consulted the clock
            would decide differently on the machine replaying it. */
        std::int64_t lastChangeTick = 0;

        /*  The tick at which an autosave was last ATTEMPTED - stamped by the
            handler, on the tick thread, before the bytes are handed to the
            writer and whether or not they ever land, which is the difference
            between a retry every thirty seconds and a retry fifty times a
            second.

            A refused or failed autosave leaves `autosavedRevision` where it
            was, so the "is there anything to write" test above stays true;
            what stops the before-tick submitting again on the very next tick is
            this stamp and the `lastChangeTick >= lastAutosaveTick` term in
            `autosaveDue`. A bundle on a stick somebody pulled out is then one
            refused record every thirty seconds rather than fifty a second,
            which is a log somebody can still read. The same term is what keeps
            an autosave the writer has not yet confirmed from being submitted
            twice: its stamp is taken at once, and its revision a tick or two
            later. */
        std::int64_t lastAutosaveTick = 0;

        /*  WHERE AN EARLIER SESSION'S UNANSWERED RECOVERY LIVES, and empty when
            there is none: `/godot/document/recovery`, in the one place it is
            decided (§14.10, as the author decided it on 2026-09-11).

            FOUND AT OPEN AND NEVER LOOKED FOR AGAIN. `Bundle::offeredRecovery`
            answers it once: `recovery/` when it holds a show.xml - the last
            session died - and otherwise the highest-numbered
            `recovery.previous.N/`, an afternoon an earlier session moved aside
            and nobody answered.

            A RECORD AND NEVER A LOOK AT THE DISK, and that is the whole of why
            it is a field. This session's own autosave writes `recovery/` two
            seconds after the first edit, so a node that answered by asking the
            filesystem would light up on every unsaved show and tell an operator
            that work from a previous session was waiting - which would be
            false, and would be false most of the time. What the node reports is
            somebody ELSE's unfinished afternoon: found at open, and gone only
            when this session answers it - `document.recover` adopts it and
            `document.discardRecovery` deletes it, wherever it then lives.
            Answering clears it - a recovery at once, a discard when the writer
            says the folder has gone - and the FOLDER goes when it is safe to.
            A discard deletes it on the writer. A recovery adopted from a
            `recovery.previous.N/` is consumed, and deleted by the writer once
            the first autosave or save after it has landed - not before, so a
            crash straight after the recovery still loses nothing
            (`DocumentWriter::setConsumed`; §14.10 as refined at the author's
            direction on 2026-09-11). Nothing else deletes one: a
            `recovery.previous.N/` nobody answered is offered again by the next
            session that opens the bundle without a `recovery/`.

            IT MOVES, AND THIS SESSION'S AUTOSAVE IS WHAT MOVES IT. The first
            time this session needs `recovery/` while the offer still sits
            there, the writer renames that folder to the next
            `recovery.previous.N/` and then autosaves as usual - so this
            session's edits are protected from the first quiet on, and the
            earlier afternoon is put where nothing this session does can reach
            it. The rename is the writer's, in queue order, and its address
            comes back through `settle`, which is where this field learns it;
            the writer keeps its own copy of the address, which is the one it
            acts on (`DocumentWriter::offer`), because only it knows whether
            its rename worked. In between, this field still names `recovery/`
            while the folder is being renamed, which is harmless for every
            reader it has then: the published node says an offer exists, and
            one does; `document.recover` drains the writer and settles before
            it reads the address; and `document.discardRecovery` is itself a
            job, queued behind the rename, deleting the folder wherever the
            writer's copy says it went.

            THE EMPTY BRACES ARE LOAD-BEARING, here and on the two strings
            below: every verb and test builds this record as `{ folder,
            revision }`, and a field with no initialiser of its own is one
            -Wmissing-field-initializers names at every one of those sites. The
            older fields all carry `= 0`, which is why they never did. */
        juce::File offeredRecovery {};

        /*  WHAT THE WRITER THREAD LAST FAILED TO DO, in a sentence, and empty
            once nothing it was handed has failed since a later write of the
            same kind landed: `/godot/document/writeError`.

            ITS OWN NODE AND NOT `/godot/engine/lastError`, and the difference
            is the log. `lastError` and `errorCount` quote a rejected RECORD - a
            refusal a client can find at the tick and sequence it names - and a
            write that fails on the writer has no such record: the command was
            applied, and its `A` means "taken and handed to the writer", which
            it was. Folding the failure in would make `errorCount` disagree with
            the number of refusals in the log, and make a replay - which writes
            inline, onto another disk, and usually succeeds - disagree with the
            session it reproduces.

            Named with the command and the tick it was applied at, so an
            operator can find the record it belongs to, and carrying the
            writer's own sentence, which names the file and what became of it -
            the diagnosis a reason code never had room for. */
        std::string writeError {};

        /*  Which command `writeError` is about, so that `settle` knows what
            puts it out: a later write of the same command that lands, or a
            save that lands over an autosave's failure, because a save takes
            this session's `recovery/` away and the work is the show. A landed
            autosave says nothing about a save that did not land, and a landed
            save says nothing about a copy somewhere else that did not. */
        std::string writeErrorCommand {};
    };

    /*  Whether an earlier session's recovery is waiting for an answer - the
        published node, read off the record and never off the disk. */
    inline bool hasRecoveryOffer (const DocumentSession& session)
    {
        return session.offeredRecovery != juce::File();
    }

    /*  Two seconds of quiet at 50 Hz, and a thirty-second ceiling over it.

        PLAN DECISION 5, AND HERE TO BE OVERRULED EARLY rather than late: a
        fortnight of tech will settle these two numbers better than any argument
        written before the first one. They are named here rather than spelled
        into `autosaveDue` so that the test which walks their edges and the
        engine which obeys them cannot drift apart, and so that overruling them
        is one edit in one place.

        The quiet number is the one a designer feels: a person who has stopped
        typing has finished a thought, and writing mid-drag would fire on every
        intermediate value of a slider. The ceiling is the floor under what a
        crash can cost the designer who has NOT stopped - a long drag, a bulk
        edit over forty cues - who would otherwise never be quiet enough to be
        written down at all. */
    inline constexpr std::int64_t autosaveQuietTicks   = 100;
    inline constexpr std::int64_t autosaveCeilingTicks = 1500;

    /*  `/godot/document/dirty`, in the one place it is defined.

        It asks whether the SHOW HALF has changed since the folder last held
        it, and nothing else - `showRevision()` says why a GO does not count.
        Nor is it a comparison of contents: once undo exists (PR 5.4), an edit
        and its undo will be two changes and will leave this true, because what
        it answers is "is the file on disk this document's history", not "does
        this document differ from the file" (namespace draft §14.10 and
        §14.15). */
    inline bool isDirty (const ShowDocument& document, const DocumentSession& session) noexcept
    {
        return document.showRevision() != session.savedRevision;
    }

    /*  WHETHER THE ENGINE SHOULD DECIDE, ON NOBODY'S BEHALF, TO WRITE THE SHOW
        TO `recovery/` AT THIS TICK (§14.10).

        A PURE FUNCTION, AND IN THE HEADER, because it is the whole of the
        decision and the decision is the part worth testing. It reads two
        counters and two ticks; it opens nothing, asks the clock nothing and
        touches no disk, so a test can stand `tick` on either side of each
        boundary and read the answer off - which is what a threshold deserves
        and what a decision buried in a hook would never get.

        IT IS CALLED FROM THE BEFORE-TICK, which is §14.8's correction to the
        approved plan and not a detail: `TickThread`'s before hook is drained by
        the tick it runs in front of, so a `document.autosave` submitted there
        is applied at the tick this function was asked about. Submitted from the
        after hook it would land in the queue the NEXT tick drains, and the log
        would say the save happened one tick after it did. A `juce::Timer` is
        refused twice over - Spike 05 measured a 20 ms timer on an idle message
        thread at 2.60 ms late at the 99th percentile, and, decisively, the
        message thread is not the document's thread: ShowDocument is owned by
        the tick thread, which is exactly what makes it safe to serialise the
        model without a lock.

        AN EARLIER SESSION'S UNANSWERED RECOVERY DOES NOT STOP IT, and did for
        half a pull request. The first build suspended the autosave while an
        offer stood, because the only folder it could write was the one holding
        the offer - which kept the old afternoon safe by leaving the new one
        with no crash protection until somebody answered. *Corrected
        2026-09-11, at the author's direction:* the writer moves the offer
        aside to a `recovery.previous.N/` before this session's first autosave
        lands in `recovery/` (`offeredRecovery` says how), so both afternoons
        are kept and this function no longer asks about either.

        THE FIRST TWO TESTS ARE "IS THERE ANYTHING TO WRITE" and the last two
        are "is now the moment". A show nobody has edited writes nothing at all,
        which is what makes an idle engine an idle engine; a show whose current
        revision is already the one in `recovery/` writes nothing either,
        because the bytes would be the bytes that are already there.

        `lastChangeTick >= lastAutosaveTick` is the third of those and the least
        obvious. It says there has been a change since the last ATTEMPT, and it
        is what turns a failed autosave into one refusal every thirty seconds
        instead of one every tick: the quiet term alone stays true for as long
        as the quiet lasts, so a folder that has gone away would otherwise be
        rediscovered fifty times a second. It is `>=` rather than `>` so that an
        edit landing in the same tick as an attempt is still owed its own write. */
    inline bool autosaveDue (const ShowDocument& document,
                             const DocumentSession& session,
                             std::int64_t tick) noexcept
    {
        if (! isDirty (document, session))
            return false;

        if (session.autosavedRevision == document.showRevision())
            return false;

        const auto quiet = session.lastChangeTick >= session.lastAutosaveTick
                             && tick - session.lastChangeTick >= autosaveQuietTicks;

        const auto ceiling = tick - session.lastAutosaveTick >= autosaveCeilingTicks;

        return quiet || ceiling;
    }
}
