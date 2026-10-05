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
    THE IMPORT'S DISK HALF (namespace draft §30, S7): what ui/MediaCopier does
    with one file, on real files in a folder of the test's own. The decisions
    are model::MediaImports' and ClientTests holds them; this is the part that
    reads and writes - the look, the same-bytes test, the copy that never
    lands half written, Keep both's number and Replace - with no thread and no
    message loop, since `doMediaJob` is a plain function on purpose.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/client/model/Media.h>
#include <wfg/client/ui/MediaCopier.h>

#include <juce_core/juce_core.h>

#include <string>

using namespace wfg::client;

namespace
{
    /*  A folder of the test's own, with a show's `media/` in it and a place
        the picked files come from, gone when the case ends. */
    struct Disk
    {
        Disk()
        {
            root.deleteRecursively();
            REQUIRE (sounds.createDirectory().wasOk());
        }

        ~Disk() { root.deleteRecursively(); }

        juce::File pick (const juce::String& name, const juce::String& bytes) const
        {
            const auto file = sounds.getChildFile (name);
            REQUIRE (file.replaceWithText (bytes, false, false, nullptr));
            return file;
        }

        juce::File inShow (const juce::String& name, const juce::String& bytes) const
        {
            REQUIRE (media.createDirectory().wasOk());
            const auto file = media.getChildFile (name);
            REQUIRE (file.replaceWithText (bytes, false, false, nullptr));
            return file;
        }

        /** Every file in `media/`, a hidden part-file included. */
        int everythingInMedia() const
        {
            return media.getNumberOfChildFiles (juce::File::findFiles, "*");
        }

        const juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("godot-media-copier-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt()));
        const juce::File sounds = root.getChildFile ("sounds");
        const juce::File media = root.getChildFile ("show").getChildFile ("media");
    };

    model::MediaJob look (const juce::File& source)
    {
        model::MediaJob job;
        job.source = source.getFullPathName().toStdString();
        return job;
    }

    model::MediaJob copy (const juce::File& source, model::Clash answer = model::Clash::ask, std::string met = {})
    {
        auto job = look (source);
        job.copy = true;
        job.answer = answer;
        job.met = std::move (met);
        return job;
    }

