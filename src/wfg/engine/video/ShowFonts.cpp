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

#include <wfg/engine/video/ShowFonts.h>

#include <wfg/engine/command/CommandRegistry.h>

#include <algorithm>

namespace wfg::video
{
    namespace
    {
        //  A font bigger than this is not a font anybody titles a show with.
        constexpr juce::int64 fontFileMost = 64 * 1024 * 1024;

        /** The faces of one file, or why it does not read. */
        fontfile::Reading readFontFile (const juce::File& file)
        {
            if (file.getSize() > fontFileMost)
                return { {}, "larger than any font" };

            juce::MemoryBlock data;
            if (! file.loadFileAsData (data))
                return { {}, "could not be read" };

            return fontfile::read (static_cast<const std::uint8_t*> (data.getData()), data.getSize());
        }

        /** The font files directly in `folder`, by name, so a reading is the same on every machine. */
        juce::Array<juce::File> fontFilesIn (const juce::File& folder)
        {
            juce::Array<juce::File> files;
            if (! folder.isDirectory())
                return files;

            for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
                if (fontfile::isFontFileName (file.getFileName().toStdString()))
                    files.add (file);

            files.sort();
            return files;
        }

        bool ofFamily (const fontfile::Face& face, std::string_view family)
        {
            return fontfile::sameFamily (face.family, family) || fontfile::sameFamily (face.legacyFamily, family);
        }

        std::string plain (std::string text)
        {
            std::replace (text.begin(), text.end(), '\t', ' ');
            std::replace (text.begin(), text.end(), '\n', ' ');
            std::replace (text.begin(), text.end(), '\r', ' ');
            return text;
        }

        std::string joined (const std::vector<std::string>& parts, const char* between)
        {
            std::string out;
            for (const auto& part : parts)
                out += (out.empty() ? "" : between) + part;
            return out;
        }
    }

    juce::File fontsFolderOf (const juce::File& showFolder)
    {
        return showFolder.getChildFile ("fonts");
    }

    juce::File fontsFolderAround (const juce::File& showFolder)
    {
        return showFolder.getParentDirectory().getChildFile ("fonts");
    }

    //==============================================================================
    ShowFonts ShowFonts::scan (const juce::File& showFolder)
    {
        ShowFonts fonts;

        const auto own = fontsFolderOf (showFolder);
        const auto around = fontsFolderAround (showFolder);

        for (const auto& [folder, isAround] : { std::pair { own, false }, std::pair { around, true } })
        {
            if (isAround && folder == own)
                continue;

            for (const auto& file : fontFilesIn (folder))
            {
                const auto reading = readFontFile (file);
                if (reading.faces.empty())
                    fonts.problems.push_back (file.getFileName().toStdString() + ": " + reading.error);

                for (const auto& face : reading.faces)
                    fonts.all.push_back ({ file.getFullPathName().toStdString(), face, isAround });
            }
        }
        return fonts;
    }

    std::optional<FontChoice> ShowFonts::choose (std::string_view family, bool bold, bool italic) const
    {
        if (family.empty())
            return std::nullopt;

        /*  The typographic family before Windows' four-style one, and the
            show's own folder before the one around: the first of the four
            groups that holds a face is the family. */
        for (const bool legacy : { false, true })
            for (const bool around : { false, true })
            {
                std::vector<const CarriedFace*> group;
                for (const auto& carried : all)
                    if (carried.around == around
                          && fontfile::sameFamily (legacy ? carried.face.legacyFamily : carried.face.family, family))
                        group.push_back (&carried);

                if (group.empty())
                    continue;

                std::vector<fontfile::Face> faces;
                for (const auto* carried : group)
                    faces.push_back (carried->face);

                const auto pick = fontfile::pickFace (faces, bold, italic);
                const auto& chosen = *group[static_cast<std::size_t> (pick.index)];
                return FontChoice { chosen.file, chosen.face.index, pick.embolden, pick.slant };
            }

        return std::nullopt;
    }

    bool ShowFonts::carries (std::string_view family) const
    {
        return ! family.empty()
            && std::any_of (all.begin(), all.end(), [family] (const CarriedFace& c) { return ofFamily (c.face, family); });
    }

    std::vector<std::string> ShowFonts::families() const
    {
        std::vector<std::string> names;
        for (const auto& carried : all)
            if (std::none_of (names.begin(), names.end(),
                              [&carried] (const std::string& n) { return fontfile::sameFamily (n, carried.face.family); }))
                names.push_back (carried.face.family);
        return names;
    }

