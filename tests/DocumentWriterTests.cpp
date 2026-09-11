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

/*
    PR 5.5's SECOND HALF: the bytes leave the tick thread, an earlier session's
    unanswered recovery is moved aside rather than waited for, and a session
    that recovered does not replay (namespace draft §14.10, as the author
    decided it on 2026-09-11).

    WHAT A WRITER THREAD SILENTLY BREAKS is what these cases are about, one
    each: the order two writes land in; the revision a save is stamped with; a
    read that comes after a write still in the queue; what a failure looks like
    once it can no longer refuse the command that asked for it; and the clean
    exit that must not decide before the last save has landed. Most of them
    hold the writer STILL - a background writer that is never started keeps
    every job in its queue until something drains it - so that the disk can be
    looked at between a job being handed over and its landing, which a running
    thread would make a race rather than a test. Where the thread itself is the
    point, it is started.

    A serialisation surface like the bundle it writes, so every case runs under
    fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/DocumentWriter.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::doc;

namespace
{
    juce::File fixtureBundle()
    {
        const juce::File folder { juce::String (std::string (WFG_TEST_FIXTURES_DIR))
                                    + "/bundles/minimal" };

        REQUIRE_MESSAGE (folder.isDirectory(), "missing fixture bundle: "
                                                 << folder.getFullPathName());
        return folder;
    }

    /*  A scratch folder that cleans up after itself, as BundleTests.cpp's
        does: the bundle keeps its manifest's name, and the parent is what goes. */
    struct TempBundle
    {
        explicit TempBundle (const juce::String& bundleName)
        {
            parent = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("wfg-tests")
                       .getChildFile (juce::Uuid().toDashedString());
            parent.createDirectory();
            folder = parent.getChildFile (bundleName);
        }

        ~TempBundle() { parent.deleteRecursively(); }

        TempBundle (const TempBundle&) = delete;
        TempBundle& operator= (const TempBundle&) = delete;

        void copyFixture()
        {
            REQUIRE (fixtureBundle().copyDirectoryTo (folder));
        }

        juce::File parent, folder;
    };

    std::string readBytes (const juce::File& file)
    {
        juce::MemoryBlock block;
        REQUIRE_MESSAGE (file.loadFileAsData (block), "cannot read " << file.getFullPathName());
        return std::string (static_cast<const char*> (block.getData()), block.getSize());
    }

    void writeBytes (const juce::File& file, const std::string& text)
    {
        file.getParentDirectory().createDirectory();
        juce::FileOutputStream stream { file };
        REQUIRE (stream.openedOk());
        stream.setPosition (0);
        stream.truncate();
        REQUIRE (stream.write (text.data(), text.size()));
    }

    const std::string houseToHalfName = "/godot/cue/B3N8R5TW/name";

    /*  WHICH AFTERNOON A FOLDER HOLDS, read the way `document.recover` would
        read it - through the reader, into a scratch document - rather than by
        searching bytes, which would make the check depend on how the writer
        escapes an apostrophe. */
    std::string nameIn (const juce::File& recovery)
    {
        ShowDocument scratch;

        if (! Bundle::openRecoveryAt (recovery, scratch).ok)
            return "(nothing readable in " + recovery.getFileName().toStdString() + ")";

        return scratch.getAttribute (houseToHalfName).value_or ("(no such cue)");
    }

    /*  And which show the bundle's own show.xml holds. */
    std::string nameInBundle (const juce::File& folder)
    {
        ShowDocument scratch;

        if (! Bundle::open (folder, scratch).ok)
            return "(the bundle did not open)";

        return scratch.getAttribute (houseToHalfName).value_or ("(no such cue)");
    }

    /*  ONE SESSION'S WIRING, AS `wfg serve` MAKES IT: a registry, a writer and
        the session and document they share, for the life of the test - so
        that what a writer keeps between jobs, where the offer lives and which
        recovery was consumed, is kept here as it is there. The session's offer
        must be set before this is built, because registration is where the
        writer is told it. */
    struct Wiring
    {
        Wiring (ShowDocument& documentToWire, DocumentSession& sessionToWire,
                DocumentWriter::Mode writerMode)
            : document (documentToWire), session (sessionToWire), writer (writerMode)
        {
            registerBundleCommands (registry, document, session, writer);
        }

        /*  The handler alone, at the tick a test names, exactly as the engine
            would call it. Nothing is settled: a test that wants the after-tick
            asks for it, because when that happens is often the question. */
        Outcome invoke (const std::string& name,
                        const std::vector<osc::Value>& args = {},
                        std::int64_t tick = 0)
        {
            const auto* command = registry.find (name);
            REQUIRE_MESSAGE (command != nullptr, "no command " << name);

            CommandContext context;
            context.tick = tick;
            return command->handler (context, args);
        }

        /*  What the serve verb's after-tick does first. */
        std::vector<std::string> settleNow() { return settle (session, writer); }

        ShowDocument& document;
        DocumentSession& session;
        DocumentWriter writer;
        CommandRegistry registry;
    };

    /*  An earlier session's afternoon, left in `recovery/` the way a crash
        leaves it: opened, edited, autosaved, and never saved or closed. */
    void abandonAnAfternoon (const juce::File& folder, const std::string& name)
    {
        ShowDocument earlier;
        REQUIRE (Bundle::open (folder, earlier).ok);

        DocumentSession earlierSession { folder, earlier.showRevision() };
        Wiring wiring { earlier, earlierSession, DocumentWriter::Mode::synchronous };

        REQUIRE (earlier.setAttribute (houseToHalfName, name).ok);
        REQUIRE (wiring.invoke ("document.autosave", {}, 100).applied);
        wiring.settleNow();

        REQUIRE (Bundle::hasRecovery (folder));
    }
}

