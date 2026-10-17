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
    THE FONTS A SHOW CARRIES (namespace draft §62, AKX-ALC): its fonts/ folder
    and the one around it read, the face a text cue draws with chosen, the
    shelf's jobs - add, carry, remove, each said - the three commands, and
    Save as taking the folder with the show.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/InstalledFonts.h>
#include <wfg/engine/video/ShowFonts.h>

#include "FontFixture.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <utility>

#include <string>
#include <vector>

using namespace wfg;
using namespace wfg::test::fonts;

namespace
{
    /** The job's fields, split at its tabs: number, state, verb, subject, result, detail. */
    std::vector<std::string> fieldsOf (const std::string& job)
    {
        std::vector<std::string> out;
        juce::StringArray parts;
        parts.addTokens (juce::String::fromUTF8 (job.c_str()), "\t", {});
        for (const auto& part : parts)
            out.push_back (part.toStdString());
        while (out.size() < 6)
            out.emplace_back();
        return out;
    }

    void copyIn (const char* dejaVuFile, const juce::File& folder)
    {
        folder.createDirectory();
        REQUIRE (dejaVu (dejaVuFile).copyFileTo (folder.getChildFile (dejaVuFile)));
    }
}

TEST_CASE ("show fonts: a show's fonts/ read, each family named from its own files, the folder around after its own (§62, AKX, AKY)")
{
    Folder place;
    const auto show = place.file.getChildFile ("Show");
    show.createDirectory();

    video::ShowFonts nothing = video::ShowFonts::scan (show);
    CHECK (nothing.faces().empty());
    CHECK (nothing.listing().empty());
    CHECK_FALSE (nothing.choose ("DejaVu Sans Mono", false, false).has_value());

    copyIn ("DejaVuSansMono.ttf", show.getChildFile ("fonts"));
    copyIn ("DejaVuSansMono-Bold.ttf", show.getChildFile ("fonts"));
    show.getChildFile ("fonts/readme.txt").replaceWithText ("not a font, never read");
    show.getChildFile ("fonts/broken.ttf").replaceWithText ("not a font either, and named as one");

    //  The folder around: an oblique of the same family, and a family only it has.
    copyIn ("DejaVuSansMono-Oblique.ttf", place.file.getChildFile ("fonts"));
    write (place.file.getChildFile ("fonts/Around.ttf"), plain ("Around Only", "Regular").bytes());

    const auto fonts = video::ShowFonts::scan (show);
    CHECK (fonts.faces().size() == 4);
    REQUIRE (fonts.unreadable().size() == 1);
    CHECK (fonts.unreadable().front() == "broken.ttf: not a font file");

    CHECK (fonts.carries ("DejaVu Sans Mono"));
    CHECK (fonts.carries ("dejavu sans MONO"));
    CHECK (fonts.carries ("Around Only"));
    CHECK_FALSE (fonts.carries ("Arial"));
    CHECK_FALSE (fonts.carries (""));
    CHECK (fonts.families() == std::vector<std::string> { "DejaVu Sans Mono", "Around Only" });

    SUBCASE ("the face for bold or italic, from the show's own folder first")
    {
        const auto regular = fonts.choose ("DejaVu Sans Mono", false, false);
        REQUIRE (regular.has_value());
        CHECK (juce::File (regular->file) == show.getChildFile ("fonts/DejaVuSansMono.ttf"));
        CHECK_FALSE (regular->embolden);
        CHECK_FALSE (regular->slant);

        const auto bold = fonts.choose ("DejaVu Sans Mono", true, false);
        REQUIRE (bold.has_value());
        CHECK (juce::File (bold->file) == show.getChildFile ("fonts/DejaVuSansMono-Bold.ttf"));

        //  The own folder carries the family without an italic: Go.dot slants
        //  its regular, rather than mixing in the folder around's oblique.
        const auto italic = fonts.choose ("DejaVu Sans Mono", false, true);
        REQUIRE (italic.has_value());
        CHECK (juce::File (italic->file) == show.getChildFile ("fonts/DejaVuSansMono.ttf"));
        CHECK (italic->slant);

        //  A family only the folder around carries is found there.
        const auto around = fonts.choose ("Around Only", true, false);
        REQUIRE (around.has_value());
        CHECK (juce::File (around->file) == place.file.getChildFile ("fonts/Around.ttf"));
        CHECK (around->embolden);

        CHECK_FALSE (fonts.choose ("Arial", false, false).has_value());
        CHECK_FALSE (fonts.choose ("", false, false).has_value());
    }

    SUBCASE ("the listing: a family a line, its styles, where it comes from")
    {
        const auto listing = fonts.listing();
        //  Files read by name: "-Bold.ttf" before ".ttf".
        CHECK (listing == "DejaVu Sans Mono\tBold, Book, Oblique\t\t\nAround Only\tRegular\taround\t\n");
    }
}

