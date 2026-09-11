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
    The bundle: the folder a show lives in, the line between what someone
    decided and what the engine was doing, and the grammar that lets somebody
    else's validator have an opinion about both.

    A serialisation surface, so every case here runs under fr_FR as well as C.

    The fixture at fixtures/bundles/minimal is hand-authored. Nothing here
    regenerates it: a golden written by the code under test proves only that the
    code agrees with itself, and "save produces exactly this" is the single most
    load-bearing claim in the file.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentSession.h>
#include <wfg/engine/document/DocumentWriter.h>
#include <wfg/engine/document/EphemeralState.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/RelaxNg.h>
#include <wfg/engine/document/Schema.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <regex>
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

    /*  A scratch folder that cleans up after itself, named so that a bundle
        copied into it keeps its manifest's name. The parent is what gets
        deleted, so the bundle folder inside it can be renamed by a test without
        stranding anything. */
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

        /** The fixture, copied in, so a test can break one file and leave the
            committed copy alone. */
        void copyFixture()
        {
            REQUIRE (fixtureBundle().copyDirectoryTo (folder));
        }

        juce::File parent, folder;
    };

    /*  Raw bytes. Every claim in this file is about bytes - the line endings,
        the sparse attributes, the identical round trip - so nothing here goes
        near a reader that might normalise one of them out of existence. */
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

    bool mentions (const std::vector<std::string>& problems, const std::string& fragment)
    {
        for (const auto& problem : problems)
            if (problem.find (fragment) != std::string::npos)
                return true;

        return false;
    }

    const std::string standbyAddress = "/godot/list/7K2QM9X4/standby";
    const std::string houseToHalfName = "/godot/cue/B3N8R5TW/name";

    /*  Every temp a save left in `folder` - `<name>.tmp-<pid>` - files and
        directories alike, because a directory is what the failing-write case
        stands in a temp's way. */
    std::vector<std::string> tempsIn (const juce::File& folder)
    {
        std::vector<std::string> names;

        for (const auto& found : folder.findChildFiles (juce::File::findFilesAndDirectories,
                                                        false, "*.tmp-*"))
            names.push_back (found.getFileName().toStdString());

        return names;
    }

    /*  Any of the commands that know where the bundle is, invoked as the engine
        would invoke it - the handler alone, at the tick a test names, with a
        copies folder only when the test is standing in for `wfg replay` - and
        then SETTLED, as the serve verb's after-tick settles at the end of the
        tick the command ran in.

        A SYNCHRONOUS WRITER PER CALL, which is `wfg replay`'s writer: every
        job is performed inside the handler, so by the time this returns the
        bytes are on the disk, and `settle` has turned the completion into the
        session's stamps - exactly what a question about what one command did
        needs. A writer per call is enough because what a writer keeps between
        jobs is where the offer lives, and that goes back to the session through
        `settle` and on to the next writer through registration. The one thing
        a fresh writer would forget is a consumed recovery, which only
        recovering from a `recovery.previous.N/` makes; the cases that do that
        live in DocumentWriterTests.cpp, on one writer each. What a writer
        THREAD changes - the order, the stamp's revision, the drain - is asked
        there too. */
    Outcome invokeBundleCommand (ShowDocument& document, DocumentSession& session,
                                 const std::string& name,
                                 const std::vector<osc::Value>& args = {},
                                 std::int64_t tick = 0,
                                 const juce::File& copies = juce::File())
    {
        CommandRegistry registry;
        DocumentWriter writer;
        registerBundleCommands (registry, document, session, writer, copies);

        const auto* command = registry.find (name);
        REQUIRE_MESSAGE (command != nullptr, "no command " << name);

        CommandContext context;
        context.tick = tick;
        const auto outcome = command->handler (context, args);

        settle (session, writer);
        return outcome;
    }

    /*  `document.save`, the same way. */
    Outcome invokeSave (ShowDocument& document, DocumentSession& session)
    {
        return invokeBundleCommand (document, session, "document.save");
    }

    /*  An earlier session's afternoon, left in `recovery/` the way a crash
        leaves it: opened, edited, autosaved, and never saved or closed. The
        document and its session go out of scope at the closing brace, which is
        as close to a pulled plug as one process can come - nothing tidies up. */
    void abandonAnAfternoon (const juce::File& folder, const std::string& name)
    {
        ShowDocument earlier;
        REQUIRE (Bundle::open (folder, earlier).ok);

        DocumentSession earlierSession { folder, earlier.showRevision() };
        REQUIRE (earlier.setAttribute (houseToHalfName, name).ok);
        REQUIRE (invokeBundleCommand (earlier, earlierSession, "document.autosave", {}, 100).applied);
        REQUIRE (Bundle::hasRecovery (folder));
    }

    bool canUndoIn (const ShowDocument& document)
    {
        return document.history (UndoDomain::document).canUndo();
    }
}

//==============================================================================
TEST_CASE ("bundle: the committed fixture opens with nothing to say")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    ShowDocument document;
    const auto result = Bundle::open (fixtureBundle(), document);

    for (const auto& problem : result.problems)
        INFO ("problem: " << problem);

    CHECK (result.ok);
    CHECK (result.problems.empty());

    // The show came in typed, and the state came in with it.
    CHECK (document.getAttribute ("/godot/cue/B3N8R5TW/name") == std::string ("House to half"));
    CHECK (document.getAttribute (standbyAddress) == std::string ("B3N8R5TW"));
}

TEST_CASE ("bundle: open then save is byte-identical, file for file")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  The claim the whole format rests on. A replay compares its result against
        the saved bundle directly, with no normaliser in between, and that is
        only honest if a bundle that nobody changed comes back the same bytes -
        under either locale, on any platform, with no timestamp anywhere in it
        to differ. */
    ShowDocument document;
    REQUIRE (Bundle::open (fixtureBundle(), document).ok);

    TempBundle temp { "minimal" };
    const auto saved = Bundle::save (temp.folder, document);

    for (const auto& problem : saved.problems)
        INFO ("problem: " << problem);

    REQUIRE (saved.ok);

    for (const auto& name : { "show.xml", "state.xml", "minimal.wfg" })
    {
        INFO ("file: " << name);
        CHECK (readBytes (temp.folder.getChildFile (name))
                 == readBytes (fixtureBundle().getChildFile (name)));
    }
}

TEST_CASE ("bundle: every file it writes ends its lines with LF")
{
    ShowDocument document;
    REQUIRE (Bundle::open (fixtureBundle(), document).ok);

    TempBundle temp { "minimal" };
    REQUIRE (Bundle::save (temp.folder, document).ok);

    for (const auto& name : { "show.xml", "state.xml", "minimal.wfg" })
    {
        INFO ("file: " << name);
        const auto text = readBytes (temp.folder.getChildFile (name));

        CHECK (text.find('\r') == std::string::npos);
        CHECK (! text.empty());
        CHECK (text.back() == '\n');
    }
}