//==============================================================================
TEST_CASE ("writer: a save and an autosave queued in that order land in that order, the save's tidy-up between them")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  ORDERING, the first thing a writer thread can break. The save deletes
        this session's `recovery/` once its bytes have landed, because the work
        has become the show; an autosave queued BEHIND it writes a newer
        `recovery/`. In queue order the folder survives, holding the autosave.
        Out of order - the autosave first, then the save and its tidy-up - the
        folder would be gone, and a crash a moment later would lose every edit
        made after the save. So the folder being there, holding the second
        snapshot, is the order, read off the disk.

        Twice: once with the writer held still, so the disk can be seen
        untouched while both jobs wait, and once with its thread running. */
    for (const auto started : { false, true })
    {
        INFO ("the writer's thread was started: " << (started ? "yes" : "no"));

        TempBundle temp { "minimal" };
        temp.copyFixture();

        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        Wiring wiring { document, session, DocumentWriter::Mode::background };

        if (started)
            REQUIRE (wiring.writer.start());

        REQUIRE (document.setAttribute (houseToHalfName, "Saved first").ok);
        const auto savedAt = document.showRevision();
        REQUIRE (wiring.invoke ("document.save", {}, 10).applied);

        REQUIRE (document.setAttribute (houseToHalfName, "Autosaved second").ok);
        const auto autosavedAt = document.showRevision();
        REQUIRE (wiring.invoke ("document.autosave", {}, 20).applied);

        if (! started)
        {
            /*  HANDED OVER AND NOT YET WRITTEN: both records would be in the
                log by now, and the disk is exactly as it was. */
            CHECK (wiring.writer.pending() == 2u);
            CHECK (nameInBundle (temp.folder) == "House to half");
            CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
        }

        wiring.writer.drain();

        const auto completions = wiring.writer.takeCompletions();
        REQUIRE (completions.size() == 2u);

        CHECK (completions[0].kind == WriteJob::Kind::save);
        CHECK (completions[0].landed);
        CHECK (completions[0].revision == savedAt);
        CHECK (completions[0].recoveryCleared);

        CHECK (completions[1].kind == WriteJob::Kind::autosave);
        CHECK (completions[1].landed);
        CHECK (completions[1].revision == autosavedAt);

        CHECK (nameInBundle (temp.folder) == "Saved first");
        REQUIRE (Bundle::hasRecovery (temp.folder));
        CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "Autosaved second");

        wiring.writer.stop();
    }
}

TEST_CASE ("writer: a write is stamped with the revision its snapshot was taken at, never the one the document has when the disk says yes")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    Wiring wiring { document, session, DocumentWriter::Mode::background };

    REQUIRE (document.setAttribute (houseToHalfName, "In the snapshot").ok);
    const auto snapshotAt = document.showRevision();
    REQUIRE (wiring.invoke ("document.save", {}, 10).applied);

    /*  AN EDIT WHILE THE BYTES ARE ON THEIR WAY - the case that only exists
        because they now travel. It is not in the file the writer is writing. */
    REQUIRE (document.setAttribute (houseToHalfName, "Not in the snapshot").ok);
    REQUIRE (document.showRevision() != snapshotAt);

    wiring.writer.drain();
    wiring.settleNow();

    /*  STAMPED WITH WHAT WAS WRITTEN. A stamp read off the document now would
        equal its current revision and put the dot out over an edit the disk
        has never seen - a false "saved", which is the one way round this light
        must never be wrong. */
    CHECK (session.savedRevision == snapshotAt);
    CHECK (isDirty (document, session));
    CHECK (nameInBundle (temp.folder) == "In the snapshot");

    //--------------------------------------------------------------------------
    // The autosave's stamp, the same way.
    const auto autosnapshotAt = document.showRevision();
    REQUIRE (wiring.invoke ("document.autosave", {}, 20).applied);
    REQUIRE (document.setAttribute (houseToHalfName, "After the autosave was handed over").ok);

    wiring.writer.drain();
    wiring.settleNow();

    CHECK (session.autosavedRevision == autosnapshotAt);
    CHECK (session.autosavedRevision != document.showRevision());
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "Not in the snapshot");

    //--------------------------------------------------------------------------
    // And a save of the show as it stands, landed, puts the dot out.
    REQUIRE (wiring.invoke ("document.save", {}, 30).applied);
    wiring.writer.drain();
    wiring.settleNow();

    CHECK_FALSE (isDirty (document, session));
}

