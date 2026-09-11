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
    A show on disk: a folder, not a file.

        MyShow/
          MyShow.wfg      the manifest - the file you double-click
          show.xml        what someone decided (PRD §4.10)
          state.xml       where the engine had got to
          namespaces/     the OSCQuery descriptions the mounts read
          recovery/       what nobody decided yet: the autosave (PR 5.5)
          recovery.previous.N/
                          an earlier session's recovery, moved aside unanswered
                          so that this session's autosave could run (PR 5.5)

    A FOLDER RATHER THAN AN ARCHIVE, because everything in it is text that
    someone will eventually want to diff, grep, or put under version control -
    and because a show that has crashed mid-write should lose one file, not all
    of them. It follows WFS-DIY's project folder, which works.

    NOTHING IN A BUNDLE RECORDS WHEN OR WHERE IT WAS WRITTEN. No timestamp, no
    writer version, no machine name. That is what makes `open` then `save`
    byte-identical, which in turn is what lets a replay compare its result
    against the saved bundle directly instead of through a normaliser that
    strips the parts that were always going to differ. spatcore's XmlPersistence
    writes a `<!-- Created: -->` header and its harness has to strip it; this
    does not have the problem to solve.

    THE MANIFEST CARRIES A VERSION AND NOTHING ELSE. It exists so that a
    double-click, a file association and a recent-documents list have something
    to point at, and so a folder can say what it is without being opened. Any
    other content would be a second place to look for something show.xml
    already says.