TEST_CASE ("bundle: a save over an existing bundle is byte-identical, and leaves no temp behind")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  THE CASE THE ATOMIC WRITE EXISTS FOR, which the byte-identity case above
        does not reach. That one saves into an empty folder, and a target that
        does not exist is handed by `replaceFileIn` to `moveFileTo` - every
        file there is a first save. This one saves over the files `open` has
        just read, which is the replace branch: the one that must leave the
        bytes exactly as they were AND leave nothing of itself beside them.

        TWICE, because the second save finds the first one's files where the
        first found the fixture's - which is a session's second Ctrl-S, and
        the save that would trip over a temp the first had failed to take away. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    REQUIRE (Bundle::save (temp.folder, document).ok);
    REQUIRE (Bundle::save (temp.folder, document).ok);

    for (const auto& name : { "show.xml", "state.xml", "minimal.wfg" })
    {
        INFO ("file: " << name);
        CHECK (readBytes (temp.folder.getChildFile (name))
                 == readBytes (fixtureBundle().getChildFile (name)));
    }

    const auto left = tempsIn (temp.folder);
    CHECK_MESSAGE (left.empty(), "left behind: " << (left.empty() ? std::string() : left.front()));
}

TEST_CASE ("document.save: a save that lands replaces the show, and puts the dot out")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  `/godot/document/dirty` is `isDirty (document, session)`, published by
        the serve verb's after-tick; this is the half of it a unit test can
        reach - the stamp the save handler puts on the session, and the
        comparison the after-tick makes. The black-box driver asserts the
        published node. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    CHECK_FALSE (isDirty (document, session));

    REQUIRE (document.setAttribute (houseToHalfName, "Renamed before the save").ok);
    CHECK (isDirty (document, session));

    const auto outcome = invokeSave (document, session);

    CHECK (outcome.applied);
    CHECK (session.savedRevision == document.showRevision());
    CHECK_FALSE (isDirty (document, session));

    /*  REPLACED, and not merely left alone: the byte-identity case above could
        pass by writing nothing at all, and this one could not. */
    ShowDocument reopened;
    REQUIRE (Bundle::open (temp.folder, reopened).ok);
    CHECK (reopened.getAttribute (houseToHalfName) == std::string ("Renamed before the save"));

    CHECK (tempsIn (temp.folder).empty());

    /*  And the operator's position does not light it again. A standby is
        state.xml's, and a dot that came back on at the first GO after a save
        would be the dot nobody reads (plan decision 4). */
    REQUIRE (document.setAttribute (standbyAddress, "D9FH2JKA").ok);
    CHECK_FALSE (isDirty (document, session));
}

TEST_CASE ("document.save: a write that cannot land leaves the old show.xml intact, keeps the dot lit, and says why")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Making the write fail is the interesting half of this, and only some
        ways of doing it are honest on all three platforms. A read-only folder
        is not one: an elevated Windows process ignores the attribute, so the
        case would pass on POSIX and quietly save on somebody's box. A parent
        that does not exist is not one either, because `Bundle::save` creates
        its folder and every missing parent with it, and the save would
        succeed. A DIRECTORY standing where a file has to be is refused by
        CreateFile and by open(2) alike - EISDIR there, access denied here,
        whatever the privilege - and arranging it needs none.

        WHERE the directory stands moved in PR 5.2, and the move is the point.
        A save no longer opens show.xml at all: it opens the temp beside it, so
        the temp's name is the file that has to be a directory. PR 5.1 put the
        directory in show.xml's own place, and that stopped being portable the
        moment the save became a replace - Windows' ReplaceFile and macOS's
        copy both refuse an empty directory as a target, but on Linux JUCE's
        fallback after the failed rename deletes the destination first, rmdir
        succeeds, and the save goes through. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    const auto showXml = temp.folder.getChildFile ("show.xml");
    const auto before = readBytes (showXml);

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };

    /*  An edit, so the refused save had something to write that is not what
        is already there. The fixture IS what an unedited save writes, and
        finding it unchanged after a save of the same bytes would prove
        nothing. */
    REQUIRE (document.setAttribute (houseToHalfName, "Never reached the disk").ok);

    REQUIRE (Bundle::temporaryFor (showXml).createDirectory().wasOk());

    /*  A sentinel in state.xml, written after the open so the open did not
        read it. These bytes are ones no save could produce, and show.xml is
        written first, so their survival says the save stopped at the file it
        could not write and touched nothing else. */
    const std::string sentinel = "<!-- not a thing save would ever write -->\n";
    const auto stateXml = temp.folder.getChildFile ("state.xml");
    writeBytes (stateXml, sentinel);

    const auto stampBefore = session.savedRevision;
    const auto outcome = invokeSave (document, session);

    /*  APPLIED, AND THAT IS A CORRECTION (PR 5.5, second half). Until the bytes
        left the tick thread this save was refused with `write-failed`, because
        its `A` meant the bytes had landed. Now the record is written when the
        snapshot is handed to the writer, which it was; the failure comes back
        afterwards, through the writer's completion, and cannot un-write a
        record already in the log. What it does instead is below. */
    CHECK (outcome.applied);

    /*  THE SHOW THAT WAS ON DISK IS STILL THE SHOW ON DISK, byte for byte -
        the one promise §14.10 makes about a save that fails. */
    CHECK (readBytes (showXml) == before);
    CHECK (readBytes (stateXml) == sentinel);

    /*  And the dot is still lit: the stamp is put on a save that landed and
        on no other, so the operator is still told there is work not on disk -
        because there is. */
    CHECK (session.savedRevision == stampBefore);
    CHECK (isDirty (document, session));

    /*  AND IT SAYS WHY, where a client can read it: which command, and the
        writer's own sentence naming the file it could not write - the
        diagnosis the reason code never had room for. */
    INFO ("writeError: " << session.writeError);
    CHECK (session.writeError.rfind ("document.save at tick ", 0) == 0);
    CHECK (session.writeError.find (Bundle::temporaryFor (showXml).getFileName().toStdString())
             != std::string::npos);
}

//==============================================================================
/*  PR 5.5: AN AUTOSAVE NOBODY ASKS FOR, AND WHAT A PERSON DOES ABOUT IT.

    These cases prove what one process can prove: that the recovery bytes
    round-trip, that the commands around them do what §14.10's two tables say
    and deliberately no more, and that the decision to write sits exactly on
    its boundaries. What they cannot prove is the crash half, because a unit
    test cannot die - `tests/blackbox/phase5_document.py` kills a real process
    on a real folder and starts another on it, which is the only honest way to
    ask whether the work survived. */