TEST_CASE ("document.revert after a save still in the queue reads the new show.xml, because it drains the writer first")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  A READ AFTER A QUEUED WRITE. The writer is held still, so the save below
        is certainly still in its queue when the revert arrives - on a running
        thread that would be a race this case could lose by luck. A revert that
        read show.xml now would bring back the show from BEFORE the save: the
        one wrong answer a revert must never give, and a quiet one. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    Wiring wiring { document, session, DocumentWriter::Mode::background };

    REQUIRE (document.setAttribute (houseToHalfName, "Saved, and still in the queue").ok);
    REQUIRE (wiring.invoke ("document.save", {}, 10).applied);

    REQUIRE (wiring.writer.pending() == 1u);
    REQUIRE (nameInBundle (temp.folder) == "House to half");

    REQUIRE (document.setAttribute (houseToHalfName, "Thrown away by the revert").ok);

    const auto outcome = wiring.invoke ("document.revert", {}, 20);

    CHECK (outcome.applied);
    CHECK (wiring.writer.pending() == 0u);

    /*  THE NEW BYTES, READ BACK, and the dot out over them: the drain made the
        save land, the settle stamped it, and the revert's own re-stamp says the
        document is now the file. */
    CHECK (document.getAttribute (houseToHalfName) == std::string ("Saved, and still in the queue"));
    CHECK (nameInBundle (temp.folder) == "Saved, and still in the queue");
    CHECK_FALSE (isDirty (document, session));
    CHECK (session.savedRevision == document.showRevision());
}

TEST_CASE ("a write that fails on the writer is applied, keeps the dot lit and says why at writeError, until a write of its kind lands")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  WHAT A FAILURE LOOKS LIKE NOW. The command is applied - its `A` means
        "taken, and handed to the writer" - and the failure comes back through
        the completion: no stamp, so the dot stays lit, which is the truthful
        signal; and the writer's own sentence at `/godot/document/writeError`.

        The failures are made the way BundleTests.cpp makes them, portably: a
        DIRECTORY standing where the writer's temp file has to go, which every
        platform refuses to open for writing whatever the privilege. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    Wiring wiring { document, session, DocumentWriter::Mode::synchronous };

    const auto showXml = Bundle::showFile (temp.folder);
    const auto before = readBytes (showXml);

    REQUIRE (document.setAttribute (houseToHalfName, "Never reached the disk").ok);

    const auto obstacle = Bundle::temporaryFor (showXml);
    REQUIRE (obstacle.createDirectory().wasOk());

    const auto stampBefore = session.savedRevision;

    CHECK (wiring.invoke ("document.save", {}, 7).applied);
    wiring.settleNow();

    CHECK (readBytes (showXml) == before);
    CHECK (session.savedRevision == stampBefore);
    CHECK (isDirty (document, session));

    INFO ("writeError: " << session.writeError);
    CHECK (session.writeError.rfind ("document.save at tick 7: ", 0) == 0);
    CHECK (session.writeError.find (obstacle.getFileName().toStdString()) != std::string::npos);

    //--------------------------------------------------------------------------
    /*  AN AUTOSAVE THAT LANDS DOES NOT PUT IT OUT: `recovery/` being current
        says nothing about a show.xml that is not. */
    CHECK (wiring.invoke ("document.autosave", {}, 8).applied);
    wiring.settleNow();

    CHECK (Bundle::hasRecovery (temp.folder));
    CHECK (session.writeError.rfind ("document.save at tick 7: ", 0) == 0);

    //--------------------------------------------------------------------------
    // The next save that lands does, and the dot goes out with it.
    REQUIRE (obstacle.deleteRecursively());

    CHECK (wiring.invoke ("document.save", {}, 9).applied);
    wiring.settleNow();

    CHECK (session.writeError.empty());
    CHECK_FALSE (isDirty (document, session));
    CHECK (nameInBundle (temp.folder) == "Never reached the disk");

    //--------------------------------------------------------------------------
    /*  AN AUTOSAVE THAT FAILS: nothing stamped, so it is still owed; and a SAVE
        that lands over it puts it out, because a landed save takes this
        session's `recovery/` away - the work is the show. */
    REQUIRE (document.setAttribute (houseToHalfName, "Autosave blocked").ok);

    const auto recoveryObstacle = Bundle::temporaryFor (Bundle::recoveryShowFile (temp.folder));
    REQUIRE (recoveryObstacle.createDirectory().wasOk());

    const auto autosavedBefore = session.autosavedRevision;

    CHECK (wiring.invoke ("document.autosave", {}, 11).applied);
    wiring.settleNow();

    CHECK (session.autosavedRevision == autosavedBefore);
    CHECK (session.writeError.rfind ("document.autosave at tick 11: ", 0) == 0);

    CHECK (wiring.invoke ("document.save", {}, 12).applied);
    wiring.settleNow();

    CHECK (session.writeError.empty());
    CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());

    //--------------------------------------------------------------------------
    /*  A COPY THAT FAILS IS NOT PUT OUT BY A SAVE: the archive somebody asked
        for does not exist, whatever the session's own folder now holds. */
    const auto blocked = temp.parent.getChildFile ("blocked-archive");
    REQUIRE (Bundle::temporaryFor (Bundle::showFile (blocked)).createDirectory().wasOk());

    CHECK (wiring.invoke ("document.saveAs",
                          { osc::Value::string (blocked.getFullPathName().toStdString()) }, 13)
             .applied);
    wiring.settleNow();

    CHECK (session.writeError.rfind ("document.saveAs at tick 13: ", 0) == 0);

    REQUIRE (document.setAttribute (houseToHalfName, "Saved after a copy that failed").ok);
    CHECK (wiring.invoke ("document.save", {}, 14).applied);
    wiring.settleNow();

    CHECK (session.writeError.rfind ("document.saveAs at tick 13: ", 0) == 0);
    CHECK_FALSE (isDirty (document, session));
}