TEST_CASE ("show fonts: a family matched by the name Windows groups four styles under (§62, AKZ)")
{
    Folder place;
    const auto show = place.file.getChildFile ("Show");
    auto semibold = plain ("Stage Sans Semibold", "Regular", 600);
    semibold.names.push_back (windows (16, "Stage Sans"));
    semibold.names.push_back (windows (17, "Semibold"));
    write (show.getChildFile ("fonts/StageSans-Semibold.ttf"), semibold.bytes());
    write (show.getChildFile ("fonts/StageSans-Regular.ttf"), plain ("Stage Sans", "Regular").bytes());

    const auto fonts = video::ShowFonts::scan (show);
    const auto byFamily = fonts.choose ("Stage Sans", false, false);
    REQUIRE (byFamily.has_value());
    CHECK (juce::File (byFamily->file).getFileName() == "StageSans-Regular.ttf");

    const auto byLegacy = fonts.choose ("Stage Sans Semibold", false, false);
    REQUIRE (byLegacy.has_value());
    CHECK (juce::File (byLegacy->file).getFileName() == "StageSans-Semibold.ttf");
    CHECK (fonts.carries ("Stage Sans Semibold"));
}

TEST_CASE ("font shelf: a file added, a restricted one asked about, a family carried and removed, each said (§62, ALA, ALB)")
{
    Folder place;
    const auto show = place.file.getChildFile ("Show");
    show.createDirectory();

    std::vector<std::string> installed { dejaVu ("DejaVuSansMono.ttf").getFullPathName().toStdString(),
                                         dejaVu ("DejaVuSansMono-Bold.ttf").getFullPathName().toStdString() };
    video::FontShelf shelf { show, [&installed] (const std::string& family)
    {
        return family == "DejaVu Sans Mono" ? installed : std::vector<std::string> {};
    } };

    CHECK (shelf.current()->faces().empty());
    CHECK (shelf.jobText().empty());
    const auto before = shelf.changes();

    SUBCASE ("a file added: copied in, the folder read again, the job said")
    {
        shelf.add (dejaVu ("DejaVuSansMono-Oblique.ttf").getFullPathName().toStdString(), false);
        shelf.waitIdle();
        const auto job = fieldsOf (shelf.jobText());
        CHECK (job[0] == "1");
        CHECK (job[1] == "done");
        CHECK (job[2] == "add");
        CHECK (job[4] == "carried");
        CHECK (job[5] == "DejaVuSansMono-Oblique.ttf");
        CHECK (show.getChildFile ("fonts/DejaVuSansMono-Oblique.ttf").existsAsFile());
        CHECK (shelf.current()->carries ("DejaVu Sans Mono"));
        CHECK (shelf.changes() > before);

        //  The same file again: nothing copied, carried all the same.
        shelf.add (dejaVu ("DejaVuSansMono-Oblique.ttf").getFullPathName().toStdString(), false);
        shelf.waitIdle();
        CHECK (fieldsOf (shelf.jobText())[4] == "carried");
    }

    SUBCASE ("what is not a font, or not there, is said and nothing is copied")
    {
        const auto text = place.file.getChildFile ("notes.ttf");
        text.replaceWithText ("words");
        shelf.add (text.getFullPathName().toStdString(), false);
        shelf.waitIdle();
        auto job = fieldsOf (shelf.jobText());
        CHECK (job[4] == "not-a-font");
        CHECK (job[5] == "notes.ttf: too short to be a font file");

        shelf.add (place.file.getChildFile ("gone.ttf").getFullPathName().toStdString(), false);
        shelf.waitIdle();
        CHECK (fieldsOf (shelf.jobText())[4] == "not-a-font");
        CHECK_FALSE (show.getChildFile ("fonts").exists());
    }

    SUBCASE ("a restricted licence asked about, then carried when asked again")
    {
        auto licensed = plain ("Licensed Face", "Regular");
        licensed.embedding = 0x0002;
        const auto file = place.file.getChildFile ("Licensed.ttf");
        write (file, licensed.bytes());

        shelf.add (file.getFullPathName().toStdString(), false);
        shelf.waitIdle();
        auto job = fieldsOf (shelf.jobText());
        CHECK (job[4] == "restricted");
        CHECK (job[5] == "Licensed Face Regular");
        CHECK_FALSE (show.getChildFile ("fonts/Licensed.ttf").exists());

        shelf.add (file.getFullPathName().toStdString(), true);
        shelf.waitIdle();
        CHECK (fieldsOf (shelf.jobText())[4] == "carried");
        CHECK (show.getChildFile ("fonts/Licensed.ttf").existsAsFile());
        CHECK (shelf.current()->listing().find ("Licensed Face\tRegular\t\trestricted") != std::string::npos);
    }

    SUBCASE ("an installed family carried, every face; then removed")
    {
        shelf.carry ("DejaVu Sans Mono", false);
        shelf.waitIdle();
        auto job = fieldsOf (shelf.jobText());
        CHECK (job[2] == "carry");
        CHECK (job[4] == "carried");
        CHECK (job[5] == "DejaVuSansMono.ttf, DejaVuSansMono-Bold.ttf");
        CHECK (shelf.current()->faces().size() == 2);

        shelf.carry ("Not Here", false);
        shelf.waitIdle();
        CHECK (fieldsOf (shelf.jobText())[4] == "not-installed");

        shelf.remove ("dejavu sans mono");
        shelf.waitIdle();
        job = fieldsOf (shelf.jobText());
        CHECK (job[4] == "removed");
        CHECK (job[5] == "DejaVuSansMono-Bold.ttf, DejaVuSansMono.ttf");
        CHECK (shelf.current()->faces().empty());
        CHECK_FALSE (show.getChildFile ("fonts/DejaVuSansMono.ttf").exists());

        shelf.remove ("DejaVu Sans Mono");
        shelf.waitIdle();
        CHECK (fieldsOf (shelf.jobText())[4] == "not-carried");
    }

    SUBCASE ("a family in the folder around is the show's, not this performance's to remove")
    {
        copyIn ("DejaVuSansMono.ttf", place.file.getChildFile ("fonts"));
        shelf.rescan();
        shelf.waitIdle();
        CHECK (shelf.current()->carries ("DejaVu Sans Mono"));

        shelf.remove ("DejaVu Sans Mono");
        shelf.waitIdle();
        CHECK (fieldsOf (shelf.jobText())[4] == "not-carried");
        CHECK (place.file.getChildFile ("fonts/DejaVuSansMono.ttf").existsAsFile());
    }
}