TEST_CASE ("autosaveDue: nothing to write, the quiet boundary, and the ceiling boundary")
{
    /*  THE DECISION, AT ITS EDGES, WITH NO DISK. A fresh document is at show
        revision 1 and a session nobody stamped reads 0, so the document below
        is dirty and has never been autosaved until a case says otherwise. */
    ShowDocument document;
    REQUIRE (document.showRevision() > 0);

    /*  THE TWO NUMBERS THEMSELVES, pinned, because they are plan decision 5 and
        here to be overruled early: when they are overruled it should be this
        line that changes, on purpose, and not a boundary below that drifts. */
    CHECK (autosaveQuietTicks == 100);
    CHECK (autosaveCeilingTicks == 1500);

    //--------------------------------------------------------------------------
    // Not dirty: nothing to write, at any tick, however long the quiet.
    {
        DocumentSession session;
        session.savedRevision = document.showRevision();

        CHECK_FALSE (autosaveDue (document, session, autosaveQuietTicks));
        CHECK_FALSE (autosaveDue (document, session, autosaveCeilingTicks));
        CHECK_FALSE (autosaveDue (document, session, 10 * autosaveCeilingTicks));
    }

    //--------------------------------------------------------------------------
    // Two seconds of quiet: 99 ticks is not quiet, 100 is.
    {
        DocumentSession session;
        session.lastChangeTick = 1000;

        CHECK_FALSE (autosaveDue (document, session, 1000 + autosaveQuietTicks - 1));
        CHECK (autosaveDue (document, session, 1000 + autosaveQuietTicks));
    }

    //--------------------------------------------------------------------------
    /*  The thirty-second ceiling, reached by somebody who never stops: the last
        change is always one tick ago, so the quiet never comes, and the only
        thing that can write is the ceiling. 1499 ticks since the last attempt
        is not enough; 1500 is. */
    {
        DocumentSession session;
        session.lastAutosaveTick = 2000;

        session.lastChangeTick = 2000 + autosaveCeilingTicks - 2;
        CHECK_FALSE (autosaveDue (document, session, 2000 + autosaveCeilingTicks - 1));

        session.lastChangeTick = 2000 + autosaveCeilingTicks - 1;
        CHECK (autosaveDue (document, session, 2000 + autosaveCeilingTicks));
    }

    //--------------------------------------------------------------------------
    /*  Already in `recovery/`: the same bytes are never written twice, which is
        what stops a dirty, idle show autosaving on every tick after its quiet -
        at both boundaries at once. */
    {
        DocumentSession session;
        session.autosavedRevision = document.showRevision();
        session.lastChangeTick = 1000;

        CHECK_FALSE (autosaveDue (document, session, 1000 + autosaveQuietTicks));
        CHECK_FALSE (autosaveDue (document, session, 1000 + autosaveCeilingTicks));
    }

    //--------------------------------------------------------------------------
    /*  A FAILED ATTEMPT WAITS FOR THE CEILING. The edit came before the
        attempt, so the quiet it ended is spent; without that term a folder that
        has gone would be tried fifty times a second. */
    {
        DocumentSession session;
        session.lastChangeTick = 1000;
        session.lastAutosaveTick = 1000 + autosaveQuietTicks;

        CHECK_FALSE (autosaveDue (document, session, 1000 + autosaveQuietTicks + 1));
        CHECK_FALSE (autosaveDue (document, session,
                                  1000 + autosaveQuietTicks + autosaveCeilingTicks - 1));
        CHECK (autosaveDue (document, session, 1000 + autosaveQuietTicks + autosaveCeilingTicks));
    }

    //--------------------------------------------------------------------------
    /*  An edit in the SAME tick as an attempt is still owed its own write: the
        attempt may have serialised the document before that edit was applied,
        and `>=` rather than `>` is what keeps it from waiting thirty seconds. */
    {
        DocumentSession session;
        session.lastChangeTick = 5000;
        session.lastAutosaveTick = 5000;

        CHECK (autosaveDue (document, session, 5000 + autosaveQuietTicks));
    }

    //--------------------------------------------------------------------------
    /*  AND AN EARLIER SESSION'S UNANSWERED AFTERNOON DOES NOT STOP IT. The first
        half of PR 5.5 suspended the autosave while an offer stood, which kept
        the old afternoon safe by leaving the new one unprotected; at the
        author's direction (2026-09-11) the writer moves the offer aside first
        instead, so the decision no longer asks about it at all - at either
        boundary. */
    {
        DocumentSession session;
        session.offeredRecovery = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                    .getChildFile ("somebody").getChildFile ("recovery");
        session.lastChangeTick = 1000;

        CHECK (autosaveDue (document, session, 1000 + autosaveQuietTicks));

        session.lastAutosaveTick = 1000;
        session.lastChangeTick = 1000 + autosaveCeilingTicks - 1;
        CHECK (autosaveDue (document, session, 1000 + autosaveCeilingTicks));
    }
}

TEST_CASE ("recovery: the autosave's bytes round-trip in one process, and never touch the authored pair")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };

    REQUIRE (document.setAttribute (houseToHalfName, "Renamed and never saved").ok);
    REQUIRE (document.setAttribute (standbyAddress, "D9FH2JKA").ok);
    REQUIRE (isDirty (document, session));

    const auto outcome = invokeBundleCommand (document, session, "document.autosave", {}, 4321);

    CHECK (outcome.applied);
    CHECK (outcome.appliedArgs.empty());

    /*  The session's own record of it: WHEN it was attempted, and WHICH show
        the folder now holds. */
    CHECK (session.lastAutosaveTick == 4321);
    CHECK (session.autosavedRevision == document.showRevision());

    /*  AND THE DOT STAYS LIT. What is on disk as the show is still what it
        was; an autosave is not a save, and a light that went out here would be
        telling somebody their work was saved when it was not. */
    CHECK (isDirty (document, session));

    /*  NEVER THE AUTHORED PAIR, byte for byte - which is what keeps not saving
        a gesture somebody can make (§14.10). */
    for (const auto& name : { "show.xml", "state.xml", "minimal.wfg" })
    {
        INFO ("file: " << name);
        CHECK (readBytes (temp.folder.getChildFile (name))
                 == readBytes (fixtureBundle().getChildFile (name)));
    }

    /*  What `recovery/` holds is exactly what the two writers produce - the
        same canonical bytes a save would have put in the authored files. */
    CHECK (readBytes (Bundle::recoveryShowFile (temp.folder)) == CanonicalXml::write (document));
    CHECK (readBytes (Bundle::recoveryStateFile (temp.folder)) == EphemeralState::write (document));

    /*  No manifest, so nothing walking a disk for shows mistakes the folder for
        one; and no temp left beside the files it replaced. */
    CHECK (Bundle::recoveryFolder (temp.folder)
             .findChildFiles (juce::File::findFiles, false, "*.wfg").isEmpty());
    CHECK (tempsIn (Bundle::recoveryFolder (temp.folder)).empty());

    /*  And the log header does not see it: a session that autosaved still
        hashes as the bundle it was recorded against. */
    CHECK (Bundle::contentHash (temp.folder) == Bundle::contentHash (fixtureBundle()));

    //--------------------------------------------------------------------------
    /*  THE ROUND TRIP. A fresh document opens the authored show - the morning -
        and the recovery on top of it brings the afternoon back, the operator's
        position included, to the byte. */
    ShowDocument reopened;
    REQUIRE (Bundle::open (temp.folder, reopened).ok);
    CHECK (reopened.getAttribute (houseToHalfName) == std::string ("House to half"));

    const auto recovered = Bundle::openRecovery (temp.folder, reopened);

    for (const auto& problem : recovered.problems)
        INFO ("problem: " << problem);

    CHECK (recovered.ok);
    CHECK (recovered.problems.empty());
    CHECK (reopened.getAttribute (houseToHalfName) == std::string ("Renamed and never saved"));
    CHECK (reopened.getAttribute (standbyAddress) == std::string ("D9FH2JKA"));
    CHECK (CanonicalXml::write (reopened) == CanonicalXml::write (document));
    CHECK (EphemeralState::write (reopened) == EphemeralState::write (document));
}