//==============================================================================
TEST_CASE ("an earlier session's recovery is moved aside by this session's first autosave, and recover adopts that afternoon, not this session's")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  THE AUTHOR'S RULE, END TO END (§14.10, 2026-09-11): the offer found at
        `recovery/` is renamed to the first free `recovery.previous.N/` the first
        time this session needs to autosave, and the autosave then runs as
        usual - so the operator is protected from the first quiet on and the
        afternoon waits where nothing this session does can reach it.

        With the writer held still, so that the autosave carrying the rename is
        STILL QUEUED when the operator answers: `document.recover` has to drain
        it before it reads. Without the drain it would adopt the offer from
        `recovery/`, and the queued rename would then find no offer and write
        this session's work straight over the only copy of that afternoon. */
    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "The afternoon nobody saved");

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);
    REQUIRE (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));

    Wiring wiring { document, session, DocumentWriter::Mode::background };
    CHECK (wiring.writer.offer() == Bundle::recoveryFolder (temp.folder));

    const auto aside = temp.folder.getChildFile ("recovery.previous.1");

    REQUIRE (document.setAttribute (houseToHalfName, "This session's work").ok);
    REQUIRE (wiring.invoke ("document.autosave", {}, 150).applied);

    CHECK (wiring.writer.pending() == 1u);
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "The afternoon nobody saved");
    CHECK_FALSE (aside.exists());

    //--------------------------------------------------------------------------
    const auto outcome = wiring.invoke ("document.recover", {}, 151);

    CHECK (outcome.applied);
    CHECK (wiring.writer.pending() == 0u);

    /*  THE EARLIER AFTERNOON, NOT THIS SESSION'S: moved aside first, this
        session's autosave written where it used to be, and the recovery read
        from where the move put it. */
    CHECK (document.getAttribute (houseToHalfName) == std::string ("The afternoon nobody saved"));
    CHECK (nameIn (aside) == "The afternoon nobody saved");
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "This session's work");

    // Answered, and dirty: the recovered work is not on disk as the show.
    CHECK_FALSE (hasRecoveryOffer (session));
    CHECK (wiring.writer.offer() == juce::File());
    CHECK (isDirty (document, session));

    /*  CONSUMED AND NOT YET DELETED. At this instant the recovered show is in
        memory and in that folder and nowhere else, so a crash now must still
        find it; `recovery/` holds this session's own older work, so the catch-up
        autosave is owed. */
    CHECK (aside.isDirectory());
    CHECK (wiring.writer.consumed() == aside);
    CHECK (session.autosavedRevision == 0u);

    //--------------------------------------------------------------------------
    /*  THE CATCH-UP. The after-tick would have stamped the change at 151; two
        seconds of quiet later the autosave is due, and once its bytes land the
        consumed folder goes - in queue order, on the writer, and not a moment
        before. */
    session.lastChangeTick = 151;
    REQUIRE (autosaveDue (document, session, 151 + autosaveQuietTicks));
    REQUIRE (wiring.invoke ("document.autosave", {}, 151 + autosaveQuietTicks).applied);

    CHECK (aside.isDirectory());

    wiring.writer.drain();
    wiring.settleNow();

    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "The afternoon nobody saved");
    CHECK_FALSE (aside.exists());
    CHECK (wiring.writer.consumed() == juce::File());
    CHECK (session.autosavedRevision == document.showRevision());
    CHECK (session.writeError.empty());

    //--------------------------------------------------------------------------
    // And a later autosave moves nothing: there is no offer left to move.
    REQUIRE (document.setAttribute (houseToHalfName, "After the recovery").ok);
    REQUIRE (wiring.invoke ("document.autosave", {}, 600).applied);
    wiring.writer.drain();
    wiring.settleNow();

    CHECK_FALSE (aside.exists());
    CHECK_FALSE (temp.folder.getChildFile ("recovery.previous.2").exists());
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "After the recovery");
}

