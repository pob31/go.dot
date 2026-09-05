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

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/EphemeralState.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/RelaxNg.h>
#include <wfg/engine/document/Schema.h>

#include <juce_core/juce_core.h>

#include <regex>
#include <string>

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
    REQUIRE (document.setAttribute (standbyAddress, "F7HR8TVD").ok);

    const auto show = CanonicalXml::write (document);
    const auto state = EphemeralState::write (document);

    CHECK (show.find ("standby") == std::string::npos);
    CHECK (state.find ("standby=\"F7HR8TVD\"") != std::string::npos);

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
    REQUIRE (document.setAttribute (standbyAddress, "E4GP6QSC").ok);

    TempBundle temp { "minimal" };
    REQUIRE (Bundle::save (temp.folder, document).ok);

    ShowDocument reopened;
    REQUIRE (Bundle::open (temp.folder, reopened).ok);

    CHECK (reopened.getAttribute (standbyAddress) == std::string ("E4GP6QSC"));
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