TEST_CASE ("document.recover: the afternoon comes back dirty, with no history, and the offer is answered")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "The afternoon nobody saved");

    //--------------------------------------------------------------------------
    // The next session, which finds it and adopts nothing on its own.
    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);

    REQUIRE (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));
    CHECK (document.getAttribute (houseToHalfName) == std::string ("House to half"));
    CHECK_FALSE (isDirty (document, session));

    /*  Something on the stack, so that "the history is cleared" is a claim
        about a history that had something in it. */
    REQUIRE (document.setAttribute (houseToHalfName, "Typed this morning").ok);
    REQUIRE (canUndoIn (document));

    const auto outcome = invokeBundleCommand (document, session, "document.recover");

    CHECK (outcome.applied);
    CHECK (document.getAttribute (houseToHalfName) == std::string ("The afternoon nobody saved"));

    /*  LIT, DELIBERATELY: the recovered work is not on disk as the show, and
        the dot is telling the truth. `recover` re-stamps nothing. */
    CHECK (isDirty (document, session));

    // The history went with the show it was about.
    CHECK_FALSE (canUndoIn (document));

    /*  The offer is answered, and - adopted where it stood - `recovery/` is now
        this session's own, holding exactly this show. */
    CHECK_FALSE (hasRecoveryOffer (session));
    CHECK (session.autosavedRevision == document.showRevision());

    /*  NOT DELETED by adopting it: only a save or a discard removes the folder,
        and until then a crash would find the afternoon there again. */
    CHECK (Bundle::hasRecovery (temp.folder));

    //--------------------------------------------------------------------------
    /*  A SAVE MAKES THE WORK THE SHOW, puts the dot out, and takes the folder
        with it - so the next start offers nothing it has already kept. */
    CHECK (invokeSave (document, session).applied);
    CHECK_FALSE (isDirty (document, session));
    CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
    CHECK (session.autosavedRevision == 0u);

    ShowDocument reopened;
    REQUIRE (Bundle::open (temp.folder, reopened).ok);
    CHECK (reopened.getAttribute (houseToHalfName) == std::string ("The afternoon nobody saved"));

    //--------------------------------------------------------------------------
    // And now there is nothing to recover and nothing to discard.
    const auto again = invokeBundleCommand (document, session, "document.recover");
    CHECK_FALSE (again.applied);
    CHECK (again.reason == reason::noRecovery);

    const auto discard = invokeBundleCommand (document, session, "document.discardRecovery");
    CHECK_FALSE (discard.applied);
    CHECK (discard.reason == reason::noRecovery);

    /*  The word, spelled out: a reason code is part of the log format and so a
        contract, and a rename would be a format change that has to fail here. */
    CHECK (std::string (reason::noRecovery) == "no-recovery");
}

TEST_CASE ("document.recover: a torn or absent recovery file is refused, and the document is untouched")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };

    REQUIRE (document.setAttribute (houseToHalfName, "Still on screen").ok);
    REQUIRE (canUndoIn (document));

    const auto revisionBefore = document.revision();
    const auto showBefore = CanonicalXml::write (document);
    const auto stateBefore = EphemeralState::write (document);

    //--------------------------------------------------------------------------
    // Absent: nothing offered, and nothing there to adopt.
    {
        const auto outcome = invokeBundleCommand (document, session, "document.recover");

        CHECK_FALSE (outcome.applied);
        CHECK (outcome.reason == reason::noRecovery);
    }

    //--------------------------------------------------------------------------
    /*  TORN: the first half of a show, cut off mid-element - the file the
        atomic write exists to make impossible, stood there by hand because a
        power cut on some platform will one day manage it anyway. And the offer
        is standing, so the refusal is not the absence of an offer talking. */
    writeBytes (Bundle::recoveryShowFile (temp.folder), showBefore.substr (0, showBefore.size() / 2));
    session.offeredRecovery = Bundle::recoveryFolder (temp.folder);

    {
        const auto outcome = invokeBundleCommand (document, session, "document.recover");

        CHECK_FALSE (outcome.applied);
        CHECK (outcome.reason == reason::noRecovery);
    }

    /*  UNTOUCHED, in every sense a later command could notice: the same
        revision, the same bytes out of both writers, the same history, and the
        offer still standing - a refusal answers nothing. */
    CHECK (document.revision() == revisionBefore);
    CHECK (CanonicalXml::write (document) == showBefore);
    CHECK (EphemeralState::write (document) == stateBefore);
    CHECK (document.getAttribute (houseToHalfName) == std::string ("Still on screen"));
    CHECK (canUndoIn (document));
    CHECK (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));

    // And the reader says which file and why, for anybody asking it directly.
    const auto direct = Bundle::openRecovery (temp.folder, document);
    CHECK_FALSE (direct.ok);
    CHECK (mentions (direct.problems, "recovery/show.xml"));
    CHECK (CanonicalXml::write (document) == showBefore);
}

TEST_CASE ("document.revert: the disk wins, the history goes, and the dot goes OUT")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };

    REQUIRE (document.setAttribute (houseToHalfName, "An afternoon somebody regrets").ok);
    REQUIRE (document.setAttribute (standbyAddress, "D9FH2JKA").ok);
    REQUIRE (isDirty (document, session));
    REQUIRE (canUndoIn (document));

    // This session's own autosave of the afternoon about to be thrown away.
    REQUIRE (invokeBundleCommand (document, session, "document.autosave", {}, 10).applied);
    REQUIRE (Bundle::hasRecovery (temp.folder));

    const auto outcome = invokeBundleCommand (document, session, "document.revert");

    CHECK (outcome.applied);
    CHECK (document.getAttribute (houseToHalfName) == std::string ("House to half"));

    /*  AND ITS COPY GOES WITH IT, on `document.save`'s terms: the document and
        the folder now hold the same show, and a crash before the next edit
        must not offer back the very afternoon the operator just threw away. */
    CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
    CHECK (session.autosavedRevision == 0u);

    /*  THE DOT GOES OUT, which takes a re-stamp: `adopt` counts a load as the
        largest change there is and moves the show counter, so a revert that
        did not re-stamp would report unsaved changes at the instant the
        document matched the disk. */
    CHECK_FALSE (isDirty (document, session));
    CHECK (session.savedRevision == document.showRevision());

    CHECK_FALSE (canUndoIn (document));

    /*  The pointer comes back with the document it is in, to what state.xml
        says - not as a side effect of the revert but because loading a
        document is the whole of what it does (§14.7). */
    CHECK (document.getAttribute (standbyAddress) == std::string ("B3N8R5TW"));

    //--------------------------------------------------------------------------
    /*  A FOLDER THAT IS NO LONGER A READABLE BUNDLE: `bad-address`, because
        this is the one refusal here that genuinely sends somebody to look at a
        path - and the document on screen is left exactly as it was. */
    REQUIRE (document.setAttribute (houseToHalfName, "Edited again").ok);
    REQUIRE (temp.folder.getChildFile ("show.xml").deleteFile());

    const auto refused = invokeBundleCommand (document, session, "document.revert");

    CHECK_FALSE (refused.applied);
    CHECK (refused.reason == reason::badAddress);
    CHECK (document.getAttribute (houseToHalfName) == std::string ("Edited again"));
    CHECK (isDirty (document, session));
}

TEST_CASE ("document.revert leaves an earlier session's unanswered recovery where it is")
{
    /*  A revert says the bundle wins. It says nothing about somebody else's
        afternoon, and throwing that away as a side effect of a different
        gesture is the decision §14.10 keeps for the operator. */
    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "Somebody else's afternoon");

    const auto theirs = readBytes (Bundle::recoveryShowFile (temp.folder));

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);
    REQUIRE (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));

    REQUIRE (document.setAttribute (houseToHalfName, "Typed this morning").ok);
    CHECK (invokeBundleCommand (document, session, "document.revert").applied);

    CHECK_FALSE (isDirty (document, session));
    CHECK (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));
    CHECK (readBytes (Bundle::recoveryShowFile (temp.folder)) == theirs);
}