TEST_CASE ("nothing deletes an unanswered recovery.previous.N but its own discard - not a save, not a revert, not a clean exit")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "The afternoon nobody saved");

    const auto aside = temp.folder.getChildFile ("recovery.previous.1");

    {
        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        session.offeredRecovery = Bundle::offeredRecovery (temp.folder);

        Wiring wiring { document, session, DocumentWriter::Mode::synchronous };

        REQUIRE (document.setAttribute (houseToHalfName, "First edit").ok);
        REQUIRE (wiring.invoke ("document.autosave", {}, 150).applied);
        wiring.settleNow();

        REQUIRE (session.offeredRecovery == aside);

        //----------------------------------------------------------------------
        // A save takes this session's recovery/, and not that.
        CHECK (wiring.invoke ("document.save", {}, 200).applied);
        wiring.settleNow();

        CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
        CHECK (nameIn (aside) == "The afternoon nobody saved");
        CHECK (session.offeredRecovery == aside);

        //----------------------------------------------------------------------
        // Nor does a revert.
        REQUIRE (document.setAttribute (houseToHalfName, "Second edit").ok);
        REQUIRE (wiring.invoke ("document.autosave", {}, 300).applied);
        wiring.settleNow();
        REQUIRE (Bundle::hasRecovery (temp.folder));

        CHECK (wiring.invoke ("document.revert", {}, 400).applied);

        CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
        CHECK (nameIn (aside) == "The afternoon nobody saved");

        //----------------------------------------------------------------------
        /*  Nor a clean exit. A save and an autosave of the same show, in that
            order, leave the document clean with this session's `recovery/` on
            the disk - which the exit tidies away, and the afternoon beside it
            it does not. */
        REQUIRE (document.setAttribute (houseToHalfName, "Third edit").ok);
        REQUIRE (wiring.invoke ("document.save", {}, 500).applied);
        REQUIRE (wiring.invoke ("document.autosave", {}, 501).applied);
        wiring.settleNow();

        REQUIRE_FALSE (isDirty (document, session));
        REQUIRE (Bundle::hasRecovery (temp.folder));

        finishSession (document, session, wiring.writer);

        CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
        CHECK (nameIn (aside) == "The afternoon nobody saved");
    }

    //--------------------------------------------------------------------------
    /*  SO THE NEXT OPEN OFFERS IT AGAIN - there is no `recovery/`, and it is the
        highest `recovery.previous.N/` there is - until somebody answers it. */
    CHECK (Bundle::offeredRecovery (temp.folder) == aside);

    ShowDocument next;
    REQUIRE (Bundle::open (temp.folder, next).ok);

    DocumentSession nextSession { temp.folder, next.showRevision() };
    nextSession.offeredRecovery = Bundle::offeredRecovery (temp.folder);

    Wiring nextWiring { next, nextSession, DocumentWriter::Mode::synchronous };

    CHECK (nextWiring.invoke ("document.discardRecovery", {}, 1).applied);
    nextWiring.settleNow();

    CHECK_FALSE (aside.exists());
    CHECK_FALSE (hasRecoveryOffer (nextSession));
    CHECK (Bundle::offeredRecovery (temp.folder) == juce::File());
}