TEST_CASE ("font shelf: a show with no folder yet has nowhere to put a font, and an installed family needs a way to be found (§62, ALA)")
{
    Folder place;
    video::FontShelf unsaved { place.file.getChildFile ("never saved") };
    unsaved.add (dejaVu ("DejaVuSansMono.ttf").getFullPathName().toStdString(), false);
    unsaved.waitIdle();
    CHECK (fieldsOf (unsaved.jobText())[4] == "no-folder");

    const auto show = place.file.getChildFile ("Show");
    show.createDirectory();
    video::FontShelf blind { show };
    blind.carry ("DejaVu Sans Mono", false);
    blind.waitIdle();
    CHECK (fieldsOf (blind.jobText())[4] == "not-installed");
}

TEST_CASE ("font commands: fonts.add, .carry and .remove ask the shelf, this machine's alone; an empty name refused; with no shelf, taken (§62, ALA)")
{
    Folder place;
    const auto show = place.file.getChildFile ("Show");
    show.createDirectory();
    video::FontShelf shelf { show };

    CommandRegistry registry;
    video::registerFontCommands (registry, &shelf);

    for (const auto* name : { "fonts.add", "fonts.carry", "fonts.remove" })
    {
        const auto* command = registry.find (name);
        REQUIRE_MESSAGE (command != nullptr, name);
        CHECK (command->machineLocal);
        CHECK_FALSE (command->mutates);
    }

    CommandContext context;
    const auto* add = registry.find ("fonts.add");
    CHECK (add->handler (context, { osc::Value::string ("") }).reason == "bad-value");

    const auto path = dejaVu ("DejaVuSansMono.ttf").getFullPathName().toStdString();
    CHECK (add->handler (context, { osc::Value::string (path) }).applied);
    shelf.waitIdle();
    CHECK (shelf.current()->carries ("DejaVu Sans Mono"));

    CHECK (registry.find ("fonts.remove")->handler (context, { osc::Value::string ("DejaVu Sans Mono") }).applied);
    shelf.waitIdle();
    CHECK_FALSE (shelf.current()->carries ("DejaVu Sans Mono"));

    CommandRegistry replay;
    video::registerFontCommands (replay, nullptr);
    CHECK (replay.find ("fonts.add")->handler (context, { osc::Value::string (path), osc::Value::boolean (true) }).applied);
}