TEST_CASE ("the lock refuses revert and recover in their own handlers, and none of the byte writers")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    /*  An earlier session's afternoon, so that `document.discardRecovery` has
        something to answer: since PR 5.5's second half it acts on an offer, and
        a session with none is refused `no-recovery` before any lock is asked. */
    abandonAnAfternoon (temp.folder, "Somebody else's afternoon");

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);
    REQUIRE (hasRecoveryOffer (session));

    /*  This session's first autosave moves that afternoon aside - the rule the
        author chose (§14.10) - and writes this session's own. */
    REQUIRE (document.setAttribute (houseToHalfName, "Edited before the lock").ok);
    REQUIRE (invokeBundleCommand (document, session, "document.autosave", {}, 10).applied);
    REQUIRE (session.offeredRecovery == temp.folder.getChildFile ("recovery.previous.1"));

    REQUIRE (document.setAttribute ("/godot/document/locked", "true").ok);
    REQUIRE (document.isLocked());

    /*  THE TWO THAT REPLACE THE SHOW. `adopt` is a hatch and not a door, so the
        four predicates never see them; and it replaces the root, lock included,
        so either of them could otherwise unlock a locked show because a file
        on disk said so (§14.11). */
    const auto revert = invokeBundleCommand (document, session, "document.revert");
    CHECK_FALSE (revert.applied);
    CHECK (revert.reason == reason::locked);

    const auto recover = invokeBundleCommand (document, session, "document.recover");
    CHECK_FALSE (recover.applied);
    CHECK (recover.reason == reason::locked);

    CHECK (document.isLocked());
    CHECK (document.getAttribute (houseToHalfName) == std::string ("Edited before the lock"));

    /*  AND THE FOUR THAT WRITE BYTES, which keep working: saving during a
        locked show is the point of locking it (§14.7). The offer is still
        standing - a refused recover answers nothing - so the discard has
        something to delete, and deletes it where the autosave moved it. */
    CHECK (hasRecoveryOffer (session));
    CHECK (invokeBundleCommand (document, session, "document.autosave", {}, 20).applied);
    CHECK (invokeBundleCommand (document, session, "document.discardRecovery").applied);
    CHECK_FALSE (hasRecoveryOffer (session));
    CHECK_FALSE (temp.folder.getChildFile ("recovery.previous.1").exists());
    CHECK (invokeBundleCommand (document, session, "document.saveAs",
                                { osc::Value::string (temp.parent.getChildFile ("archive")
                                                        .getFullPathName().toStdString()) })
             .applied);
    CHECK (invokeSave (document, session).applied);
    CHECK (session.writeError.empty());
}

TEST_CASE ("document.saveAs: a bundle open accepts, with namespaces/ and without media/, and the session stays put")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    /*  Media the copy must NOT carry - the heavy half of a bundle, and a copy
        of it is a decision about a disk rather than about a show. */
    writeBytes (temp.folder.getChildFile ("media").getChildFile ("tone.wav"), "not really a wav\n");

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };

    REQUIRE (document.setAttribute (houseToHalfName, "Kept for the archive").ok);

    // And a recovery the copy must not carry either: this session's afternoon.
    REQUIRE (invokeBundleCommand (document, session, "document.autosave", {}, 10).applied);

    const auto archive = temp.parent.getChildFile ("archive");
    const auto path = archive.getFullPathName().toStdString();
    const auto folderBefore = session.folder;
    const auto stampBefore = session.savedRevision;

    const auto outcome = invokeBundleCommand (document, session, "document.saveAs",
                                              { osc::Value::string (path) });

    CHECK (outcome.applied);

    // The record carries the path as it was asked for.
    REQUIRE (outcome.appliedArgs.size() == 1u);
    CHECK (outcome.appliedArgs[0].getString() == path);

    //--------------------------------------------------------------------------
    /*  A BUNDLE `open` ACCEPTS, with nothing to say about it: the manifest is
        named after the folder it landed in, which is where a copy stops being a
        bundle if nobody writes it. */
    ShowDocument copied;
    const auto opened = Bundle::open (archive, copied);

    for (const auto& problem : opened.problems)
        INFO ("problem: " << problem);

    CHECK (opened.ok);
    CHECK (opened.problems.empty());
    CHECK (archive.getChildFile ("archive.wfg").existsAsFile());
    CHECK (copied.getAttribute (houseToHalfName) == std::string ("Kept for the archive"));

    // The descriptions, copied byte for byte: save-plus-a-copy, and it says so.
    for (const auto& name : { "console.json", "wfs-diy.json" })
    {
        INFO ("namespace file: " << name);
        CHECK (readBytes (Bundle::namespacesFolder (archive).getChildFile (name))
                 == readBytes (Bundle::namespacesFolder (temp.folder).getChildFile (name)));
    }

    CHECK_FALSE (archive.getChildFile ("media").exists());
    CHECK_FALSE (Bundle::recoveryFolder (archive).exists());

    //--------------------------------------------------------------------------
    /*  IT DOES NOT RE-POINT THE SESSION. The folder it names is the one it
        opened, the stamp is where it was, the dot is lit - because this
        session's own file is still behind - and that file is untouched. */
    CHECK (session.folder == folderBefore);
    CHECK (session.savedRevision == stampBefore);
    CHECK (isDirty (document, session));
    CHECK (readBytes (temp.folder.getChildFile ("show.xml"))
             == readBytes (fixtureBundle().getChildFile ("show.xml")));

    /*  So the next Ctrl-S goes where the operator last opened, and the archive
        keeps the moment it was made. */
    REQUIRE (document.setAttribute (houseToHalfName, "After the archive").ok);
    CHECK (invokeSave (document, session).applied);

    ShowDocument fromArchive;
    REQUIRE (Bundle::open (archive, fromArchive).ok);
    CHECK (fromArchive.getAttribute (houseToHalfName) == std::string ("Kept for the archive"));

    ShowDocument fromSession;
    REQUIRE (Bundle::open (temp.folder, fromSession).ok);
    CHECK (fromSession.getAttribute (houseToHalfName) == std::string ("After the archive"));
}

TEST_CASE ("document.saveAs under a replay lands inside --out, whatever the path it names")
{
    /*  A replayed copy must never write where the live one did - that folder is
        somebody's archive on the machine that recorded the log - and a path
        recorded on Windows is not even absolute on the Mac mini. So a replay's
        copies go to `<out>/saveAs/<last component>`, and the record still
        carries the path as it was asked for, so the replay compares line for
        line. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    const auto out = temp.parent.getChildFile ("replayed");
    DocumentSession session { out, document.showRevision() };

    /*  One path spelled with backslashes and one with forward slashes and a
        trailing one, each of which must reduce to `archive` on every platform.
        Both are rooted in this test's own scratch folder, so that if the
        re-rooting ever broke, the stray copy would land somewhere this test
        cleans up and the count at the end would catch it - rather than in a
        real folder on whichever machine ran the suite. */
    const auto root = temp.parent.getFullPathName().toStdString();

    for (const auto& asked : { root + "\\elsewhere\\archive",
                               root + "/elsewhere/archive/" })
    {
        INFO ("path in the record: " << asked);

        const auto outcome = invokeBundleCommand (document, session, "document.saveAs",
                                                  { osc::Value::string (asked) }, 0, out);

        CHECK (outcome.applied);
        REQUIRE (outcome.appliedArgs.size() == 1u);
        CHECK (outcome.appliedArgs[0].getString() == asked);

        ShowDocument copied;
        CHECK (Bundle::open (out.getChildFile ("saveAs").getChildFile ("archive"), copied).ok);
    }

    // Nothing was written anywhere but under the folder it was handed.
    CHECK (temp.parent.findChildFiles (juce::File::findDirectories, false).size() == 2);
}