TEST_CASE ("document.discardRecovery is a job: queued behind a rename, it deletes the offer where the rename put it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  THE LOCK DOES NOT REFUSE A DISCARD, so it can be asked for during a
        performance, and so it waits for nothing: it is queued. Behind an
        autosave that is about to move the offer, which is the case that makes
        the queue matter - deleted where the offer WAS, the folder would be gone
        from under the rename and the afternoon it was moving would live on,
        unanswered, under a new name. */
    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "The afternoon nobody saved");

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);

    Wiring wiring { document, session, DocumentWriter::Mode::background };

    REQUIRE (document.setAttribute (houseToHalfName, "This session's work").ok);
    REQUIRE (wiring.invoke ("document.autosave", {}, 150).applied);

    CHECK (wiring.invoke ("document.discardRecovery", {}, 151).applied);

    /*  Both waiting, the disk untouched, and the offer still published: it is
        withdrawn by the completion that says the folder has gone, never before. */
    CHECK (wiring.writer.pending() == 2u);
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "The afternoon nobody saved");
    CHECK (hasRecoveryOffer (session));

    wiring.writer.drain();
    wiring.settleNow();

    CHECK_FALSE (temp.folder.getChildFile ("recovery.previous.1").exists());
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "This session's work");
    CHECK_FALSE (hasRecoveryOffer (session));
    CHECK (wiring.writer.offer() == juce::File());
    CHECK (session.writeError.empty());

    // And an empty gesture is refused, here, before anything is queued.
    const auto again = wiring.invoke ("document.discardRecovery", {}, 152);

    CHECK_FALSE (again.applied);
    CHECK (again.reason == reason::noRecovery);
    CHECK (wiring.writer.pending() == 0u);
}

TEST_CASE ("recovering is one function for serve --recover and document.recover: in place it consumes nothing, from aside it consumes the folder")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "The afternoon nobody saved");

    //--------------------------------------------------------------------------
    /*  IN PLACE: `recovery/` becomes this session's own, holding exactly the
        show now on screen, so nothing is owed and nothing is consumed. */
    {
        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        session.offeredRecovery = Bundle::offeredRecovery (temp.folder);

        DocumentWriter writer;
        writer.setOffer (session.offeredRecovery);

        const auto adopted = Bundle::adoptOfferedRecovery (document, session, writer);

        CHECK (adopted.ok);
        CHECK (document.getAttribute (houseToHalfName) == std::string ("The afternoon nobody saved"));
        CHECK (isDirty (document, session));
        CHECK_FALSE (hasRecoveryOffer (session));
        CHECK (writer.offer() == juce::File());
        CHECK (writer.consumed() == juce::File());
        CHECK (session.autosavedRevision == document.showRevision());
    }

    //--------------------------------------------------------------------------
    /*  FROM ASIDE: the afternoon moved to a `recovery.previous.N/` by a session
        that then ended, and `recovery/` gone - the next open offers the
        previous one, and adopting it consumes that folder: still there, and
        deleted by the first write that lands. */
    const auto aside = temp.folder.getChildFile ("recovery.previous.1");
    REQUIRE (Bundle::recoveryFolder (temp.folder).moveFileTo (aside));

    {
        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        session.offeredRecovery = Bundle::offeredRecovery (temp.folder);
        REQUIRE (session.offeredRecovery == aside);

        Wiring wiring { document, session, DocumentWriter::Mode::synchronous };

        const auto adopted = Bundle::adoptOfferedRecovery (document, session, wiring.writer);

        CHECK (adopted.ok);
        CHECK (document.getAttribute (houseToHalfName) == std::string ("The afternoon nobody saved"));
        CHECK_FALSE (hasRecoveryOffer (session));
        CHECK (session.autosavedRevision == 0u);
        CHECK (wiring.writer.consumed() == aside);
        CHECK (aside.isDirectory());

        /*  A SAVE is the other write that puts the recovered work somewhere
            safe, and it consumes the folder just as the catch-up autosave does. */
        CHECK (wiring.invoke ("document.save", {}, 5).applied);
        wiring.settleNow();

        CHECK_FALSE (aside.exists());
        CHECK (wiring.writer.consumed() == juce::File());
        CHECK_FALSE (isDirty (document, session));
        CHECK (nameInBundle (temp.folder) == "The afternoon nobody saved");
    }

    //--------------------------------------------------------------------------
    /*  AN OFFER WHOSE FOLDER HAS GONE is withdrawn and refused: nothing to
        adopt, and nothing left to answer. */
    {
        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        session.offeredRecovery = temp.folder.getChildFile ("recovery.previous.7");

        DocumentWriter writer;
        writer.setOffer (session.offeredRecovery);

        const auto adopted = Bundle::adoptOfferedRecovery (document, session, writer);

        CHECK_FALSE (adopted.ok);
        CHECK_FALSE (hasRecoveryOffer (session));
        CHECK (writer.offer() == juce::File());
        CHECK (document.getAttribute (houseToHalfName) == std::string ("The afternoon nobody saved"));
    }
}