TEST_CASE ("save as: a show's fonts/ goes with it, whole (§62, AKX)")
{
    Folder place;
    const auto source = place.file.getChildFile ("Show");
    doc::ShowDocument document;
    REQUIRE (doc::Bundle::save (source, document).ok);
    copyIn ("DejaVuSansMono.ttf", source.getChildFile ("fonts"));
    copyIn ("DejaVuSansMono-Bold.ttf", source.getChildFile ("fonts"));

    const auto copy = place.file.getChildFile ("Show copy");
    const auto result = doc::Bundle::saveCopy (copy, document, source);
    REQUIRE (result.ok);
    CHECK (copy.getChildFile ("fonts/DejaVuSansMono.ttf").hasIdenticalContentTo (source.getChildFile ("fonts/DejaVuSansMono.ttf")));
    CHECK (copy.getChildFile ("fonts/DejaVuSansMono-Bold.ttf").existsAsFile());
    CHECK (video::ShowFonts::scan (copy).carries ("DejaVu Sans Mono"));
}

namespace
{
    /*  AN INSTALLED FAMILY GO.DOT CAN READ: the first this machine lists whose
        files the system names and whose faces Go.dot's reader takes - a Mac
        may still hold a family in an old .dfont, which is no OpenType file. */
    struct Readable
    {
        std::string family;
        std::vector<video::fontfile::Face> faces;
    };

    Readable readableInstalledFamily()
    {
        namespace ff = video::fontfile;
        for (const auto& name : juce::Font::findAllTypefaceNames())
        {
            Readable found { name.toStdString(), {} };
            for (const auto& path : video::installed::filesOfFamily (found.family))
            {
                juce::MemoryBlock data;
                if (! juce::File (juce::String::fromUTF8 (path.c_str())).loadFileAsData (data))
                    continue;
                for (const auto& face : ff::read (static_cast<const std::uint8_t*> (data.getData()), data.getSize()).faces)
                    if (ff::sameFamily (face.family, found.family) || ff::sameFamily (face.legacyFamily, found.family))
                        found.faces.push_back (face);
            }
            if (! found.faces.empty())
                return found;
        }
        return {};
    }
}