TEST_CASE ("document.autosave: a bundle that has gone is refused, and no twin is made where it was")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.lastChangeTick = 600;

    REQUIRE (document.setAttribute (houseToHalfName, "Renamed on a stick").ok);

    // Somebody moves the bundle mid-session - or pulls out the stick it was on.
    REQUIRE (temp.folder.moveFileTo (temp.parent.getChildFile ("moved")));
    REQUIRE_FALSE (temp.folder.exists());

    const auto outcome = invokeBundleCommand (document, session, "document.autosave", {}, 777);

    /*  REFUSED AT ONCE, ON THE TICK THREAD, AND STILL IN THE LOG. The bytes
        moved to a writer thread in PR 5.5's second half; this check did not,
        because it is one `stat` and the refusal an unattended writer most needs
        to leave where somebody will read it. */
    CHECK_FALSE (outcome.applied);
    CHECK (outcome.reason == reason::writeFailed);

    /*  And the word itself, spelled out. The code is part of the log format
        and therefore a contract, so a rename that changed the text would be a
        format change and has to fail here rather than in somebody's parser. */
    CHECK (outcome.reason == "write-failed");

    /*  NO TWIN. An unattended writer that invents a folder is how a bundle
        acquires one: the real show with its media in one place, and three
        files at the old path with none, and nothing to say which the operator
        was working in (§14.10). */
    CHECK_FALSE (temp.folder.exists());

    /*  THE ATTEMPT IS STAMPED and the revision is not, so the decision waits
        for the ceiling rather than asking again on the very next tick. */
    CHECK (session.lastAutosaveTick == 777);
    CHECK (session.autosavedRevision == 0u);
    CHECK_FALSE (autosaveDue (document, session, 778));
    CHECK_FALSE (autosaveDue (document, session, 777 + autosaveCeilingTicks - 1));
    CHECK (autosaveDue (document, session, 777 + autosaveCeilingTicks));

    /*  And the asymmetry, pinned from the other side: `document.save` DOES make
        the folder, because somebody asked for it, and a person can see a new
        folder and undo it. */
    CHECK (invokeSave (document, session).applied);
    CHECK (temp.folder.getChildFile ("show.xml").existsAsFile());
    CHECK_FALSE (Bundle::namespacesFolder (temp.folder).exists());
}

TEST_CASE ("an unanswered recovery still in recovery/ survives this session's save, until somebody answers it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  THE ONE CASE WHERE `recovery/` IS NOT THIS SESSION'S TO DELETE. A save
        takes this session's own recovery away because the work has become the
        show; an earlier session's afternoon that no autosave has yet moved
        aside is still sitting in that folder, and a save says nothing about it.
        What happens when the autosave does move it is DocumentWriterTests.cpp's
        subject; this is the save that comes first. */
    TempBundle temp { "minimal" };
    temp.copyFixture();
    abandonAnAfternoon (temp.folder, "Somebody else's afternoon");

    const auto theirs = readBytes (Bundle::recoveryShowFile (temp.folder));

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    DocumentSession session { temp.folder, document.showRevision() };
    session.offeredRecovery = Bundle::offeredRecovery (temp.folder);
    REQUIRE (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));

    REQUIRE (document.setAttribute (houseToHalfName, "This session's work").ok);
    session.lastChangeTick = 50;

    /*  THE AUTOSAVE IS DUE, OFFER OR NO OFFER - the author's decision
        (§14.10): this session's edits are protected from the first quiet on,
        and the writer moves the afternoon aside before it writes. */
    CHECK (autosaveDue (document, session, 50 + autosaveQuietTicks));

    /*  BUT A SAVE THAT COMES FIRST LEAVES THE FOLDER ALONE. It makes THIS
        session's work the show; it does not make that afternoon anything, so
        the folder stays, byte for byte, and the offer stands. */
    CHECK (invokeSave (document, session).applied);
    CHECK_FALSE (isDirty (document, session));
    CHECK (session.offeredRecovery == Bundle::recoveryFolder (temp.folder));
    CHECK (readBytes (Bundle::recoveryShowFile (temp.folder)) == theirs);

    // Until somebody answers - here by throwing it away - after which it is gone.
    CHECK (invokeBundleCommand (document, session, "document.discardRecovery").applied);
    CHECK_FALSE (Bundle::recoveryFolder (temp.folder).exists());
    CHECK_FALSE (hasRecoveryOffer (session));
}

