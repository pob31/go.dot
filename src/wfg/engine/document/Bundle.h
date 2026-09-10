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

#include <juce_core/juce_core.h>

namespace wfg::doc
{
    namespace Bundle
    {
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
            `writeBytesAtomically` in Bundle.cpp. */
        ReadResult save (const juce::File& folder, const ShowDocument& document);

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

        /** Whether there is anything for `document.recover` to adopt: a
            `recovery/show.xml`, which is the file the state beside it is
            meaningless without. */
        bool hasRecovery (const juce::File& folder);

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
            engine, holding files only this engine reads. */
        ReadResult saveRecovery (const juce::File& folder, const ShowDocument& document);

        /*  Reads `recovery/show.xml`, and the state beside it, into `document`.

            Same asymmetry as `open` and for the same reason: an unreadable
            show refuses and leaves the document exactly as it was - a torn
            autosave, the half-written file this whole design exists to make
            impossible and would still rather survive - while a state file that
            cannot be applied is a problem worth reporting and costs nobody
            their work. */
        ReadResult openRecovery (const juce::File& folder, ShowDocument& document);

        /** Deletes `recovery/` and everything in it. True when the folder is
            gone afterwards, which includes it never having been there. */
        bool discardRecovery (const juce::File& folder);

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

        Applied on the tick thread like everything else, which is what makes it
        safe to write the model out at all: nothing else is touching it. The cost
        is a file write inside a tick.

        *THIS PARAGRAPH IS A CORRECTION (PR 5.5).* It used to end "and that is
        why this is Phase 1's answer rather than Phase 5's - crash-safe autosave
        (PRD 4.3) is a background writer working from a snapshot, and it is a
        different piece of work". Phase 5 changed its mind, deliberately: the
        autosave writes from the tick thread too, from the document itself,
        because it is the same canonical serialisation and the same
        `replaceFileIn` that a save already is - on a document nothing else is
        touching, at most once every two seconds of quiet and once every thirty
        otherwise. What that can cost a GO is lateness and never a block (PRD
        §4.1): the queue is FIFO, so a GO already submitted is applied first,
        and a GO arriving during the write waits out the tick it landed in,
        exactly as it waits behind any other applied command. No lock is taken
        and nothing the tick thread does not already allocate is allocated.

        HOW LATE is M23's question (§14.14), and its instrument is in
        tests/BundleTests.cpp. At or under a quarter tick the bytes stay here;
        above it, the serialisation - the snapshot - stays on the tick thread,
        which it must in either shape, and the bytes go to a writer thread on
        `MountProbe`'s shape. The log record is `document.autosave` either way,
        which is why §14.10 could answer the constraint without waiting for
        the number.

        IT TAKES THE SESSION, AND BY REFERENCE, since PR 5.2. It used to take
        the folder by value, which was all a save needed to know; a save that
        lands now also stamps `session.savedRevision`, which is what puts
        `/godot/document/dirty` out, and a stamp on a copy would be a stamp
        nobody reads. So the session must outlive the registry's use of this
        command - declared in the verb's own scope, beside the document, and
        never inside a block that closes before the engine stops applying. The
        autosave's three fields and the recovery latch ride on the same record
        for the same reason, and are read by the serve verb's two tick hooks.

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
                                 const juce::File& copiesFolder = juce::File());
}