*/

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wfg::doc
{
    class DocumentWriter;

    namespace Bundle
    {
        /*  WHAT A WRITE WRITES, taken on the tick thread and handed to another.

            THE BYTES AND THE REVISION THEY ARE, AND NOTHING THAT POINTS BACK
            INTO THE DOCUMENT. Serialising has to happen on the tick thread,
            because the document has one writer and that is what makes reading
            it without a lock safe at all (ShowDocument.h); writing the bytes
            need not, and since M23 (§14.14) does not. So the handoff is plain
            values - two strings and an integer - which the writer thread owns
            outright once it has them: no reference, no handle into a
            `juce::ValueTree`, nothing whose lifetime or reference count the two
            threads would then have to share.

            `revision` is `showRevision()` at the instant the bytes were taken,
            and it travels to the writer and back so that a save which lands is
            stamped with the show it wrote rather than with whatever the
            document has become by the time the disk says yes. */
        struct Snapshot
        {
            std::string show;
            std::string state;
            std::uint64_t revision = 0;
        };

        /*  The tick thread's half of every write: one canonical serialisation
            of each file, which M23 measured at 1.4 ms on the 500-cue show. */
        Snapshot snapshotOf (const ShowDocument& document);

        /** `<folder>/<folder name>.wfg`. */
        juce::File manifestFile (const juce::File& folder);
        juce::File showFile (const juce::File& folder);
        juce::File stateFile (const juce::File& folder);
        juce::File namespacesFolder (const juce::File& folder);

        /*  Where a write to `target` puts its bytes before they become
            `target`: `<name>.tmp-<pid>`, beside it, in the same directory.

            Public because a test has to be able to stand something in its way;
            nothing else should need it. The suffix goes AFTER the name, so a
            temp left beside a manifest is `MyShow.wfg.tmp-<pid>` and is never
            mistaken for a second manifest by `open`'s `*.wfg` search. */
        juce::File temporaryFor (const juce::File& target);

        /*  Reads a bundle into `document`.

            `ok` means THE SHOW LOADED and the document is usable. `problems`
            can be non-empty even so, and the two are not the same question:
            a missing state.xml is silent, a stale entry in one is a problem
            worth printing that costs nobody their show, and an unreadable
            show.xml is neither - it sets ok false and the document is left
            exactly as it was.

            The asymmetry is deliberate and it is the whole design of this
            layer: refuse what risks someone's work, report what does not. */
        ReadResult open (const juce::File& folder, ShowDocument& document);

        /*  Writes the manifest, show.xml and state.xml, creating the folder if
            it is not there.

            It does NOT touch `namespaces/`: those files describe other people's
            programs, Go.dot only reads them, and a save that rewrote them would
            be claiming an authorship it does not have. A bundle opened and
            saved keeps whatever was in that folder, untouched.

            EACH FILE IS REPLACED, NEVER REWRITTEN IN PLACE (since PR 5.2): the
            bytes go to a sibling temp and the temp takes the file's place, so
            a save that fails or is interrupted leaves the file it was replacing
            whole. What that does and does not promise is argued at
            `writeBytesAtomically` in Bundle.cpp.

            TWO SPELLINGS, ONE WRITE. The snapshot one is what the writer thread
            calls: it reads nothing but its arguments and the disk, so it can
            run anywhere. The document one is the snapshot one with the snapshot
            taken first, for the callers that have no clock to protect - `wfg
            replay` seeding its `--out`, and the tests. */
        ReadResult save (const juce::File& folder, const ShowDocument& document);
        ReadResult save (const juce::File& folder, const Snapshot& snapshot);

        //======================================================================
        /*  `recovery/`, and the two files in it: WHAT NOBODY DECIDED YET.

            The autosave writes here and NEVER to the authored pair, and that is
            not a matter of taste (§14.10). A designer who spent an afternoon of
            tech moving cues and then decides the afternoon was wrong must be
            able to throw it away by not saving, and an autosave that wrote into
            show.xml would take that gesture away - silently, and from the person
            who most wanted it. PRD §4.10 says the document holds what someone
            decided; nobody decided to save.

            INSIDE THE BUNDLE rather than beside it, so a bundle carried to
            another machine carries its own unfinished work. It disturbs nothing
            that reads the folder: `contentHash` covers show.xml, state.xml and
            the non-recursive contents of namespaces/ and nothing else, so a log
            header is the same header with a recovery folder present or absent,
            and a replay of a session that autosaved still matches the bundle it
            was recorded against. */
        juce::File recoveryFolder (const juce::File& folder);
        juce::File recoveryShowFile (const juce::File& folder);
        juce::File recoveryStateFile (const juce::File& folder);

        /** Whether `recovery/` holds anything to adopt: a `recovery/show.xml`,
            which is the file the state beside it is meaningless without. */
        bool hasRecovery (const juce::File& folder);

        /*  THE RECOVERY A SESSION OPENING THIS BUNDLE OFFERS, or an empty File
            when there is none (§14.10, as the author decided it on
            2026-09-11).

            `recovery/` when it holds a show.xml, because then the last session
            died with work in it; otherwise the highest-numbered
            `recovery.previous.N/`, an afternoon an earlier session moved aside
            unanswered - newest first, so the one offered is the one most
            recently abandoned, and the others wait their turn. A
            `recovery.previous.N/` is offered as a FOLDER, whatever is left in
            it: `document.discardRecovery` deletes a folder, and one whose show
            has gone is still something to delete; `document.recover` asks for
            the show inside and refuses when it is not there. */
        juce::File offeredRecovery (const juce::File& folder);

        /*  Every `recovery.previous.N/` in the bundle, lowest N first. Folders
            only: a file that happens to carry the name is nothing anybody
            moved aside. */
        std::vector<juce::File> previousRecoveries (const juce::File& folder);

        /*  Where the next move aside goes: `recovery.previous.N/` with N one
            past the highest in use.

            A COUNTER AND NEVER A CLOCK, because nothing that decides where a
            show's bytes go may read one (Engine.h). And ONE PAST THE HIGHEST
            rather than the lowest gap, which is the reading of "the first free
            N" that keeps "the highest is the newest" true: every gap the engine
            itself makes is at the top - the offer is always the highest, and a
            discard deletes the offer - so the two readings agree on every
            folder this engine leaves, and differ only when somebody has
            deleted a middle one by hand, where the lowest gap would file the
            newest afternoon under the oldest number. Files are counted as
            well as folders, so that the rename can never land on a file
            somebody put there under that name. */
        juce::File nextPreviousRecovery (const juce::File& folder);

        /*  Writes the show and the state into `recovery/`, atomically, exactly
            as `save` writes the authored pair.

            NO MANIFEST, because the folder is not a bundle and must never be
            opened as one - it holds a snapshot of a document whose bundle is
            its own parent, and a `.wfg` in it would be a second show for
            anything walking a disk looking for shows.

            IT REFUSES A FOLDER THAT HAS GONE, which is the one place it differs
            from `save` and the difference is the point. `save` calls
            `createDirectory`, parents and all, because somebody asked for it: a
            save against a bundle that was moved mid-session makes a new one at
            the old path, holding three files and no media, and that is a
            surprise a person can see and undo. Autosave is unattended and
            decides on its own, and an unattended writer that invents a folder
            is how a bundle acquires a twin - one on the disk with the media and
            one at the old path with none, and nothing to say which the operator
            was working in. So a bundle that has gone is `write-failed` and the
            engine says so once every thirty seconds until somebody looks.

            The `recovery/` subfolder itself IS created, and that is not the
            same act: it is a child of a bundle that exists, named by this
            engine, holding files only this engine reads.

            It does NOT move an earlier session's offer aside: that is the
            writer's decision, taken from a fact only the writer holds
            (DocumentWriter.h), and made before it calls this. */
        ReadResult saveRecovery (const juce::File& folder, const ShowDocument& document);
        ReadResult saveRecovery (const juce::File& folder, const Snapshot& snapshot);

        /*  Reads `recovery/show.xml`, and the state beside it, into `document`.

            Same asymmetry as `open` and for the same reason: an unreadable
            show refuses and leaves the document exactly as it was - a torn
            autosave, the half-written file this whole design exists to make
            impossible and would still rather survive - while a state file that
            cannot be applied is a problem worth reporting and costs nobody
            their work. */
        ReadResult openRecovery (const juce::File& folder, ShowDocument& document);

        /*  The same, from a recovery folder named outright - `recovery/` or a
            `recovery.previous.N/` - which is what `document.recover` adopts
            since an offer can live in either. The argument is the RECOVERY
            folder, not the bundle: the two spellings take different folders
            and are different names so that nobody hands one the other's. */
        ReadResult openRecoveryAt (const juce::File& recovery, ShowDocument& document);

        /** Deletes `recovery/` and everything in it. True when the folder is
            gone afterwards, which includes it never having been there. */
        bool discardRecovery (const juce::File& folder);

        /** The same for a recovery folder named outright, which is how an offer
            living in a `recovery.previous.N/` is discarded. */
        bool discardRecoveryAt (const juce::File& recovery);

        /*  ADOPTS THE RECOVERY THIS SESSION OFFERS: `document.recover`'s act and
            `wfg serve --recover`'s, in one place so that the two cannot come to
            mean different things.

            It reads the offer wherever it now lives into `document`, through a
            scratch document, and on success answers it: the offer is cleared on
            the session and on the writer, and what `recovery/` is from then on
            follows from where the bytes came from. From `recovery/` itself,
            that folder already holds exactly this show and becomes the
            session's own - `autosavedRevision` says so. From a
            `recovery.previous.N/`, `recovery/` holds nothing of the show now on
            screen, so `autosavedRevision` goes to nought and the next quiet
            writes the recovered show there; and the folder it came from is
            CONSUMED - deleted by the writer once that autosave, or a save, has
            landed, and not before, so that a crash straight after this loses
            nothing (§14.10, as refined at the author's direction on
            2026-09-11).

            Refuses, touching nothing, when there is no offer or its show
            cannot be read; withdraws the offer when its folder has gone. The
            dot is left lit either way: `savedRevision` is never touched here.

            THE WRITER MUST BE IDLE - drained, or not yet started - because this
            reads a folder the writer might otherwise be renaming, and changes
            two facts the writer acts on. */
        ReadResult adoptOfferedRecovery (ShowDocument& document, DocumentSession& session,
                                         DocumentWriter& writer);

        /*  `save` into `destination`, PLUS the directory copy `save` refuses to
            make: `source`'s `namespaces/`.

            SAVE-PLUS-A-COPY RATHER THAN ONE ACT, and it says so in its name.
            `save` writes exactly three files and refuses `namespaces/` as a
            principle - those files describe other people's programs and Go.dot
            only reads them - so this copies them beside the save rather than
            pretending the copy is part of it. It takes two folders for the same
            reason: the descriptions are not in the document and cannot be
            written out of it, so they have to be copied from the bundle they
            were read from.

            `media/` is NOT copied, and `recovery/` is not either. Media is the
            heavy half of a bundle and a gesture that silently duplicated
            gigabytes is a gesture nobody uses twice; a recovery folder is one
            session's unfinished afternoon and would arrive in the copy as work
            somebody had abandoned somewhere else. */
        ReadResult saveCopy (const juce::File& destination, const ShowDocument& document,
                             const juce::File& source);
        ReadResult saveCopy (const juce::File& destination, const Snapshot& snapshot,
                             const juce::File& source);

        //======================================================================
        /*  A SHA-256 over everything a session READ: show.xml, state.xml and
            every file in namespaces/.

            The event log's header carries it so a replay can REFUSE a log that
            was recorded against a different show. Without it, replaying the
            wrong log against the right bundle produces divergence after
            divergence and says nothing about the cause; with it, the first
            line of the file says the two do not belong together.

            The namespace files are in it because they are read too. A mount
            describes somebody else's box, and a session that read one
            description behaves differently from one that read another - so a
            replay against a changed description is not a replay.

            Deterministic: files in sorted order, each contributing its
            bundle-relative path, its length and its bytes. The path is in there
            so that renaming a namespace file changes the hash; the length is in
            there so that no concatenation of two files can look like another
            pair. Empty when the folder cannot be read. */
        std::string contentHash (const juce::File& folder);

        /** The `# ` header lines an event log should carry for this bundle. */
        std::vector<std::string> logHeaderLines (const juce::File& folder);

        /*  The `# ` header line a session that started with `wfg serve
            --recover` carries: `recovered <folder>/`, the recovery it adopted
            before its first record, named relative to the bundle.

            A MARK, BECAUSE THERE IS NO RECORD TO POINT AT. `document.recover`
            is a command and leaves an `A` a replay can stop at; `--recover`
            adopts before the log has a single record, so without this line a
            replay of that session diverges from tick nought with nothing in the
            file to say why - the one kind of divergence that looks exactly
            like an engine that is not deterministic. */
        std::string recoveredLogHeaderLine (const juce::File& folder, const juce::File& adopted);

        /*  HOW FAR A LOG CAN BE REPLAYED AGAINST ITS BUNDLE, and why no further
            (§14.10, as the author decided it on 2026-09-11).

            A session that adopted a recovery ran a show whose bytes are in
            neither the log nor the bundle the log's header hashes: the
            recovered show.xml is not part of `contentHash`, and was never
            meant to be. A replay past that point would build a different show
            and report the difference as divergence, sending somebody hunting
            for non-determinism in an engine that has none. So the replay
            refuses, and says so in a sentence - up front for a `recovered`
            header line, and at the record for an applied `document.recover`,
            with everything before it still replayed. A REJECTED
            `document.recover` adopted nothing, and does not stop it.

            `replayable` is how many records, from the first, can be replayed;
            `refusal` is empty when that is all of them. */
        struct ReplayBoundary
        {
            std::size_t replayable = 0;
            std::string refusal;
        };

        ReplayBoundary replayBoundary (const LogFile& log);
    }

    //==============================================================================
    /*  THE COMMANDS THAT KNOW WHERE THE BUNDLE IS: `document.save` since Phase
        1, and since PR 5.5 `document.autosave`, `document.recover`,
        `document.discardRecovery`, `document.revert` and `document.saveAs`.

        SEPARATE FROM registerDocumentCommands because they are the only commands
        that need to know where the bundle IS. The document holds what someone
        decided; it does not hold which folder that came from, and giving it a
        path so one command could use it would put a filesystem inside the model.

        THE SPLIT IS ALSO WHAT LETS SAVING SURVIVE THE LOCK (§14.7). Everything
        registered here writes bytes rather than the document, so show mode
        refuses none of it - saving during a locked show is the point of locking
        it. The two exceptions are `document.revert` and `document.recover`,
        which replace the show through `adopt` and therefore ask `isLocked()` in
        their own handlers: `adopt` is a hatch and not a door, so the four
        predicates that refuse every ordinary edit never see them (§14.11).

        AND IT DECIDES WHAT `wfg replay` REGISTERS. This call sits behind
        `--out` as well as `--bundle` (Console.cpp), which is why §14.10 says a
        log carrying a `document.autosave` needs both flags: registered here,
        `document.autosave` replays as a rejection on a log opened without an
        output folder, which is loud, and better than a replay that wrote over
        the show it was checking.

        IT IS A COMMAND AND NOT A METHOD, because PRD 4.11 admits no exceptions:
        every gesture-reachable action exists as a named command. Saving is a
        gesture. It is also the one an OSCQuery client has no other way to ask
        for - there is no node whose value is "saved" - so without this a remote
        session could change a show and never commit it.

        APPLIED ON THE TICK THREAD LIKE EVERYTHING ELSE, which is what makes it
        safe to read the model at all: nothing else is touching it. What that
        thread does now is take the snapshot - one canonical serialisation of
        each file, 1.4 ms on the 500-cue show - and hand it on. THE BYTES ARE
        WRITTEN SOMEWHERE ELSE.

        *THIS PARAGRAPH IS A CORRECTION, AND THE SECOND (PR 5.5, 2026-09-11,
        at the author's direction).* Phase 1 wrote a save's bytes inside the
        tick and said crash-safe autosave would be "a background writer working
        from a snapshot, and a different piece of work". The first half of PR
        5.5 changed its mind and kept the autosave's bytes on the tick thread as
        well, pending M23. M23 answered (§14.14): 21 ms at the median against a
        5 ms threshold, 1.4 ms of it serialising and the rest FlushFileBuffers
        and ReplaceFile - the durability, which no faster Go.dot can move. So
        Phase 1's sentence was right, and it is now what happens: a
        `document.save`, a `document.autosave` and a `document.saveAs` take the
        snapshot here and hand the bytes to a `DocumentWriter`, a thread on
        MountProbe's shape, and the GO path stops sharing a thread with a disk
        at all. The log record is the same either way.

        WHAT THEIR `A` MEANS CHANGED WITH IT: "taken, and handed to the
        writer". A write that fails there cannot reject the command that asked
        for it - the record is already in the log - so the failure comes back
        through the writer's completions, leaves `savedRevision` unstamped so
        that the dirty dot stays lit, which is the truthful signal, and is
        published at `/godot/document/writeError` with the writer's own
        sentence (DocumentSession.h says why not at `lastError`). The checks
        that need no write stay here and still refuse at once: a
        `document.saveAs` with no path, and a `document.autosave` into a bundle
        that has gone - one `stat`, and the refusal an unattended writer most
        needs to leave in the log where somebody will read it.

        `document.revert` and `document.recover` read the disk, so they DRAIN
        THE WRITER before they look - the one place the tick thread waits for it,
        and Bundle.cpp says why that wait is acceptable there and would not be
        on the GO path. `document.discardRecovery` reads nothing and the lock
        does not refuse it, so it could be asked for during a performance; it
        waits for nothing, and is a job on the same writer instead - queued
        behind any rename that is moving the folder it deletes.

        IT TAKES THE SESSION, AND BY REFERENCE, since PR 5.2. It used to take
        the folder by value, which was all a save needed to know; a save that
        lands now also stamps `session.savedRevision`, which is what puts
        `/godot/document/dirty` out, and a stamp on a copy would be a stamp
        nobody reads. So the session must outlive the registry's use of this
        command - declared in the verb's own scope, beside the document, and
        never inside a block that closes before the engine stops applying. The
        autosave's three fields, the offer and the writer's failure ride on the
        same record for the same reason, and are read by the serve verb's two
        tick hooks.

        IT TAKES THE WRITER, BY REFERENCE AND FOR THE SAME REASON, and it tells
        the writer the one thing the two must start out agreeing about: where
        an earlier session's unanswered recovery lives, which the writer keeps
        its own copy of from then on (DocumentWriter.h). Registration happens
        once, before any command runs, so this is the moment they agree; after
        it, only the writer's own jobs and a recovery adopted on a drained
        writer change it. `wfg serve` hands in a writer it starts; `wfg replay`
        hands in a synchronous one, because a replay has no GO path to protect
        and a thread there would only add nondeterminism.

        `copiesFolder` IS WHERE A `document.saveAs` LANDS WHEN IT MUST NOT GO
        WHERE IT WAS TOLD, which is `wfg replay`'s case and nobody else's. Left
        empty - `serve`, every test - the path means what it says. Given - the
        replay's `--out` - every copy lands in `<copiesFolder>/saveAs/<the last
        component of the path it named>`, because a replay must never write
        outside the folder it was handed: the recorded path is somebody's
        archive on the machine that recorded it, overwriting it at three in the
        morning is the replay moving the world, and on another platform a
        Windows path is not even absolute and would land under the working
        directory with a drive letter in its name. The record carries the path
        as it was asked for either way, so a replay still compares line for line.
    */
    void registerBundleCommands (CommandRegistry& registry,
                                 ShowDocument& document,
                                 DocumentSession& session,
                                 DocumentWriter& writer,
                                 const juce::File& copiesFolder = juce::File());
}
