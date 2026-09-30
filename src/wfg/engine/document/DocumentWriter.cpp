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

#include <wfg/engine/document/DocumentWriter.h>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace wfg::doc
{
    namespace
    {
        /*  One tick at 50 Hz, the first of the pauses `writeBytesAtomically`
            takes, for the same kind of cause: JUCE's `moveFileTo` does not say
            why a rename failed, and the likeliest cause is another process - an
            indexer, a virus scanner, a sync client - holding a file inside the
            folder for a few milliseconds. It is paid on this thread, never on
            the one GO shares. *(2026-09-30, fixer of H6b: this said "its one
            blind retry", because the platform would not say why - true of the
            move aside still, and of neither half of that since the file
            renames took three pauses and began to say why.)* */
        constexpr int moveRetryPauseMs = 20;

        /*  Every problem a write reported, as one line. Most carry one; a copy
            can carry the save's and the namespaces' both. */
        std::string describe (const ReadResult& result)
        {
            std::string text;

            for (const auto& problem : result.problems)
            {
                if (! text.empty())
                    text += "; ";

                text += problem;
            }

            return text;
        }

        /*  Renames an earlier session's `recovery/` to where it is kept, with
            one blind retry.

            A RENAME, NEVER A COPY: the destination is a sibling inside the same
            bundle, so on every platform this is one directory entry changing
            its name - `MoveFile` on Windows, `rename(2)` on POSIX - and the
            afternoon inside it is never read, never rewritten and never, even
            for an instant, in two places or in none. JUCE's POSIX fallback to a
            copy refuses a non-empty directory outright (`moveInternal`), so
            there is no half-copied folder for a failure to leave behind: the
            old one is where it was, whole. */
        bool moveAside (const juce::File& from, const juce::File& aside)
        {
            if (from.moveFileTo (aside))
                return true;

            juce::Thread::sleep (moveRetryPauseMs);
            return from.moveFileTo (aside);
        }

        /*  Whether a write of `kind` landing puts out a failure `failed` left.

            The same command, always: a save that lands after a save that did
            not has done what the first could not. And a SAVE over an
            AUTOSAVE's failure, because a landed save takes this session's
            `recovery/` away - the work is the show, and a folder the autosave
            could not write is no longer a folder anybody needs. Nothing else:
            an autosave that lands says nothing about a show.xml that did not,
            and a save says nothing about a copy somewhere else that was never
            made. */
        bool putsOut (WriteJob::Kind kind, const std::string& failed)
        {
            if (failed == commandNameOf (kind))
                return true;

            return kind == WriteJob::Kind::save
                && failed == commandNameOf (WriteJob::Kind::autosave);
        }
    }

    //==========================================================================
    const char* commandNameOf (WriteJob::Kind kind) noexcept
    {
        switch (kind)
        {
            case WriteJob::Kind::save:            return "document.save";
            case WriteJob::Kind::autosave:        return "document.autosave";
            case WriteJob::Kind::saveAs:          return "document.saveAs";
            case WriteJob::Kind::discardRecovery: return "document.discardRecovery";
        }

        /*  Unreachable with every enumerator handled above; here because a
            function that can fall off its end without returning is -Wreturn-type
            on GCC, whatever the switch covers. */
        return "document.save";
    }

    //==========================================================================
    DocumentWriter::~DocumentWriter()
    {
        stop();
    }

    bool DocumentWriter::start()
    {
        if (mode != Mode::background)
            return false;

        /*  UNDER THE LOCK, so that the flag `submit` and `drain` read and the
            thread that flag describes come into being together. `run` takes
            the same lock first thing, so it waits here until this returns.
            `thread` itself is touched only here and in `stop`, both on the
            verb's thread and never at once. */
        const std::lock_guard<std::mutex> lock { guard };

        if (threadRunning)
            return false;

        stopping = false;
        threadRunning = true;
        thread = std::thread ([this] { run(); });
        return true;
    }

    void DocumentWriter::stop()
    {
        {
            const std::lock_guard<std::mutex> lock { guard };

            if (threadRunning)
                stopping = true;
        }

        wake.notify_all();

        /*  The thread leaves its loop only when the queue is empty, so this
            join is also the drain: every job submitted before the stop has
            been performed by the time it returns. */
        if (thread.joinable())
            thread.join();

        {
            const std::lock_guard<std::mutex> lock { guard };
            threadRunning = false;
            stopping = false;
        }

        /*  AND ANYTHING STILL QUEUED IS PERFORMED HERE: jobs handed to a
            background writer that was never started, which is how a test holds
            a job still to look at the disk before it lands. With the thread
            gone this is the only performer, so the order is still the queue's. */
        performQueuedHere();
    }

    bool DocumentWriter::isRunning() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return threadRunning;
    }

    //==========================================================================
    void DocumentWriter::submit (WriteJob job)
    {
        if (mode == Mode::synchronous)
        {
            /*  PERFORMED HERE, ON THE CALLER, which is `wfg replay`'s tick loop
                inside the handler that asked. Nothing else performs for a
                synchronous writer, so the lock below is only for the deque the
                completions share with `takeCompletions`, and it is never
                contended. */
            auto done = perform (job);

            const std::lock_guard<std::mutex> lock { guard };
            completed.push_back (std::move (done));
            finished.store (completed.size(), std::memory_order_relaxed);
            return;
        }

        {
            /*  A PUSH AND NOTHING ELSE UNDER THE LOCK. The job was moved in,
                and a deque push of a moved job moves its strings rather than
                copying them, so the tick thread's cost here does not grow with
                the size of the show - the snapshot was the part that did, and
                it has already been paid. */
            const std::lock_guard<std::mutex> lock { guard };
            queued.push_back (std::move (job));
        }

        wake.notify_one();
    }

    void DocumentWriter::drain()
    {
        {
            std::unique_lock<std::mutex> lock { guard };

            if (threadRunning)
            {
                /*  Waits for the queue to empty AND for the job last popped to
                    finish: a writer that has taken the last job off the queue
                    is still writing it, and a revert that read show.xml then
                    would read the file the job has not yet replaced. */
                idle.wait (lock, [this] { return queued.empty() && ! busy; });
                return;
            }
        }

        performQueuedHere();
    }

    std::vector<WriteCompletion> DocumentWriter::takeCompletions()
    {
        if (finished.load (std::memory_order_relaxed) == 0)
            return {};

        /*  SWAPPED OUT UNDER THE LOCK, MOVED INTO A VECTOR OUTSIDE IT, so the
            writer can never be kept waiting behind an allocation made on the
            tick thread's behalf. */
        std::deque<WriteCompletion> taken;

        {
            const std::lock_guard<std::mutex> lock { guard };
            taken.swap (completed);
            finished.store (0, std::memory_order_relaxed);
        }

        return std::vector<WriteCompletion> (std::make_move_iterator (taken.begin()),
                                             std::make_move_iterator (taken.end()));
    }

    std::size_t DocumentWriter::pending() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return queued.size() + (busy ? std::size_t { 1 } : std::size_t { 0 });
    }

    void DocumentWriter::setOffer (const juce::File& where)
    {
        const std::lock_guard<std::mutex> lock { guard };
        offerLocation = where;
    }

    juce::File DocumentWriter::offer() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return offerLocation;
    }

    void DocumentWriter::setConsumed (const juce::File& where)
    {
        const std::lock_guard<std::mutex> lock { guard };
        consumedLocation = where;
    }

    juce::File DocumentWriter::consumed() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return consumedLocation;
    }

    void DocumentWriter::deleteConsumedRecovery()
    {
        /*  Read, deleted, then cleared - the deletion outside the lock like
            every other disk operation here. Nothing can change the location
            in between: the tick thread sets it only on a drained writer, and
            this thread is the one performing. */
        const auto spent = consumed();

        if (spent != juce::File() && Bundle::discardRecoveryAt (spent))
            setConsumed (juce::File());
    }

    bool DocumentWriter::firstJob()
    {
        const std::lock_guard<std::mutex> lock { guard };
        return ! std::exchange (markersSettled, true);
    }

    //==========================================================================
    void DocumentWriter::run()
    {
        std::unique_lock<std::mutex> lock { guard };

        for (;;)
        {
            wake.wait (lock, [this] { return stopping || ! queued.empty(); });

            /*  EVERYTHING QUEUED BEFORE A STOP IS PERFORMED BEFORE IT. The loop
                leaves only when there is nothing left to do, whether or not it
                was asked to stop - which is what makes `stop` a drain, and a
                save queued at Ctrl-C a save that lands. */
            if (queued.empty())
                return;

            auto job = std::move (queued.front());
            queued.pop_front();
            busy = true;

            /*  OUTSIDE THE LOCK, because this is the part that takes the
                twenty milliseconds. Holding it here would make `submit` - which
                runs on the tick thread - wait behind a disk, which is the one
                thing this class exists to prevent. */
            lock.unlock();
            auto done = perform (job);
            lock.lock();

            completed.push_back (std::move (done));
            finished.store (completed.size(), std::memory_order_relaxed);
            busy = false;

            idle.notify_all();
        }
    }

    void DocumentWriter::performQueuedHere()
    {
        for (;;)
        {
            WriteJob job;

            {
                const std::lock_guard<std::mutex> lock { guard };

                if (queued.empty())
                    return;

                job = std::move (queued.front());
                queued.pop_front();
                busy = true;
            }

            auto done = perform (job);

            const std::lock_guard<std::mutex> lock { guard };
            completed.push_back (std::move (done));
            finished.store (completed.size(), std::memory_order_relaxed);
            busy = false;
        }
    }

    //==========================================================================
    void DocumentWriter::setAfterLanding (std::function<void (const juce::File& folder)> toCall)
    {
        afterLanding = std::move (toCall);
    }

    WriteCompletion DocumentWriter::perform (const WriteJob& job)
    {
        auto done = write (job);

        if (afterLanding && done.landed
              && (job.kind == WriteJob::Kind::save || job.kind == WriteJob::Kind::saveAs))
            afterLanding (job.folder);

        return done;
    }

    WriteCompletion DocumentWriter::write (const WriteJob& job)
    {
        /*  THE FIRST JOB FIRST ANSWERS WHAT AN EARLIER SESSION'S SAVES LEFT
            MARKED (H6b, namespace draft §23.5). A save killed after its marks
            leaves folders the next open must judge, and `offeredRecovery`
            judges them without touching anything: a mark naming the show.xml
            on the disk is a save that landed, a mark naming anything else one
            that did not. What cannot wait past this session's first write is
            the ANSWER to the second kind, because that write may be a save of
            those very bytes: the mark would name the show.xml on the disk the
            moment it landed, and hide real work. So that folder loses its mark
            here, before anything is written. The first kind is deleted in the
            same breath, because it is litter; one that will not go is kept
            outgrown by every save after it, which marks it too (the save job,
            below). *(2026-09-30, fixer of H6b: until then this deletion was all
            that kept such a folder outgrown past the first save, and a folder
            something held at that instant came back as an offer once the save
            landed.)*

            HERE AND NOT AT OPEN, because this is the one thread that writes in
            the bundle and every write passes through it: nothing can land in
            between, every verb that writes gets it - `serve`, the window,
            `replay`'s `--out` - and the verbs that only look, `validate` and
            `tree`, submit nothing and change nothing. The saveAs job names its
            destination; the bundle it came from is its `source`. */
        if (firstJob())
            Bundle::resolveAllSuperseded (job.kind == WriteJob::Kind::saveAs ? job.source : job.folder);

        WriteCompletion done;
        done.kind = job.kind;
        done.revision = job.snapshot.revision;
        done.tick = job.tick;

        switch (job.kind)
        {
            case WriteJob::Kind::save:
            {
                /*  WHAT THIS SAVE WILL RETIRE IS MARKED BEFORE IT WRITES (H6b,
                    namespace draft §23.5): this session's own `recovery/`, and
                    a `recovery.previous.N/` it recovered from - never an
                    earlier session's unanswered offer, which it says nothing
                    about (§14.10). The folders go after the three writes below,
                    because until those have landed they hold the only copy of
                    the work. H6's driver killed the engine in between in about
                    one kill in five, and the next start offered the autosave
                    from before the save: adopted, it took the show back past
                    it. With the marks, a kill after show.xml has landed leaves
                    folders that name it, and nothing offers them; a kill before
                    leaves marks naming a show that is nowhere, and the folders
                    - newer than the show.xml still there - are offered, rightly.

                    AND EVERY FOLDER ALREADY SUPERSEDED, whoever's it was (fixer
                    of H6b): one a landed save outgrew, which the tidy-up after
                    it or a first job's answer could not delete because
                    something held its show. Its mark names the show.xml on the
                    disk now, and would name nothing once this save's show.xml
                    landed - the folder back on offer, older than two saves.
                    Marked again, it gains this save's line beside the one it
                    has (Bundle.h, `markSuperseded`), and is deleted first if it
                    will go by now. An earlier session's unanswered offer is
                    never superseded, so this reaches none.

                    A mark that cannot be written does not stop the save: the
                    show is what somebody asked for, and a missing mark costs
                    only what every save risked before H6b, the older autosave
                    offered if the engine dies in the next few milliseconds. */
                const auto ownRecovery = Bundle::recoveryFolder (job.folder);
                std::vector<juce::File> retiring;

                const auto retire = [&retiring] (const juce::File& folderToRetire)
                {
                    if (std::find (retiring.begin(), retiring.end(), folderToRetire) == retiring.end())
                        retiring.push_back (folderToRetire);
                };

                if (offer() != ownRecovery && ownRecovery.isDirectory())
                    retire (ownRecovery);

                if (const auto adopted = consumed(); adopted != juce::File() && adopted.isDirectory())
                    retire (adopted);

                for (const auto& outgrown : Bundle::supersededFolders (job.folder))
                    retire (outgrown);

                if (! retiring.empty())
                {
                    const auto fingerprint = Bundle::fingerprintOf (job.snapshot.show);

                    for (const auto& retired : retiring)
                        Bundle::markSuperseded (job.folder, retired, fingerprint);
                }

                const auto written = Bundle::save (job.folder, job.snapshot);

                done.landed = written.ok;
                done.problem = describe (written);

                /*  AND THIS SESSION'S `recovery/` GOES, AFTER THE SAVE'S OWN
                    BYTES AND BEFORE ANYTHING QUEUED BEHIND IT, because the work
                    has become the show (§14.10). Here and not on the tick
                    thread, which is the whole point: queued there, an autosave
                    submitted after this save could land first and then be
                    deleted by it.

                    NOT WHILE `recovery/` HOLDS AN EARLIER SESSION'S OFFER - a
                    save makes this session's work the show, and says nothing
                    about somebody else's afternoon - and that fact is read HERE,
                    when the job runs, rather than when it was queued: a rename
                    queued ahead of this save may have failed, and then the
                    folder is still theirs.

                    Its failure is not the save's. The bytes landed, the dot
                    will go out, and a folder that would not delete is a
                    tidiness problem; reporting the save as failed over it would
                    be a lie about the show. */
                if (written.ok && offer() != Bundle::recoveryFolder (job.folder))
                {
                    Bundle::discardRecovery (job.folder);
                    done.recoveryCleared = true;
                }

                /*  And a recovery the operator adopted goes with it: the work
                    it held is now the show (DocumentWriter.h, `setConsumed`). */
                if (written.ok)
                    deleteConsumedRecovery();

                break;
            }

            case WriteJob::Kind::autosave:
            {
                /*  Asked again here, because the tick thread asked a moment ago
                    and a bundle can go in between: the one thing an unattended
                    writer must never do is invent the folder it was told to
                    write into (Bundle.h, `saveRecovery`). */
                if (! job.folder.isDirectory())
                {
                    done.problem = job.folder.getFullPathName().toStdString()
                                 + " is no longer a folder; nothing was written";
                    break;
                }

                /*  AN EARLIER SESSION'S OFFER MOVES ASIDE FIRST (§14.10, as the
                    author decided it on 2026-09-11), in queue order and before
                    the bytes that needed the folder. Only ever here: this is the
                    one thread that can know whether its own rename worked, and a
                    job queued behind a rename that failed must find the folder
                    still marked as somebody else's. */
                if (const auto from = Bundle::recoveryFolder (job.folder); offer() == from)
                {
                    if (! from.isDirectory())
                    {
                        setOffer (juce::File());
                        done.offerWithdrawn = true;
                    }
                    else
                    {
                        const auto aside = Bundle::nextPreviousRecovery (job.folder);

                        if (! moveAside (from, aside))
                        {
                            done.problem = "could not move the earlier session's recovery in "
                                         + from.getFullPathName().toStdString() + " aside to "
                                         + aside.getFullPathName().toStdString()
                                         + "; it is where it was, and nothing was written over it";
                            break;
                        }

                        setOffer (aside);
                        done.offerMovedTo = aside;
                    }
                }

                /*  AND A MARK IN ITS OWN FOLDER IS ANSWERED BEFORE ITS BYTES GO
                    IN (H6b): a save earlier in this session marked `recovery/`
                    and its tidy-up could not finish. Written under that mark,
                    this autosave would carry a save's verdict it never had - a
                    mark naming show.xml would hide this session's newest work
                    from the next start. So the mark is answered as the first
                    write of a session answers one: naming show.xml, the save
                    landed and the folder goes, its old show first; naming
                    anything else, the save never landed, the show in the folder
                    is newer than show.xml, and only the mark goes - so that an
                    autosave which then fails leaves that work where it was,
                    still offered, rather than a folder emptied for bytes that
                    never came. A folder that cannot be cleared is written
                    nothing, and the autosave is owed again. */
                if (const auto ownRecovery = Bundle::recoveryFolder (job.folder);
                    ! Bundle::resolveSuperseded (job.folder, ownRecovery))
                {
                    done.problem = "could not clear the superseded mark in "
                                 + ownRecovery.getFullPathName().toStdString()
                                 + "; nothing was written there";
                    break;
                }

                const auto written = Bundle::saveRecovery (job.folder, job.snapshot);

                done.landed = written.ok;
                done.problem = describe (written);

                /*  A RECOVERY THE OPERATOR ADOPTED GOES ONCE ITS WORK IS IN
                    `recovery/`, and not a moment before: this snapshot was
                    taken after the adoption, so the recovered show is now on
                    the disk under a name this session owns, and the folder it
                    came from has nothing left that is not also here. */
                if (written.ok)
                    deleteConsumedRecovery();

                break;
            }

            case WriteJob::Kind::saveAs:
            {
                const auto written = Bundle::saveCopy (job.folder, job.snapshot, job.source);

                done.landed = written.ok;
                done.problem = describe (written);
                break;
            }

            case WriteJob::Kind::discardRecovery:
            {
                /*  THE OFFER WHERE IT IS NOW, which a rename queued ahead of
                    this job may have moved since the operator pressed the
                    button - and that is why a discard is a job at all rather
                    than a deletion on the tick thread: in the queue, it cannot
                    run before the rename, and so cannot delete the folder the
                    offer used to be in and leave the one it is in.

                    A FOLDER ALREADY GONE is a discard that has nothing left to
                    do, and it withdraws the offer without a word: the
                    operator asked for it gone, and it is. The tick thread
                    refused the empty gesture - no offer at all - before this was
                    queued; what reaches here is an offer that existed a moment
                    ago. */
                const auto where = offer();

                if (where == juce::File() || ! where.exists()
                    || Bundle::discardRecoveryAt (where))
                {
                    setOffer (juce::File());
                    done.landed = true;
                    done.offerWithdrawn = true;
                    break;
                }

                done.problem = "could not delete " + where.getFullPathName().toStdString()
                             + "; it is still there, and still offered";
                break;
            }
        }

        return done;
    }

    //==========================================================================
    std::vector<std::string> settle (DocumentSession& session, DocumentWriter& writer)
    {
        std::vector<std::string> problems;

        for (auto& done : writer.takeCompletions())
        {
            const std::string command = commandNameOf (done.kind);

            /*  WHERE THE OFFER LIVES, first and whatever else happened: an
                autosave whose own bytes failed after its rename worked has
                still moved the offer, and a session that went on believing it
                was in `recovery/` would answer the wrong folder. */
            if (done.offerMovedTo != juce::File())
                session.offeredRecovery = done.offerMovedTo;

            if (done.offerWithdrawn)
                session.offeredRecovery = juce::File();

            /*  THE STAMPS, ONLY FOR BYTES THAT LANDED, and with the revision
                the snapshot was taken at - which the job carried there and the
                completion carried back. Never `showRevision()` read now: an
                edit made while the write was in flight is not on the disk, and
                a stamp taken now would mark it saved. */
            if (done.landed)
            {
                switch (done.kind)
                {
                    case WriteJob::Kind::save:
                        session.savedRevision = done.revision;

                        if (done.recoveryCleared)
                            session.autosavedRevision = 0;

                        break;

                    case WriteJob::Kind::autosave:
                        session.autosavedRevision = done.revision;
                        break;

                    case WriteJob::Kind::saveAs:
                        /*  A copy stamps nothing: the session's own folder is
                            still behind, and the dot says so. */
                        break;

                    case WriteJob::Kind::discardRecovery:
                        /*  Nor does a discard: it deleted somebody else's
                            afternoon, which says nothing about this session's
                            show or its own `recovery/`. What it changes is the
                            offer, withdrawn above. */
                        break;
                }
            }

            if (! done.problem.empty())
            {
                session.writeError = command + " at tick " + std::to_string (done.tick)
                                   + ": " + done.problem;
                session.writeErrorCommand = command;
                problems.push_back (session.writeError);
            }
            else if (done.landed && putsOut (done.kind, session.writeErrorCommand))
            {
                session.writeError.clear();
                session.writeErrorCommand.clear();
            }
        }

        return problems;
    }

    void finishSession (const ShowDocument& document, DocumentSession& session,
                        DocumentWriter& writer)
    {
        writer.stop();
        settle (session, writer);

        if (! isDirty (document, session)
            && session.offeredRecovery != Bundle::recoveryFolder (session.folder))
            Bundle::discardRecovery (session.folder);
    }
}