TEST_CASE ("M23: an autosave of a 500-cue show - the tick thread's snapshot and the writer's bytes, timed apart")
{
    /*  MEASUREMENT M23 (§14.14): does a 500-cue autosave fit inside a tick?

        ANSWERED ON THE WINDOWS BOX (PR 5.5, 2026-09-11), and the answer was no:
        21 ms at the median for the handler that then wrote the bytes, against a
        threshold of a quarter tick - 5 ms - and 1.4 ms of it serialising. So
        the snapshot stayed on the tick thread and the bytes moved to a writer
        thread on MountProbe's shape, and the figure that answers PRD §4.1 is
        now the TICK THREAD'S: the snapshot and the handoff, which is all a GO
        can be kept waiting behind. The writer's figure is what the platform
        charges and nobody on the GO path pays. This instrument keeps the two
        apart so that each still means what it says.

        APART BY CONSTRUCTION, NOT BY GUESSING AT A THREAD. The writer is a
        background one that is never started, so the handler queues its job and
        returns - that return is the tick thread's whole cost, snapshot, `stat`
        and push - and `drain` then performs the queued job on this thread,
        which is exactly the writer's work with no wake-up and no scheduler in
        it. The record is `document.autosave` either way, which is why nothing
        here gates on either number.

        ASSERTED IS A COUNT, NEVER A CLOCK: every autosave applied, every one
        landed, and the bytes on disk the bytes the snapshot held. A wall-clock
        threshold on a shared CI runner is a flaky test that teaches people to
        re-run the suite (§14.14, and Phase 4 before it), so the milliseconds
        are printed for somebody to quote and gate nothing. Quote the Release
        figure: a Debug build carries iterator debugging and measures the build.

        THE SHOW IS M18's SHAPE - five hundred `Media` cues, each with one
        `Feed` naming one of twenty slot identifiers - built straight into the
        tree for M18's reason: every door finds its parent with `findById`, a
        depth-first walk of the whole show, and five hundred of those would
        measure the door. The serialiser reads the tree and cannot tell the
        difference, and it writes a Feed's `slot` without resolving it, so the
        identifiers need not name slots this fixture declares - the bytes are
        the bytes of a show that did.

        A HUNDRED AUTOSAVES, as §14.14 asks, because the 99th percentile of
        fewer is not a 99th percentile. The first is made outside the timing:
        it finds no `recovery/` and so takes `replaceFileIn`'s move branch,
        while every one a show would actually make after that is a replace. */
    TempBundle temp { "minimal" };
    temp.copyFixture();

    ShowDocument document;
    REQUIRE (Bundle::open (temp.folder, document).ok);

    auto list = document.findById ("7K2QM9X4");
    REQUIRE (list.isValid());

    constexpr int cueCount = 500;
    constexpr int slotCount = 20;

    for (int n = 0; n < cueCount; ++n)
    {
        juce::ValueTree media { "Media" };
        media.setProperty (juce::Identifier ("id"), juce::String ("MA") + juce::String (100000 + n),
                           nullptr);
        media.setProperty (juce::Identifier ("name"), juce::String ("Cue ") + juce::String (n),
                           nullptr);
        media.setProperty (juce::Identifier ("file"), "tone.wav", nullptr);

        juce::ValueTree feed { "Feed" };
        feed.setProperty (juce::Identifier ("id"), juce::String ("FA") + juce::String (100000 + n),
                          nullptr);
        feed.setProperty (juce::Identifier ("slot"),
                          juce::String ("SA") + juce::String (100000 + n % slotCount), nullptr);
        feed.setProperty (juce::Identifier ("gains"), "1", nullptr);
        media.addChild (feed, -1, nullptr);

        list.addChild (media, -1, nullptr);
    }

    DocumentSession session;
    session.folder = temp.folder;

    CommandRegistry registry;
    DocumentWriter writer { DocumentWriter::Mode::background };
    registerBundleCommands (registry, document, session, writer);

    const auto* autosave = registry.find ("document.autosave");
    REQUIRE (autosave != nullptr);

    //--------------------------------------------------------------------------
    // The serialisation alone, for the breakdown.
    constexpr int serialisations = 10;
    std::string written;

    const auto beforeWrites = juce::Time::getMillisecondCounterHiRes();

    for (int n = 0; n < serialisations; ++n)
        written = CanonicalXml::write (document);

    const auto serialiseCost = (juce::Time::getMillisecondCounterHiRes() - beforeWrites)
                                 / serialisations;

    //--------------------------------------------------------------------------
    CommandContext context;
    context.tick = 1;
    REQUIRE (autosave->handler (context, {}).applied);
    writer.drain();
    settle (session, writer);

    constexpr std::size_t autosaves = 100;
    std::vector<double> tickThreadCosts;
    std::vector<double> writerCosts;
    tickThreadCosts.reserve (autosaves);
    writerCosts.reserve (autosaves);

    std::size_t applied = 0;

    for (std::size_t n = 0; n < autosaves; ++n)
    {
        context.tick = static_cast<std::int64_t> (n) + 2;

        const auto start = juce::Time::getMillisecondCounterHiRes();
        const auto outcome = autosave->handler (context, {});
        const auto handedOver = juce::Time::getMillisecondCounterHiRes();
        writer.drain();
        const auto onDisk = juce::Time::getMillisecondCounterHiRes();

        tickThreadCosts.push_back (handedOver - start);
        writerCosts.push_back (onDisk - handedOver);

        if (outcome.applied)
            ++applied;
    }

    std::size_t landed = 0;

    for (const auto& done : writer.takeCompletions())
        if (done.landed && done.problem.empty())
            ++landed;

    CHECK (applied == autosaves);
    CHECK (landed == autosaves);
    CHECK (readBytes (Bundle::recoveryShowFile (temp.folder)) == written);
    CHECK (tempsIn (Bundle::recoveryFolder (temp.folder)).empty());

    /*  The median of an even count is the mean of the middle two; the 99th
        percentile is the nearest rank, which for a hundred samples is the
        ninety-ninth smallest - one sample short of the worst. */
    struct Spread
    {
        double median = 0.0;
        double percentile99 = 0.0;
        double worst = 0.0;
    };

    const auto spreadOf = [] (std::vector<double> samples)
    {
        std::sort (samples.begin(), samples.end());

        Spread spread;
        spread.median = (samples[samples.size() / 2 - 1] + samples[samples.size() / 2]) / 2.0;
        spread.percentile99 = samples[samples.size() - 2];
        spread.worst = samples.back();
        return spread;
    };

    const auto tickThread = spreadOf (tickThreadCosts);
    const auto onTheWriter = spreadOf (writerCosts);

   #if JUCE_DEBUG
    const char* const build = "Debug";
   #else
    const char* const build = "Release";
   #endif

    MESSAGE ("M23  " << cueCount << " cues, " << written.size() << " bytes of show.xml, "
                     << build << " build: CanonicalXml::write alone " << serialiseCost
                     << " ms; over " << autosaves << " autosaves, the TICK THREAD (the"
                        " document.autosave handler: snapshot, stat and handoff) median "
                     << tickThread.median << " ms, 99th percentile " << tickThread.percentile99
                     << " ms, worst " << tickThread.worst << " ms; the WRITER (show.xml and"
                        " state.xml, each written, flushed and replaced) median "
                     << onTheWriter.median << " ms, 99th percentile " << onTheWriter.percentile99
                     << " ms, worst " << onTheWriter.worst
                     << " ms; the threshold for the tick thread is a quarter tick, 5 ms");
}

//==============================================================================
TEST_CASE ("bundle: a missing state.xml is normal, and silent")
{
    /*  The plan's question A, answered: standby is persisted. Its other half is
        that losing the file costs nothing but the standby - a show whose state
        was never committed, or deliberately excluded, opens with every
        ephemeral value at its default and no complaint. */
    TempBundle temp { "minimal" };
    temp.copyFixture();
    REQUIRE (temp.folder.getChildFile ("state.xml").deleteFile());

    ShowDocument document;
    const auto result = Bundle::open (temp.folder, document);

    CHECK (result.ok);
    CHECK (result.problems.empty());
    CHECK (document.getAttribute ("/godot/cue/B3N8R5TW/name") == std::string ("House to half"));
    CHECK (document.getAttribute (standbyAddress) == std::string (""));
}

TEST_CASE ("bundle: a standby pointing at a cue that is gone is reported, not fatal")
{
    TempBundle temp { "minimal" };
    temp.copyFixture();
    writeBytes (temp.folder.getChildFile ("state.xml"),
                "<State formatVersion=\"1\">\n"
                "  <List id=\"ZZZZZZZZ\" standby=\"B3N8R5TW\"/>\n"
                "</State>\n");

    ShowDocument document;
    const auto result = Bundle::open (temp.folder, document);

    // The show is what matters; a stale pointer into it must not cost it.
    CHECK (result.ok);
    CHECK (mentions (result.problems, "ZZZZZZZZ"));
    CHECK (document.getAttribute ("/godot/cue/B3N8R5TW/name") == std::string ("House to half"));
}

TEST_CASE ("bundle: a state.xml from the future is refused, and the show still opens")
{
    TempBundle temp { "minimal" };
    temp.copyFixture();
    writeBytes (temp.folder.getChildFile ("state.xml"),
                "<State formatVersion=\"99\">\n"
                "  <List id=\"7K2QM9X4\" standby=\"B3N8R5TW\"/>\n"
                "</State>\n");

    ShowDocument document;
    const auto result = Bundle::open (temp.folder, document);

    CHECK (result.ok);
    CHECK (mentions (result.problems, "format version 99"));

    /*  Nothing from it was applied. Half-reading a format we do not know is how
        a standby ends up on the wrong cue, which is worse than having none. */
    CHECK (document.getAttribute (standbyAddress) == std::string (""));
}

//==============================================================================
TEST_CASE ("bundle: a folder with no manifest is not a bundle")
{
    TempBundle temp { "minimal" };
    temp.copyFixture();
    REQUIRE (temp.folder.getChildFile ("minimal.wfg").deleteFile());

    ShowDocument document;
    const auto result = Bundle::open (temp.folder, document);

    CHECK_FALSE (result.ok);
    CHECK (mentions (result.problems, "not a Go.dot bundle"));
}

