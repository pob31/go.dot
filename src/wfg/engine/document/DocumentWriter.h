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
    The thread that writes, so that the thread GO shares never waits for a
    disk (PRD §4.1, namespace draft §14.10 and §14.14).

    WHY A THREAD AT ALL. M23 timed the autosave handler on the 500-cue show at
    21 ms at the median and 26 ms at the 99th percentile - a full tick, against
    a threshold of a quarter of one - and 1.4 ms of that was Go.dot
    serialising. The other nineteen are the durability: `FlushFileBuffers` and
    `ReplaceFile`, twice, with real-time antivirus scanning each replaced file
    on the machine that measured it. That is the platform's answer, and the
    one kind no faster Go.dot can move. So the tick thread keeps the part it
    must keep - the snapshot, because the document's single writer is what
    makes reading it safe - and the bytes come here. A `document.save` follows
    the autosave, at the author's direction (2026-09-11): it is a person's
    gesture rather than the engine's, but it is the same write, and a save
    pressed during a show would cost the GO path exactly what an autosave did.

    MOUNTPROBE'S SHAPE, because that shape is already argued in this engine:
    one `std::thread`, one mutex, one condition variable, one deque, `start`
    and `stop`, and the slow work outside the lock. The tick thread takes the
    lock only to push a job or to take the finished ones, never while a disk
    is being written, so the longest it can wait for this class is the time the
    writer holds the lock to pop a job or push a result - microseconds.

    ONE WRITER, FIFO, AND THAT IS THE WHOLE ORDERING ARGUMENT. A save and an
    autosave queued in that order land in that order; a save's deletion of this
    session's `recovery/` happens here, after the save's own bytes and before
    the bytes of anything queued behind it; the rename that moves an earlier
    session's recovery aside happens here, in queue order, before the autosave
    that needed it; and a discard of that recovery happens here too, behind any
    rename queued before it, so it deletes the folder where the rename put it
    and not where it used to be. Two writers would need a protocol between
    them. One needs none.

    WHAT IT HOLDS OF ITS OWN are the two facts it cannot be handed per job.
    Where an earlier session's unanswered recovery lives on the disk, at this
    point in the queue: the tick thread knows it at open and says so once
    (`registerBundleCommands`); after that only this thread can know whether
    its own rename worked, and a job queued behind a rename that FAILED must
    neither write into `recovery/` nor delete it - the folder is still
    somebody else's afternoon. And which adopted recovery is waiting to be
    deleted once the work it held has landed somewhere safe, which only this
    thread can know has happened. So both live here, under the lock, and every
    job that touches a recovery folder reads them when it runs rather than when
    it was queued.

    IT NEVER TOUCHES THE DOCUMENT OR THE SESSION. A job is plain values - the
    snapshot's two strings and its revision, the folders it names, the tick it
    was applied at - owned outright by whichever thread holds it; a completion
    is plain values going the other way. The session is stamped from a
    completion by `settle`, on the tick thread, which is what keeps the tick
    thread the only writer of `DocumentSession` and of the show.

    TWO MODES, AND REPLAY IS WHY. `background` is `wfg serve`'s: jobs queue,
    and the thread performs them once started. `synchronous` is `wfg replay`'s:
    every job is performed on the caller, inside the handler that submitted it,
    because a replay has no GO path to protect and a thread there would only add
    nondeterminism. The record is identical either way (§14.14: the fallback
    changes no record and no fixture), and so is what lands on the disk, in the
    same order.