TEST_CASE ("installed fonts: an installed family's files found on this machine, and a face found again by its PostScript name (§62, ALA, ALI)")
{
    namespace ff = video::fontfile;
    const auto families = juce::Font::findAllTypefaceNames();
    REQUIRE_FALSE (families.isEmpty());

    //  A family whose files the system names and which hold faces of it: every system has one.
    const auto [family, faces] = readableInstalledFamily();
    REQUIRE_MESSAGE (! faces.empty(), "no installed family's files were found and read");
    MESSAGE ("the family looked up: " << family);

    //  And a face of it found again by the name QLab would know it by.
    const auto& one = faces.front();
    REQUIRE_FALSE (one.postScript.empty());
    const auto found = video::installed::byPostScript (one.postScript);
    REQUIRE_MESSAGE (found.has_value(), one.postScript);
    CHECK (found->family == one.family);
    CHECK (found->style == one.style);
    CHECK (found->bold == one.bold);
    CHECK (found->italic == one.italic);
    CHECK (juce::File (juce::String::fromUTF8 (found->file.c_str())).existsAsFile());

    CHECK_FALSE (video::installed::byPostScript ("NoSuchFace-NowhereAtAll").has_value());
    CHECK_FALSE (video::installed::byPostScript ("").has_value());
    CHECK (video::installed::filesOfFamily ("No Such Family Anywhere At All").empty());
    CHECK (video::installed::filesOfFamily ("").empty());

    //  Where one of each system's usual families is there, its bold is found by name too.
   #if JUCE_WINDOWS
    const std::pair<const char*, const char*> usual[] { { "Arial", "Arial-BoldMT" }, { "Segoe UI", "SegoeUI-Bold" } };
   #elif JUCE_MAC
    const std::pair<const char*, const char*> usual[] { { "Helvetica", "Helvetica-Bold" }, { "Arial", "Arial-BoldMT" } };
   #else
    const std::pair<const char*, const char*> usual[] { { "DejaVu Sans", "DejaVuSans-Bold" }, { "Liberation Sans", "LiberationSans-Bold" } };
   #endif
    for (const auto& [name, bold] : usual)
        if (families.contains (name))
        {
            CHECK_FALSE (video::installed::filesOfFamily (name).empty());
            const auto face = video::installed::byPostScript (bold);
            REQUIRE_MESSAGE (face.has_value(), bold);
            CHECK (ff::sameFamily (face->family, name));
            CHECK (face->bold);
        }
}

TEST_CASE ("font shelf: an installed family carried into a show, found through the system, drawn from the show's copy after (§62, ALA, AKW)")
{
    Folder place;
    const auto show = place.file.getChildFile ("Show");
    show.createDirectory();

    const auto family = readableInstalledFamily().family;
    REQUIRE_FALSE (family.empty());

    video::FontShelf shelf { show, video::installed::filesOfFamily };
    shelf.carry (family, false);
    shelf.waitIdle();
    const auto job = fieldsOf (shelf.jobText());
    MESSAGE ("carried " << family << ": " << shelf.jobText());

    //  A system font may forbid carrying: then it is asked about, and nothing copied.
    REQUIRE_MESSAGE ((job[4] == "carried" || job[4] == "restricted"), shelf.jobText());
    if (job[4] == "carried")
    {
        CHECK (shelf.current()->carries (family));
        const auto choice = shelf.current()->choose (family, false, false);
        REQUIRE (choice.has_value());
        CHECK (juce::File (juce::String::fromUTF8 (choice->file.c_str())).isAChildOf (show.getChildFile ("fonts")));
    }
    else
    {
        CHECK_FALSE (show.getChildFile ("fonts").exists());
    }
}