    const auto never = [] { return false; };
}

TEST_CASE ("media copier: a look says what a picked file meets in media/, and reads bytes only to tell the same from another")
{
    Disk disk;

    //  Nothing there yet - not even the folder, which a look makes.
    const auto rain = disk.pick ("Rain.wav", "rain, forty seconds of it");
    auto work = ui::doMediaJob (look (rain), disk.media, never);
    CHECK (work.found == model::Found::free);
    CHECK (work.name == "Rain.wav");
    CHECK (disk.media.isDirectory());
    CHECK (disk.everythingInMedia() == 0);              // a look copies nothing

    /*  THE AUTHOR'S CASE: the same file, left in `media/` by a session that
        was never saved - under another case, as a case-blind disk keeps it.
        Used, under the name the disk has. */
    disk.inShow ("RAIN.WAV", "rain, forty seconds of it");
    work = ui::doMediaJob (look (rain), disk.media, never);
    CHECK (work.found == model::Found::same);
    CHECK (work.name == "RAIN.WAV");

    //  Another file of the name: the same size with other bytes, and another size.
    const auto wind = disk.pick ("Wind.wav", "wind, version two");
    disk.inShow ("Wind.wav", "wind, version one");
    work = ui::doMediaJob (look (wind), disk.media, never);
    CHECK (work.found == model::Found::other);
    CHECK (work.name == "Wind.wav");

    const auto road = disk.pick ("Road.wav", "a longer road than the one in the show");
    disk.inShow ("Road.wav", "a short road");
    CHECK (ui::doMediaJob (look (road), disk.media, never).found == model::Found::other);

    //  Picked out of the show's own folder: that file, whatever it meets.
    work = ui::doMediaJob (look (disk.media.getChildFile ("Wind.wav")), disk.media, never);
    CHECK (work.found == model::Found::inShow);
    CHECK (work.name == "Wind.wav");

    //  Gone since it was picked, and a show with no folder at all.
    CHECK (ui::doMediaJob (look (disk.sounds.getChildFile ("Gone.wav")), disk.media, never).found
           == model::Found::missing);
    CHECK (ui::doMediaJob (look (rain), juce::File(), never).found == model::Found::noFolder);
}

TEST_CASE ("media copier: a copy lands whole under its name, Keep both takes the first free number, and Replace writes over")
{
    Disk disk;

    //  A name past ASCII, the author's own kind.
    const auto choir = disk.pick (juce::String::fromUTF8 ("Danse en Ch\xc5\x93ur + C10.wav"), "the choir");
    auto work = ui::doMediaJob (copy (choir), disk.media, never);
    REQUIRE (work.found == model::Found::copied);
    CHECK (juce::String::fromUTF8 (work.name.c_str()) == choir.getFileName());
    CHECK (disk.media.getChildFile (choir.getFileName()).loadFileAsString() == "the choir");
    CHECK (disk.everythingInMedia() == 1);              // no part-file left beside it

    //  Keep both: beside it, under the first free number - and the next one after that.
    const auto other = disk.pick ("Thunder.wav", "the new thunder");
    disk.inShow ("Thunder.wav", "the old thunder");
    disk.inShow ("thunder 3.WAV", "somebody's third");

    work = ui::doMediaJob (copy (other, model::Clash::keepBoth), disk.media, never);
    REQUIRE (work.found == model::Found::copied);
    CHECK (work.name == "Thunder 2.wav");
    CHECK (disk.media.getChildFile ("Thunder 2.wav").loadFileAsString() == "the new thunder");
    CHECK (disk.media.getChildFile ("Thunder.wav").loadFileAsString() == "the old thunder");

    work = ui::doMediaJob (copy (other, model::Clash::keepBoth), disk.media, never);
    CHECK (work.name == "Thunder 4.wav");               // 2 is taken now, and 3 case-blind

    //  Replace: over the one it met, under that one's name.
    work = ui::doMediaJob (copy (other, model::Clash::replace, "Thunder.wav"), disk.media, never);
    REQUIRE (work.found == model::Found::copied);
    CHECK (work.name == "Thunder.wav");
    CHECK (disk.media.getChildFile ("Thunder.wav").loadFileAsString() == "the new thunder");

    /*  A COPY UNDER ITS OWN NAME LOOKS AGAIN FIRST: two files of one name in
        one drop were both free when they were looked at, and the second must
        not land on the first. */
    const auto first = disk.pick ("Bell.wav", "the first bell");
    REQUIRE (ui::doMediaJob (copy (first), disk.media, never).found == model::Found::copied);

    const auto second = disk.root.getChildFile ("elsewhere").getChildFile ("Bell.wav");
    REQUIRE (second.getParentDirectory().createDirectory().wasOk());
    REQUIRE (second.replaceWithText ("the other bell", false, false, nullptr));

    work = ui::doMediaJob (copy (second), disk.media, never);
    CHECK (work.found == model::Found::other);
    CHECK (disk.media.getChildFile ("Bell.wav").loadFileAsString() == "the first bell");

    //  And the same bytes dropped twice are the one file, used.
    CHECK (ui::doMediaJob (copy (first), disk.media, never).found == model::Found::same);
}

TEST_CASE ("media copier: a copy stopped part of the way leaves nothing behind, and says it did not land")
{
    Disk disk;

    //  Three megabytes, so the copy asks about stopping more than once.
    juce::MemoryBlock bytes (3u << 20u);
    bytes.fillWith (0x5a);
    const auto big = disk.sounds.getChildFile ("Long.wav");
    REQUIRE (big.replaceWithData (bytes.getData(), bytes.getSize()));

    auto asked = 0;
    const auto stopSecond = [&asked] { return ++asked >= 2; };

    const auto work = ui::doMediaJob (copy (big), disk.media, stopSecond);
    CHECK (work.found == model::Found::failed);
    CHECK_FALSE (work.why.empty());
    CHECK_FALSE (disk.media.getChildFile ("Long.wav").exists());
    CHECK (disk.everythingInMedia() == 0);              // the hidden part-file went too
}