TEST_CASE ("bundle: a renamed folder still opens, and says the manifest no longer matches")
{
    TempBundle temp { "renamed" };
    REQUIRE (fixtureBundle().copyDirectoryTo (temp.folder));   // manifest is still minimal.wfg

    ShowDocument document;
    const auto result = Bundle::open (temp.folder, document);

    CHECK (result.ok);
    CHECK (mentions (result.problems, "renamed.wfg"));
}

TEST_CASE ("bundle: a manifest from the future is refused outright")
{
    TempBundle temp { "minimal" };
    temp.copyFixture();
    writeBytes (temp.folder.getChildFile ("minimal.wfg"), "<Bundle formatVersion=\"99\"/>\n");

    ShowDocument document;
    const auto result = Bundle::open (temp.folder, document);

    CHECK_FALSE (result.ok);
    CHECK (mentions (result.problems, "format version 99"));
}

//==============================================================================
TEST_CASE ("state never appears in show.xml, and decisions never appear in state.xml")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  PRD §4.10 made mechanical. The parameter table's `persist` column decides
        which file an attribute lands in, and the two writers read that same
        column - so a standby cannot end up in the document and a cue's name
        cannot end up in the state. */
    ShowDocument document;
    REQUIRE (Bundle::open (fixtureBundle(), document).ok);
    /*  D9FH2JKA, the Preshow GROUP, because it is a TOP-LEVEL child of the
        list. This said F7HR8TVD until PR 1.7, which is a cue nested inside
        that group - a standby the referential invariant now refuses, and
        rightly: the pointer names something GO can act on. */
    REQUIRE (document.setAttribute (standbyAddress, "D9FH2JKA").ok);

    const auto show = CanonicalXml::write (document);
    const auto state = EphemeralState::write (document);

    CHECK (show.find ("standby") == std::string::npos);
    CHECK (state.find ("standby=\"D9FH2JKA\"") != std::string::npos);

    CHECK (show.find ("House to half") != std::string::npos);
    CHECK (state.find ("House to half") == std::string::npos);
}

TEST_CASE ("show.xml carrying a standby is refused, and told where it goes")
{
    /*  Silently relocating it would hide a hand-edit; silently keeping it would
        leave two files disagreeing about where GO is pointed. */
    ShowDocument document;
    const auto result = CanonicalXml::read (
        "<Show>\n"
        "  <Lists>\n"
        "    <List id=\"7K2QM9X4\" name=\"Main\" standby=\"B3N8R5TW\"/>\n"
        "  </Lists>\n"
        "  <Mounts/>\n"
        "</Show>\n", document);

    CHECK_FALSE (result.ok);
    CHECK (mentions (result.problems, "state.xml"));
}

TEST_CASE ("state.xml: an object with nothing to remember is left out entirely")
{
    ShowDocument document;
    REQUIRE (Bundle::open (fixtureBundle(), document).ok);
    REQUIRE (document.setAttribute (standbyAddress, "").ok);

    const auto state = EphemeralState::write (document);

    // One list, four cues, one mount - and not one of them has anything to say.
    CHECK (state == "<State formatVersion=\"1\"/>\n");
}

TEST_CASE ("state.xml: an ephemeral value survives a save and a load")
{
    ShowDocument document;
    REQUIRE (Bundle::open (fixtureBundle(), document).ok);
    REQUIRE (document.setAttribute (standbyAddress, "D9FH2JKA").ok);

    TempBundle temp { "minimal" };
    REQUIRE (Bundle::save (temp.folder, document).ok);

    ShowDocument reopened;
    REQUIRE (Bundle::open (temp.folder, reopened).ok);

    CHECK (reopened.getAttribute (standbyAddress) == std::string ("D9FH2JKA"));
}

//==============================================================================
TEST_CASE ("relax ng: the grammar is deterministic")
{
    /*  It is committed and compared byte for byte, so anything that varied
        between two calls in one process would make the gate flap rather than
        fail - a map iterated in address order, a number formatted through the
        locale, a set of pointers. */
    CHECK (RelaxNg::generate() == RelaxNg::generate());
}

TEST_CASE ("relax ng: the committed grammar is the generated one")
{
    const juce::File committed { juce::String (std::string (WFG_REPO_ROOT))
                                   + "/docs/schema/show.rng" };

    REQUIRE_MESSAGE (committed.existsAsFile(),
                     "no committed grammar at " << committed.getFullPathName()
                       << "; generate it with: wfg schema --out=docs/schema/show.rng");

    CHECK (readBytes (committed) == RelaxNg::generate());
}

TEST_CASE ("relax ng: it describes the three roots a bundle has")
{
    const auto grammar = RelaxNg::generate();

    CHECK (grammar.find ("<ref name=\"Show\"/>") != std::string::npos);
    CHECK (grammar.find ("<ref name=\"State\"/>") != std::string::npos);
    CHECK (grammar.find ("<ref name=\"Bundle\"/>") != std::string::npos);

    // The identifier pattern is the one the engine actually enforces.
    CHECK (grammar.find (std::string (Id::pattern)) != std::string::npos);

    /*  A boolean is a closed set of the two canonical spellings, not
        xsd:boolean, which would also admit "1" and "0" - values our writer can
        never produce and a round-trip comparison would then fail on. */
    CHECK (grammar.find ("<value>true</value>") != std::string::npos);
    CHECK (grammar.find ("type=\"boolean\"") == std::string::npos);
}

TEST_CASE ("relax ng: a range in the table reaches the grammar as a facet")
{
    // cue/preWait is declared 0.. in the CSV, and seconds do not run backwards.
    const auto grammar = RelaxNg::generate();
    CHECK (grammar.find ("<param name=\"minInclusive\">0</param>") != std::string::npos);
}

//==============================================================================
TEST_CASE ("the identifier pattern and the identifier alphabet agree")
{
    /*  Id::pattern restates Id::alphabet in another language, which is a second
        place for the truth to live. This is the check that keeps them one:
        every letter Crockford keeps must match, and every letter he drops must
        not - I, L, O and U, the four a person reads as 1, 1, 0 and V. */
    const std::regex whole { "^" + std::string (Id::pattern) + "$" };

    for (const char c : Id::alphabet)
    {
        const std::string candidate (Id::length, c);
        INFO ("alphabet character: " << c);
        CHECK (std::regex_match (candidate, whole));
    }

    for (const char c : std::string ("ILOUilou"))
    {
        const std::string candidate (Id::length, c);
        INFO ("excluded character: " << c);
        CHECK_FALSE (std::regex_match (candidate, whole));
    }

    // And the length is the length, not one either side of it.
    CHECK_FALSE (std::regex_match (std::string (Id::length - 1, '0'), whole));
    CHECK_FALSE (std::regex_match (std::string (Id::length + 1, '0'), whole));
}

TEST_CASE ("the format version comes from the parameter table, not from a literal")
{
    const auto* row = Schema::instance().attribute (Schema::rootElement, "formatVersion");

    REQUIRE (row != nullptr);
    REQUIRE (row->hasDefault());
    CHECK (std::to_string (Schema::formatVersion()) == std::string (row->defaultText()));

    // And every file in a bundle is stamped with that one number.
    ShowDocument document;
    REQUIRE (Bundle::open (fixtureBundle(), document).ok);

    const auto stamp = "formatVersion=\"" + std::to_string (Schema::formatVersion()) + "\"";

    CHECK (EphemeralState::write (document).find (stamp) != std::string::npos);
    CHECK (readBytes (fixtureBundle().getChildFile ("minimal.wfg")).find (stamp)
             != std::string::npos);
}
