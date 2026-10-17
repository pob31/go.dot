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
    THE FONTS A SHOW CARRIES (namespace draft §62, AKV-ALC).

    A show folder may hold `fonts/`, beside `media/` (AKX): font files put there
    on request, a family at a time - an installed family carried, or a file
    added (AKV) - never copied unasked. A performance finds its show's fonts as
    it finds its sounds: in its own folder first, then in the folder around it
    (§25), a family in its own shadowing one of the same name around.

    `ShowFonts` is what the folders held when they were read: every face, named
    from its own file (FontFile.h, AKY), and `choose`, which turns a text cue's
    family, bold and italic into the one face file that draws it. The show's
    copy wins over an installed font of the same name (AKW), because the
    renderer is handed the file itself, not the name (AKZ).

    `FontShelf` is the folder's keeper on one machine: it reads the folders,
    carries, adds and removes fonts on a thread of its own - never the tick's,
    which runs GO - and says what it did. The three commands (ALA) ask it.
*/

#include <wfg/engine/video/FontFile.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wfg { class CommandRegistry; }

namespace wfg::video
{
    /** `<show>/fonts`, and the folder around's: `<show>/../fonts` (AKX). */
    juce::File fontsFolderOf (const juce::File& showFolder);
    juce::File fontsFolderAround (const juce::File& showFolder);

    struct CarriedFace
    {
        std::string file;           ///< the font file's absolute path
        fontfile::Face face;
        bool around = false;        ///< in the folder around the show's, not its own
    };

    /** The face file a text cue draws with, and what Go.dot adds to it (AKZ). */
    struct FontChoice
    {
        std::string file;
        int face = 0;               ///< within a collection
        bool embolden = false;
        bool slant = false;
    };

    class ShowFonts
    {
    public:
        /** What `<show>/fonts` and the folder around's hold now. */
        static ShowFonts scan (const juce::File& showFolder);

        /*  The face that draws `family` bold or italic, when the show carries
            the family - matched by its typographic family first, then by the
            family Windows groups its styles under; its own folder's faces before
            the folder around's. Nothing when the show does not carry it: the
            renderer then asks the system by name, as before. */
        std::optional<FontChoice> choose (std::string_view family, bool bold, bool italic) const;

        bool carries (std::string_view family) const;

        /** The families carried, own first, each once. */
        std::vector<std::string> families() const;

        /*  `/godot/engine/fonts`: a line per family, its name, a tab, its styles
            separated by ", ", a tab, "around" when it comes from the folder
            around, a tab, "restricted" when a face's licence restricts it. */
        std::string listing() const;

        const std::vector<CarriedFace>& faces() const noexcept { return all; }

        /** "<file>: <why>" for each file in the folders that did not read. */
        const std::vector<std::string>& unreadable() const noexcept { return problems; }

    private:
        std::vector<CarriedFace> all;
        std::vector<std::string> problems;
    };

    /*  THE FONT FOLDER'S KEEPER on this machine (ALA). Its jobs run one at a
        time on its own thread; after each it reads the folders again, counts a
        change, and says what it did in `jobText`:

            <job number> \t working|done \t add|carry|remove \t <subject> \t <result> \t <detail>

        <result>: carried, removed, restricted (<detail> the faces, by name),
        not-a-font, not-installed, not-carried, no-folder, failed (<detail> the
        system's words). `restricted` is a question for the operator: the same
        job asked again with `restrictedToo` carries it (ALB). */
    class FontShelf
    {
    public:
        /*  Where an installed family's files are, on this machine - every face
            of it (QF.3). Without it, carrying an installed family answers
            not-installed. */
        using InstalledFiles = std::function<std::vector<std::string> (const std::string& family)>;

        explicit FontShelf (juce::File showFolder, InstalledFiles installed = {});
        ~FontShelf();

        FontShelf (const FontShelf&) = delete;
        FontShelf& operator= (const FontShelf&) = delete;

        /** What the folders held when last read; never null. */
        std::shared_ptr<const ShowFonts> current() const;

        /** Counts every reading of the folders and every job's end: a reader compares it to the last it saw. */
        std::uint32_t changes() const noexcept { return changeCount.load (std::memory_order_acquire); }

        std::string jobText() const;

        void add (std::string path, bool restrictedToo);
        void carry (std::string family, bool restrictedToo);
        void remove (std::string family);

        /** The folders read again, as a job: after a show is saved somewhere new, or by hand. */
        void rescan();

        /** Waits until every job asked so far is done - for a test, or a verb that exits after one. */
        void waitIdle();

    private:
        struct Job
        {
            std::string verb, subject;
            bool restrictedToo = false;
        };

        void run();
        void doJob (const Job&, std::uint32_t number);
        void say (std::uint32_t number, const char* state, const Job&, const std::string& result,
                  const std::string& detail);
        void reread();

        const juce::File show;
        const InstalledFiles installed;

        mutable std::mutex guard;
        std::condition_variable wake, quiet;
        std::deque<Job> jobs;
        bool busy = false;
        bool stopping = false;
        std::uint32_t jobsAsked = 0;
        std::shared_ptr<const ShowFonts> fonts;
        std::string lastJob;
        std::atomic<std::uint32_t> changeCount { 0 };
        std::thread worker;
    };

    /*  `fonts.add`, `fonts.carry` and `fonts.remove` (ALA): this machine's files,
        so never sent to a backup and never a step of undo - as
        `media.cleanCache`. With no shelf - a replay, a listing - taken and
        ignored. */
    void registerFontCommands (CommandRegistry& registry, FontShelf* shelf);
}