    std::string ShowFonts::listing() const
    {
        std::string text;
        for (const auto& family : families())
        {
            std::vector<std::string> styles;
            bool around = true;
            bool restricted = false;
            for (const auto& carried : all)
                if (fontfile::sameFamily (carried.face.family, family))
                {
                    if (std::find (styles.begin(), styles.end(), carried.face.style) == styles.end())
                        styles.push_back (carried.face.style);
                    around = around && carried.around;
                    restricted = restricted || carried.face.restricted();
                }

            text += plain (family) + "\t" + plain (joined (styles, ", ")) + "\t" + (around ? "around" : "")
                  + "\t" + (restricted ? "restricted" : "") + "\n";
        }
        return text;
    }

    //==============================================================================
    FontShelf::FontShelf (juce::File showFolder, InstalledFiles installedFiles)
        : show (std::move (showFolder)),
          installed (std::move (installedFiles)),
          fonts (std::make_shared<const ShowFonts> (ShowFonts::scan (show)))
    {
        changeCount.store (1, std::memory_order_release);
        worker = std::thread ([this] { run(); });
    }

    FontShelf::~FontShelf()
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            stopping = true;
        }
        wake.notify_all();
        worker.join();
    }

    std::shared_ptr<const ShowFonts> FontShelf::current() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return fonts;
    }

    std::string FontShelf::jobText() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return lastJob;
    }

    void FontShelf::add (std::string path, bool restrictedToo)
    {
        const std::lock_guard<std::mutex> lock { guard };
        jobs.push_back ({ "add", std::move (path), restrictedToo });
        wake.notify_all();
    }

    void FontShelf::carry (std::string family, bool restrictedToo)
    {
        const std::lock_guard<std::mutex> lock { guard };
        jobs.push_back ({ "carry", std::move (family), restrictedToo });
        wake.notify_all();
    }

    void FontShelf::remove (std::string family)
    {
        const std::lock_guard<std::mutex> lock { guard };
        jobs.push_back ({ "remove", std::move (family), false });
        wake.notify_all();
    }

    void FontShelf::rescan()
    {
        const std::lock_guard<std::mutex> lock { guard };
        jobs.push_back ({ "rescan", {}, false });
        wake.notify_all();
    }

    void FontShelf::waitIdle()
    {
        std::unique_lock<std::mutex> lock { guard };
        quiet.wait (lock, [this] { return jobs.empty() && ! busy; });
    }

    void FontShelf::run()
    {
        for (;;)
        {
            Job job;
            std::uint32_t number = 0;
            {
                std::unique_lock<std::mutex> lock { guard };
                wake.wait (lock, [this] { return stopping || ! jobs.empty(); });
                if (stopping)
                    return;
                job = std::move (jobs.front());
                jobs.pop_front();
                busy = true;
                number = job.verb == "rescan" ? 0 : ++jobsAsked;
            }

            if (job.verb == "rescan")
                reread();
            else
                doJob (job, number);

            {
                const std::lock_guard<std::mutex> lock { guard };
                busy = false;
            }
            quiet.notify_all();
        }
    }

    void FontShelf::say (std::uint32_t number, const char* state, const Job& job, const std::string& result,
                         const std::string& detail)
    {
        const auto text = std::to_string (number) + "\t" + state + "\t" + job.verb + "\t" + plain (job.subject) + "\t"
                        + result + "\t" + plain (detail);
        {
            const std::lock_guard<std::mutex> lock { guard };
            lastJob = text;
        }
        changeCount.fetch_add (1, std::memory_order_acq_rel);
    }

    void FontShelf::reread()
    {
        auto next = std::make_shared<const ShowFonts> (ShowFonts::scan (show));
        {
            const std::lock_guard<std::mutex> lock { guard };
            fonts = std::move (next);
        }
        changeCount.fetch_add (1, std::memory_order_acq_rel);
    }

    void FontShelf::doJob (const Job& job, std::uint32_t number)
    {
        say (number, "working", job, {}, {});

        const auto done = [&] (const std::string& result, const std::string& detail)
        {
            reread();
            say (number, "done", job, result, detail);
        };

        if (! show.isDirectory())
            return done ("no-folder", {});

        const auto folder = fontsFolderOf (show);

        if (job.verb == "remove")
        {
            std::vector<std::string> gone;
            for (const auto& file : fontFilesIn (folder))
            {
                const auto reading = readFontFile (file);
                if (std::any_of (reading.faces.begin(), reading.faces.end(),
                                 [&job] (const fontfile::Face& face) { return ofFamily (face, job.subject); }))
                {
                    if (! file.deleteFile())
                        return done ("failed", file.getFileName().toStdString() + " could not be deleted");
                    gone.push_back (file.getFileName().toStdString());
                }
            }
            return done (gone.empty() ? "not-carried" : "removed", joined (gone, ", "));
        }

        //  add or carry: the files to copy, read first, so nothing is copied of a family refused.
        std::vector<juce::File> sources;
        if (job.verb == "add")
            sources.push_back (juce::File (juce::String::fromUTF8 (job.subject.c_str())));
        else if (installed)
            for (const auto& path : installed (job.subject))
            {
                /*  OpenType only: a system may keep a family's older faces as Type 1
                    (.pfb) or Macintosh resources (.dfont), which no other system reads. */
                const juce::File file (juce::String::fromUTF8 (path.c_str()));
                if (fontfile::isFontFileName (file.getFileName().toStdString()))
                    sources.push_back (file);
            }

        if (sources.empty())
            return done ("not-installed", {});

        std::vector<std::string> restricted;
        for (const auto& source : sources)
        {
            if (! source.existsAsFile())
                return done (job.verb == "add" ? "not-a-font" : "not-installed", source.getFullPathName().toStdString());

            const auto reading = readFontFile (source);
            if (reading.faces.empty())
                return done ("not-a-font", source.getFileName().toStdString() + ": " + reading.error);

            for (const auto& face : reading.faces)
                if (face.restricted())
                    restricted.push_back (face.fullName.empty() ? face.family + " " + face.style : face.fullName);
        }

        if (! restricted.empty() && ! job.restrictedToo)
            return done ("restricted", joined (restricted, ", "));

        if (! folder.isDirectory() && ! folder.createDirectory())
            return done ("failed", "the fonts folder could not be made");

        std::vector<std::string> copied;
        for (const auto& source : sources)
        {
            const auto target = folder.getChildFile (source.getFileName());
            if (target == source || (target.existsAsFile() && target.hasIdenticalContentTo (source)))
            {
                copied.push_back (target.getFileName().toStdString());
                continue;
            }
            if (! source.copyFileTo (target))
                return done ("failed", source.getFileName().toStdString() + " could not be copied into the show");
            copied.push_back (target.getFileName().toStdString());
        }
        return done ("carried", joined (copied, ", "));
    }

    //==============================================================================
    void registerFontCommands (CommandRegistry& registry, FontShelf* shelf)
    {
        registry.add (machineLocal ({ "fonts.add",
                        "Copies a font file (.ttf, .otf, .ttc) into the show folder's fonts/ (namespace draft 62):"
                        " its families then draw the show's words on every machine the show opens on, over"
                        " an installed font of the same name. A face whose licence restricts carrying it is"
                        " refused until asked again with restricted true. /godot/engine/fontsJob says what was done.",
                        { { "path", 's', false }, { "restricted", 'T', true } },
                        false,
                        [shelf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (args[0].getString().empty())
                                return Outcome::rejected ("bad-value");
                            if (shelf != nullptr)
                                shelf->add (args[0].getString(), args.size() > 1 && args[1].isBool() && args[1].getBool());
                            return Outcome::ok (args);
                        } }));

        registry.add (machineLocal ({ "fonts.carry",
                        "Copies every face of a font family installed on this machine into the show folder's"
                        " fonts/ (namespace draft 62), as fonts.add does with a file.",
                        { { "family", 's', false }, { "restricted", 'T', true } },
                        false,
                        [shelf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (args[0].getString().empty())
                                return Outcome::rejected ("bad-value");
                            if (shelf != nullptr)
                                shelf->carry (args[0].getString(), args.size() > 1 && args[1].isBool() && args[1].getBool());
                            return Outcome::ok (args);
                        } }));

        registry.add (machineLocal ({ "fonts.remove",
                        "Deletes the files of a font family from the show folder's fonts/ (namespace draft 62): its"
                        " text cues then draw in a font of that name installed on the machine, if there is one.",
                        { { "family", 's', false } },
                        false,
                        [shelf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (args[0].getString().empty())
                                return Outcome::rejected ("bad-value");
                            if (shelf != nullptr)
                                shelf->remove (args[0].getString());
                            return Outcome::ok (args);
                        } }));
    }
}