*/

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/ShowDocument.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg::doc
{
    //==========================================================================
    /*  One write, taken on the tick thread and performed on the writer.

        PLAIN VALUES, COPIED OR MOVED IN, AND NOTHING SHARED. The two strings
        are the snapshot's own; each `juce::File` is a path held in a
        `juce::String`, whose storage is shared between copies by an ATOMIC
        reference count and copied before any change - so a copy handed across
        under the lock is a value each thread owns, and neither thread ever
        modifies one the other is reading. */
    struct WriteJob
    {
        /*  Three writes and one deletion. `discardRecovery` is here rather than
            on the tick thread because the lock does not refuse it - it touches
            no show - so it can be asked for during a performance, and a command
            that could be asked for then must not wait for a disk. */
        enum class Kind { save, autosave, saveAs, discardRecovery };

        Kind kind = Kind::save;

        /*  `save`, `autosave` and `discardRecovery`: the bundle. `saveAs`: the
            destination, already resolved on the tick thread, where the working
            directory and the replay's copies folder were known. */
        juce::File folder;

        /*  `saveAs` only: the bundle whose `namespaces/` the copy carries. */
        juce::File source;

        Bundle::Snapshot snapshot;

        /*  The tick the command was applied at, so that a failure can name the
            record it belongs to. */
        std::int64_t tick = 0;
    };

    /*  What one job came to, going back the other way. */
    struct WriteCompletion
    {
        WriteJob::Kind kind = WriteJob::Kind::save;

        /*  The snapshot's revision, back from the writer untouched: what a save
            that landed stamps as saved, and never `showRevision()` at the time
            it is read (DocumentSession.h, `savedRevision`). */
        std::uint64_t revision = 0;

        std::int64_t tick = 0;

        /*  Whether the bytes reached the disk: for a save, show.xml, state.xml
            and the manifest, each in its own place; for an autosave, both
            files in `recovery/`; for a copy, the save half; for a discard, that
            the offer's folder is gone. */
        bool landed = false;

        /*  The writer's own sentence when something went wrong, naming the
            file - empty when nothing did. A copy can land and still carry one:
            a `namespaces/` that would not follow is reported without
            withholding the show (Bundle.h, `saveCopy`). */
        std::string problem;

        /*  An autosave that had to move an earlier session's recovery aside
            first: where it went. Reported whether or not the autosave's own
            bytes then landed, because the offer has moved either way. */
        juce::File offerMovedTo;

        /*  The offer is gone: a discard deleted it, or a job went to act on it
            and found no folder there - somebody deleted it by hand while the
            engine ran. There is then nothing left to protect and nothing to
            offer, and saying so is what stops every later autosave failing on
            a rename of nothing, one failure every thirty seconds for the rest
            of the show, with this session's own work unprotected all the
            while. */
        bool offerWithdrawn = false;

        /*  A save that landed and took this session's `recovery/` with it. */
        bool recoveryCleared = false;
    };

    /*  The command a job belongs to, as the log spells it. */
    const char* commandNameOf (WriteJob::Kind kind) noexcept;

    //==========================================================================
    class DocumentWriter
    {
    public:
        enum class Mode { background, synchronous };

        explicit DocumentWriter (Mode modeToUse = Mode::synchronous) : mode (modeToUse) {}

        /** Drains and joins, like `stop`. */
        ~DocumentWriter();

        DocumentWriter (const DocumentWriter&) = delete;
        DocumentWriter& operator= (const DocumentWriter&) = delete;

        /*  Starts the thread. `background` only; false when already running,
            and false for a synchronous writer, which has no thread to start.

            Called from the verb's thread before the tick thread starts, and
            never concurrently with anything else here. */
        bool start();

        /*  Performs everything queued, in order, and then joins.

            A SAVE QUEUED AT CTRL-C LANDS, which is why this drains rather than
            abandons: the serve verb calls it after `ticks.stop()` has joined
            the only thread that submits, and before the clean-exit tidy-up
            that asks whether the document is dirty - a question whose answer
            is only true once the last save has landed and been settled. A
            kill loses what was queued, and that is accepted: the atomic write
            leaves every file either old and whole or new and whole.

            Never concurrent with `submit`: the thread that submits has been
            joined before this is called. */
        void stop();

        bool isRunning() const;

        /*  Tick thread. Hands a job over, or - synchronously - performs it now.

            In the background mode it takes the lock for a push and a notify
            and returns: the snapshot inside the job was moved, not copied, so
            nothing here is proportional to the size of the show. */
        void submit (WriteJob job);

        /*  Tick thread. Returns when every job submitted so far has been
            performed, and its completion is waiting to be settled.

            THE ONE PLACE THE TICK THREAD WAITS FOR THIS CLASS, and the wait is
            bounded by what is queued - a handful of jobs at most, each one
            write-flush-replace. It is for the two commands that read the disk,
            `document.revert` and `document.recover`, and Bundle.cpp argues why
            it is acceptable for them and not for the GO path.

            A background writer that has not been started, or has been stopped,
            performs what is queued on the caller instead of waiting for a
            thread that is not there: a job is never left in a queue nobody
            reads, and a drain can never wait for ever. */
        void drain();

        /*  Tick thread. Everything performed since the last call, in the order
            it was submitted - which is the order it landed in. Costs one
            relaxed atomic read, and no lock, when there is nothing. */
        std::vector<WriteCompletion> takeCompletions();

        /*  Jobs queued or being performed. Diagnostics, and tests. */
        std::size_t pending() const;

        /*  WHERE AN EARLIER SESSION'S UNANSWERED RECOVERY LIVES, as of the
            writer's place in the queue, and an empty File when there is none.

            Set once by `registerBundleCommands`, before any job. Moved by this
            thread when its own rename puts the offer aside, and emptied by it
            when a discard deletes the folder or a job finds it gone. Emptied by
            the tick thread in one place, `Bundle::adoptOfferedRecovery` -
            `document.recover`'s act and `wfg serve --recover`'s - which runs
            only on a writer that is drained or not yet started, so the writer
            is idle when it changes under it. Guarded by the lock either way, so
            that the reader and the writer of it never need to reason about
            which thread they are on; and a `juce::File` crosses the lock as a
            copy, whose path storage is shared by an atomic reference count and
            never modified in place. */
        void setOffer (const juce::File& where);
        juce::File offer() const;

        /*  A `recovery.previous.N/` THE OPERATOR HAS ADOPTED, and which is
            waiting for the recovered work to be somewhere safe before it goes;
            an empty File when there is none (§14.10, as refined at the author's
            direction on 2026-09-11).

            RECOVERING IS AN ANSWER TOO, so the folder it answered is consumed -
            but not deleted by the recovery itself, because a crash straight
            after `document.recover` must still lose nothing, and at that
            instant the recovered show exists only in memory and in this folder.
            It is deleted HERE, on this thread and in queue order, by the first
            autosave or save that LANDS after the recovery: either one puts the
            recovered work on the disk under a name this session owns, and every
            job queued after the recovery was snapshotted after it, since the
            recovery drained the queue first. A deletion that fails leaves it
            for the next landing to try again. A recovery adopted from
            `recovery/` itself consumes nothing: that folder simply becomes this
            session's own, and its next autosave writes over it.

            Set by the tick thread only, and only on a drained writer - by
            `Bundle::adoptOfferedRecovery` - and cleared by this thread. */
        void setConsumed (const juce::File& where);
        juce::File consumed() const;

    private:
        /*  The slow part, on whichever thread performs: the writer in the
            background mode, the caller otherwise - and never both at once, which
            is what makes the jobs' effects on the disk land in queue order.
            Takes the lock only to read or move the offer above. */
        WriteCompletion perform (const WriteJob& job);

        /*  Performs everything queued on the calling thread. Only when no
            thread is running, which the callers establish. */
        void performQueuedHere();

        /*  After a write that put the show on the disk: deletes the consumed
            recovery, if there is one. On the performing thread. */
        void deleteConsumedRecovery();

        void run();

        const Mode mode;

        /*  EVERYTHING BELOW THE MUTEX IS GUARDED BY IT, and every access to it
            is made with the lock held - by the tick thread pushing a job or
            taking completions, by the writer popping a job or pushing a
            result, by `drain` waiting. Three exceptions, each argued: `mode`
            above, which is const; `thread`, which only `start` and `stop`
            touch, both on the verb's thread and never at once; and `finished`,
            which is a hint and says so where it is declared. */
        mutable std::mutex guard;

        /*  Wakes the writer: a job arrived, or it is time to stop. */
        std::condition_variable wake;

        /*  Wakes a `drain`: a job has been performed. */
        std::condition_variable idle;

        std::deque<WriteJob> queued;
        std::deque<WriteCompletion> completed;

        /*  A job has been popped and is being performed. `drain` waits for the
            queue to be empty AND this to be false, or it would return while the
            last job was still half on the disk. */
        bool busy = false;

        bool stopping = false;
        bool threadRunning = false;
        juce::File offerLocation;
        juce::File consumedLocation;

        /*  HOW MANY COMPLETIONS ARE WAITING, AS A HINT AND NEVER A GUARANTEE.
            The after-tick asks fifty times a second and the answer is almost
            always nought, so it reads this and takes no lock when it is. It is
            written with the lock held, after the completion is in the deque;
            the deque itself is only ever read under the lock, so the atomic
            carries no data and needs no ordering. A stale nought costs one
            tick's delay in a stamp - the completion is still there next time;
            a stale count costs one lock. And after a `drain`, which takes the
            same lock the writer released, nothing written before that release
            can be stale at all. */
        std::atomic<std::size_t> finished { 0 };

        std::thread thread;
    };

    //==========================================================================
    /*  TICK THREAD. Applies what the writer has finished to the session.

        THE ONE PLACE A WRITE BECOMES A STAMP, and so the one place the session
        hears from the writer at all. Called by the serve verb's after-tick
        before `dirty` is read - so a save that landed during this tick puts the
        dot out in this tick's snapshot - and by the two commands that drain,
        right after they do, so they act on the session as the disk now is.

        What each completion does, in the order they landed:
          * a save that landed stamps `savedRevision` with its snapshot's
            revision, and zeroes `autosavedRevision` when it took `recovery/`
            with it;
          * an autosave that landed stamps `autosavedRevision` likewise;
          * one that moved the offer aside moves `offeredRecovery` with it;
          * a discard that deleted the offer, or any job that found its folder
            gone, withdraws it - which is when `/godot/document/recovery`
            goes false, a tick or two after the gesture and never before the
            folder has actually gone;
          * anything that failed leaves every stamp where it was - the dot
            stays lit, truthfully - and sets `writeError`.

        Returns the problems it settled, for a caller that prints them - `wfg
        replay`, which has no tree to publish them in. */
    std::vector<std::string> settle (DocumentSession& session, DocumentWriter& writer);

    /*  THE CLEAN EXIT'S TIDY-UP, after the tick thread has been joined.

        Drains and stops the writer - a save queued at Ctrl-C lands - settles
        what it finished, and then deletes this session's own `recovery/` when
        the document is not dirty, because a folder holding nothing anybody
        could want would greet the next start with an offer to restore work that
        is already in show.xml. NOT when dirty: a tidy shutdown with unsaved
        work is precisely what the folder is for. And never when `recovery/`
        still holds an earlier session's unanswered offer, which is not this
        session's to tidy; a `recovery.previous.N/` is never touched at all.

        Here rather than in the serve verb so that a test can call the same
        decision the verb makes. */
    void finishSession (const ShowDocument& document, DocumentSession& session,
                        DocumentWriter& writer);
}