TEST_CASE ("an offer whose recovery/ somebody deleted by hand is withdrawn by the next autosave, which then writes as usual")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Without this, every autosave for the rest of the show would try to
        rename a folder that is not there, fail, and leave this session's work
        unprotected - one failure every thirty seconds, for nothing. */
    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "The afternoon nobody saved");

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);

    Wiring wiring { document, session, DocumentWriter::Mode::synchronous };

    REQUIRE (Bundle::recoveryFolder (temp.folder).deleteRecursively());

    REQUIRE (document.setAttribute (houseToHalfName, "This session's work").ok);
    CHECK (wiring.invoke ("document.autosave", {}, 150).applied);
    wiring.settleNow();

    CHECK_FALSE (hasRecoveryOffer (session));
    CHECK (wiring.writer.offer() == juce::File());
    CHECK (session.writeError.empty());
    CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "This session's work");
    CHECK (Bundle::previousRecoveries (temp.folder).empty());
}

TEST_CASE ("the clean exit drains the writer before it decides, so a save queued at the end lands and the tidy-up sees it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    //--------------------------------------------------------------------------
    /*  A SAVE QUEUED AT CTRL-C LANDS. The writer's thread is running, as in
        `wfg serve`; the save is the last thing handed to it, and the exit is
        asked for straight after. Asked before the drain, "is it dirty" would
        answer yes about a show on its way to the disk, and the tidy-up would
        leave a `recovery/` behind to be offered back at the next start. */
    {
        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        Wiring wiring { document, session, DocumentWriter::Mode::background };
        REQUIRE (wiring.writer.start());

        REQUIRE (document.setAttribute (houseToHalfName, "Autosaved").ok);
        REQUIRE (wiring.invoke ("document.autosave", {}, 10).applied);

        REQUIRE (document.setAttribute (houseToHalfName, "Saved at Ctrl-C").ok);
        REQUIRE (wiring.invoke ("document.save", {}, 20).applied);

        finishSession (document, session, wiring.writer);

        CHECK_FALSE (wiring.writer.isRunning());
        CHECK (nameInBundle (temp.folder) == "Saved at Ctrl-C");
        CHECK_FALSE (isDirty (document, session));
        CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
    }

    //--------------------------------------------------------------------------
    /*  AND A DIRTY EXIT KEEPS THE FOLDER: a tidy shutdown with unsaved work is
        precisely what the folder is for. */
    {
        ShowDocument document;
        REQUIRE (Bundle::open (temp.folder, document).ok);

        DocumentSession session { temp.folder, document.showRevision() };
        Wiring wiring { document, session, DocumentWriter::Mode::background };
        REQUIRE (wiring.writer.start());

        REQUIRE (document.setAttribute (houseToHalfName, "Never saved").ok);
        REQUIRE (wiring.invoke ("document.autosave", {}, 10).applied);

        finishSession (document, session, wiring.writer);

        CHECK (isDirty (document, session));
        CHECK (nameIn (Bundle::recoveryFolder (temp.folder)) == "Never saved");
    }
}

//==============================================================================
TEST_CASE ("the offer at open prefers recovery/, then the highest recovery.previous.N, counted as numbers")
{
    TempBundle temp { "minimal" };
    temp.copyFixture();

    const auto folder = temp.folder;

    /*  What is inside does not matter to the offer, which is about folders; a
        show.xml is written so each one looks the way a real one would. */
    const auto aRecoveryAt = [] (const juce::File& at)
    {
        writeBytes (at.getChildFile ("show.xml"), "<Show/>\n");
    };

    CHECK (Bundle::offeredRecovery (folder) == juce::File());
    CHECK (Bundle::nextPreviousRecovery (folder) == folder.getChildFile ("recovery.previous.1"));

    aRecoveryAt (folder.getChildFile ("recovery.previous.2"));
    aRecoveryAt (folder.getChildFile ("recovery.previous.10"));
    aRecoveryAt (folder.getChildFile ("recovery.previous.9"));
    REQUIRE (folder.getChildFile ("recovery.previous.old").createDirectory().wasOk());

    /*  THE HIGHEST BY NUMBER, which is the newest: sorted as names, `.10` would
        come before `.9` and the oldest of the three would be offered first. A
        folder whose suffix is not a number is somebody else's. */
    CHECK (Bundle::offeredRecovery (folder) == folder.getChildFile ("recovery.previous.10"));

    const auto previous = Bundle::previousRecoveries (folder);
    REQUIRE (previous.size() == 3u);
    CHECK (previous[0].getFileName() == juce::String ("recovery.previous.2"));
    CHECK (previous[1].getFileName() == juce::String ("recovery.previous.9"));
    CHECK (previous[2].getFileName() == juce::String ("recovery.previous.10"));

    /*  ONE PAST THE HIGHEST, not the lowest gap - `.1` and `.3` to `.8` are
        free, and filing the next afternoon there would make it look oldest. */
    CHECK (Bundle::nextPreviousRecovery (folder) == folder.getChildFile ("recovery.previous.11"));

    /*  A FILE under the name counts for the next number, so the rename can
        never land on it, and is not offered, because it is not a recovery. */
    writeBytes (folder.getChildFile ("recovery.previous.12"), "not a folder\n");
    CHECK (Bundle::nextPreviousRecovery (folder) == folder.getChildFile ("recovery.previous.13"));
    CHECK (Bundle::offeredRecovery (folder) == folder.getChildFile ("recovery.previous.10"));

    /*  A `recovery/` holding only an interrupted write's temp has no show to
        offer, so the moved-aside ones still come first. */
    writeBytes (Bundle::temporaryFor (Bundle::recoveryShowFile (folder)), "half a show");
    CHECK (Bundle::offeredRecovery (folder) == folder.getChildFile ("recovery.previous.10"));

    /*  And one with a show.xml beats them all: the last session died, and its
        afternoon is the newest there is. */
    writeBytes (Bundle::recoveryShowFile (folder), "<Show/>\n");
    CHECK (Bundle::offeredRecovery (folder) == Bundle::recoveryFolder (folder));
}

