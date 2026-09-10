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
    its document and hands it to `registerBundleCommands`; the save handler
    writes `savedRevision` and the verb's after-tick reads it. A handler that
    captured it by value would stamp its own copy, the after-tick would compare
    against the original for the life of the process, and the dot would never go
    out - which is the whole difference between a session record that works and
    one that quietly does not (namespace draft §14.10).

    SIX FIELDS SINCE PR 5.5, and every one of them is read by something. Two
    are Phase 1's; three are the autosave's arithmetic (§14.10); the sixth is
    the latch behind `/godot/document/recovery`, which is a fact about this
    session's relationship with its disk and therefore this record's business
    rather than the document's. A field nobody reads is a field somebody will
    read wrongly, because the only thing it can hold until then is its
    initialiser, and an initialiser looks exactly like an answer.

    THREADING: the tick thread's, like the document beside it. The save handler,
    the autosave handler, the before-tick and the after-tick all run there,
    which is why none of them needs a lock. The serve verb also touches it from
    its own thread, and only where that thread cannot overlap the tick thread:
    at open, before the clock starts, and at a clean exit, after
    `ticks.stop()` has joined it.
*/

#include <wfg/engine/document/ShowDocument.h>

#include <juce_core/juce_core.h>

#include <cstdint>

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
            and what a `document.save`, a `document.revert` or a
            `document.discardRecovery` leaves when it has just deleted the
            folder. */
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
            handler whether the bytes landed or not, which is the difference
            between a retry every thirty seconds and a retry fifty times a
            second.

            A rejected autosave leaves `autosavedRevision` where it was, so the
            "is there anything to write" test above stays true; what stops the
            before-tick submitting again on the very next tick is this stamp and
            the `lastChangeTick >= lastAutosaveTick` term in `autosaveDue`. A
            bundle on a stick somebody pulled out is then one refused record
            every thirty seconds rather than fifty a second, which is a log
            somebody can still read. */
        std::int64_t lastAutosaveTick = 0;

        /*  Whether a `recovery/show.xml` was found when this session opened its
            bundle, and has been neither adopted nor discarded since:
            `/godot/document/recovery`, in the one place it is decided.

            A LATCH AND NEVER A LOOK AT THE DISK, and that is the whole of why
            it is a field. This session's own autosave creates `recovery/` two
            seconds after the first edit, so a node that answered by asking the
            filesystem would light up on every unsaved show and tell an operator
            that work from a previous session was waiting - which would be
            false, and would be false most of the time. What the node reports is
            somebody ELSE's unfinished afternoon: found at open, and gone only
            when this session answers it - by recovering it or discarding it.

            The refusals do ask the disk, and the asymmetry is deliberate:
            `document.recover` and `document.discardRecovery` answer
            `no-recovery` when there is nothing THERE, because refusing a
            gesture on the strength of a flag while the folder sits in front of
            the operator is how a client ends up unable to clean up after
            itself.

            WHILE IT IS SET, NOTHING THIS SESSION DOES MAY DESTROY WHAT IT GUARDS.
            The folder holds an afternoon somebody else abandoned and nobody has
            yet decided about, and four writers would otherwise decide for
            them: the autosave, which would overwrite it with this session's
            work two seconds after the first edit; `document.save` and
            `document.revert`, which delete the folder because the document and
            the disk now agree - which says something about THIS session's
            autosave and nothing about somebody else's; and the clean exit,
            which deletes it when the document is not dirty. So `autosaveDue`
            answers no, the save and the revert leave the folder where it is,
            and the exit leaves it too, until the operator answers the question
            with `document.recover` or `document.discardRecovery`. The cost is
            that an unanswered offer leaves this session's own edits without a
            recovery copy - which is why the node is published, so a client can
            put the question in front of somebody rather than let it sit. (A
            decision §14.10 did not take, taken here and reported with the PR.) */
        bool recoveryFound = false;
    };

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

        THE FIRST TEST IS "MAY IT WRITE AT ALL": not while an earlier session's
        recovery is waiting for an answer, because the only file it could write
        is the one that holds that answer's subject (`recoveryFound` says why).

        THE NEXT TWO ARE "IS THERE ANYTHING TO WRITE" and the last two are "is
        now the moment". A show nobody has edited writes nothing at all, which
        is what makes an idle engine an idle engine; a show whose current
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
        if (session.recoveryFound)
            return false;

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