//==============================================================================
TEST_CASE ("wfg replay refuses a session that recovered: up front for a --recover header, at the record for document.recover")
{
    /*  The recovered bytes are in neither the log nor the bundle its header
        hashes, so a replay past the adoption would build a different show and
        call the difference divergence. `Bundle::replayBoundary` is what `wfg
        replay` asks, and it exits 2 - "could not run" - when the answer is a
        refusal; the verb's exit code is its business, and this is the rule. */
    const auto bundle = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("MyShow");

    //--------------------------------------------------------------------------
    // The mark a `--recover` session writes, through the parser and back.
    const auto mark = Bundle::recoveredLogHeaderLine (bundle, bundle.getChildFile ("recovery.previous.2"));
    CHECK (mark == "recovered recovery.previous.2/");
    CHECK (Bundle::recoveredLogHeaderLine (bundle, Bundle::recoveryFolder (bundle))
             == "recovered recovery/");

    {
        const auto log = LogFile::parse ("# wfg-log 1\n"
                                         "# bundle MyShow sha256:00\n"
                                         "# " + mark + "\n"
                                         "A 0 0 cli noop\n"
                                         "A 1 1 cli noop\n");
        REQUIRE (log.errors.empty());

        const auto boundary = Bundle::replayBoundary (log);

        /*  UP FRONT: its very first record ran against the recovered show. */
        CHECK (boundary.replayable == 0u);
        CHECK (boundary.refusal.find ("--recover") != std::string::npos);
        CHECK (boundary.refusal.find ("recovery.previous.2/") != std::string::npos);
    }

    //--------------------------------------------------------------------------
    {
        const auto log = LogFile::parse ("# wfg-log 1\n"
                                         "A 0 0 cli noop\n"
                                         "A 3 1 udp:127.0.0.1:50000 document.recover\n"
                                         "A 3 2 cli noop\n");
        REQUIRE (log.errors.empty());

        const auto boundary = Bundle::replayBoundary (log);

        /*  AT THE RECORD: what came before it ran against the bundle, and is
            replayed; nothing from it on did. */
        CHECK (boundary.replayable == 1u);
        CHECK (boundary.refusal.find ("tick 3") != std::string::npos);
        CHECK (boundary.refusal.find ("document.recover") != std::string::npos);
    }

    //--------------------------------------------------------------------------
    {
        /*  A REFUSED RECOVER ADOPTED NOTHING - locked, or nothing to adopt - and
            replays as the refusal it was. */
        const auto log = LogFile::parse ("# wfg-log 1\n"
                                         "R 3 0 udp:127.0.0.1:50000 no-recovery document.recover\n"
                                         "R 4 1 udp:127.0.0.1:50000 locked document.recover\n"
                                         "A 5 2 cli noop\n");
        REQUIRE (log.errors.empty());

        const auto boundary = Bundle::replayBoundary (log);

        CHECK (boundary.refusal.empty());
        CHECK (boundary.replayable == 3u);
    }

    //--------------------------------------------------------------------------
    {
        // And an ordinary session - autosaves, a save, a discard - is not refused.
        const auto log = LogFile::parse ("# wfg-log 1\n"
                                         "# bundle MyShow sha256:00\n"
                                         "A 100 0 engine document.autosave\n"
                                         "A 150 1 cli document.save\n"
                                         "A 160 2 cli document.discardRecovery\n");
        REQUIRE (log.errors.empty());

        const auto boundary = Bundle::replayBoundary (log);

        CHECK (boundary.refusal.empty());
        CHECK (boundary.replayable == 3u);
    }
}
