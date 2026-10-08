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

#include <wfg/engine/import/ImportCommon.h>

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace wfg::import
{
    std::string number (double value, int decimals)
    {
        const auto scale = std::pow (10.0, decimals);
        return osc::formatDouble (std::round (value * scale) / scale);
    }

    std::uint64_t fnv1a (const std::string& text)
    {
        std::uint64_t hash = 14695981039346656037ull;

        for (const auto character : text)
        {
            hash ^= static_cast<unsigned char> (character);
            hash *= 1099511628211ull;
        }

        return hash;
    }

    std::string idFor (const std::string& salt, const std::string& key)
    {
        return doc::Id::encode (fnv1a (salt + key) & 0xFFFFFFFFFFull);
    }

    std::string Identities::of (const std::string& key)
    {
        if (const auto found = given.find (key); found != given.end())
            return found->second;

        auto id = idFor (salt, key);

        for (int again = 2; taken.count (id) != 0; ++again)
            id = idFor (salt, key + "#" + std::to_string (again));

        taken.insert (id);
        given[key] = id;
        return id;
    }

    /*  A combining mark is dropped, and a composed Latin letter (U+00C0 to
        U+017F) becomes the letter it is built on, by the table below -
        Unicode's own decompositions, generated, not typed. The Lazzi sets name
        five of their files decomposed, and the disk holds them composed. */
    juce::String folded (const juce::String& name)
    {
        static constexpr juce::juce_wchar latin[] = {
            0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0xC6, 0x43, 0x45, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x49,
            0xD0, 0x4E, 0x4F, 0x4F, 0x4F, 0x4F, 0x4F, 0xD7, 0xD8, 0x55, 0x55, 0x55, 0x55, 0x59, 0xDE, 0xDF,
            0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0xE6, 0x63, 0x65, 0x65, 0x65, 0x65, 0x69, 0x69, 0x69, 0x69,
            0xF0, 0x6E, 0x6F, 0x6F, 0x6F, 0x6F, 0x6F, 0xF7, 0xF8, 0x75, 0x75, 0x75, 0x75, 0x79, 0xFE, 0x79,
            0x41, 0x61, 0x41, 0x61, 0x41, 0x61, 0x43, 0x63, 0x43, 0x63, 0x43, 0x63, 0x43, 0x63, 0x44, 0x64,
            0x110, 0x111, 0x45, 0x65, 0x45, 0x65, 0x45, 0x65, 0x45, 0x65, 0x45, 0x65, 0x47, 0x67, 0x47, 0x67,
            0x47, 0x67, 0x47, 0x67, 0x48, 0x68, 0x126, 0x127, 0x49, 0x69, 0x49, 0x69, 0x49, 0x69, 0x49, 0x69,
            0x49, 0x131, 0x132, 0x133, 0x4A, 0x6A, 0x4B, 0x6B, 0x138, 0x4C, 0x6C, 0x4C, 0x6C, 0x4C, 0x6C, 0x13F,
            0x140, 0x141, 0x142, 0x4E, 0x6E, 0x4E, 0x6E, 0x4E, 0x6E, 0x149, 0x14A, 0x14B, 0x4F, 0x6F, 0x4F, 0x6F,
            0x4F, 0x6F, 0x152, 0x153, 0x52, 0x72, 0x52, 0x72, 0x52, 0x72, 0x53, 0x73, 0x53, 0x73, 0x53, 0x73,
            0x53, 0x73, 0x54, 0x74, 0x54, 0x74, 0x166, 0x167, 0x55, 0x75, 0x55, 0x75, 0x55, 0x75, 0x55, 0x75,
            0x55, 0x75, 0x55, 0x75, 0x57, 0x77, 0x59, 0x79, 0x59, 0x5A, 0x7A, 0x5A, 0x7A, 0x5A, 0x7A, 0x17F,
        };

        juce::String out;

        for (auto character = name.getCharPointer(); ! character.isEmpty(); ++character)
        {
            const auto code = *character;

            if (code >= 0x300 && code <= 0x36F)
                continue;

            out += code >= 0xC0 && code <= 0x17F ? latin[code - 0xC0] : code;
        }

        return out;
    }

    std::vector<std::string> spellings (const std::string& name)
    {
        std::vector<std::string> out { name };

        if (name.find (':') == std::string::npos)
            return out;

        for (const auto instead : { '/', '_', ' ' })
        {
            auto spelled = name;
            std::replace (spelled.begin(), spelled.end(), ':', instead);
            out.push_back (spelled);
        }

        /*  And dropped, with the space after it, which is what the Lazzi copy
            did: "19_CARGO: S.Berger" became "19_CARGO S.Berger". */
        auto dropped = name;
        dropped.erase (std::remove (dropped.begin(), dropped.end(), ':'), dropped.end());
        out.push_back (dropped);
        return out;
    }

    juce::File findFile (const FileSought& sought, const std::vector<juce::File>& folders)
    {
        /*  BY ITS RELATIVE PATH, a folder at a time, so a `:` in one part is
            tried as its spellings without the drive-letter reading Windows
            would give the whole string. */
        if (! sought.relativePath.empty())
        {
            juce::StringArray parts;
            parts.addTokens (juce::String::fromUTF8 (sought.relativePath.c_str()), "/", {});
            parts.removeEmptyStrings();

            for (const auto& root : folders)
            {
                std::vector<juce::File> here { root };

                for (const auto& part : parts)
                {
                    std::vector<juce::File> next;

                    for (const auto& folder : here)
                        for (const auto& spelled : spellings (part.toStdString()))
                            next.push_back (folder.getChildFile (juce::String::fromUTF8 (spelled.c_str())));

                    here = std::move (next);
                }

                for (const auto& candidate : here)
                    if (candidate.existsAsFile())
                        return candidate;
            }
        }

        if (! sought.absolutePath.empty())
            if (const juce::File absolute { juce::String::fromUTF8 (sought.absolutePath.c_str()) };
                  juce::File::isAbsolutePath (absolute.getFullPathName()) && absolute.existsAsFile())
                return absolute;

        if (folders.empty())
            return {};

        /*  BY NAME AND SIZE, under the first folder - the project the source sits
            in holds its sounds, and a size is what tells two takes of one name
            apart - each name compared with its accents folded. */
        std::set<juce::String> wanted;

        for (const auto& spelled : spellings (sought.name))
            wanted.insert (folded (juce::String::fromUTF8 (spelled.c_str())));

        for (const auto& entry : juce::RangedDirectoryIterator (folders.front(), true, "*", juce::File::findFiles))
        {
            const auto file = entry.getFile();

            if (file.getFullPathName().contains ("Backup") || file.getFileExtension() == ".asd")
                continue;

            if (wanted.count (folded (file.getFileName())) == 0)
                continue;

            if (sought.size <= 0 || file.getSize() == sought.size)
                return file;
        }

        return {};
    }

    int channelsOf (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) };
        return reader != nullptr ? static_cast<int> (reader->numChannels) : 0;
    }

    PlacedFile placeFile (const juce::File& source, const juce::File& mediaFolder, bool copy, MediaBook& book,
                          const std::function<void (const std::string&)>& say)
    {
        PlacedFile placed;
        const auto key = source.getFullPathName();

        if (const auto done = book.copied.find (key); done != book.copied.end())
        {
            const auto there = mediaFolder.getChildFile (juce::String::fromUTF8 (done->second.c_str()));
            placed.name = done->second;
            placed.channels = channelsOf (there.existsAsFile() ? there : source);
            return placed;
        }

        /*  ITS OWN NAME, unless another file already took it. */
        auto name = source.getFileName().toStdString();

        for (int again = 2; book.namesUsed.count (name) != 0; ++again)
            name = source.getFileNameWithoutExtension().toStdString() + " (" + std::to_string (again) + ")"
                     + source.getFileExtension().toStdString();

        book.namesUsed.insert (name);
        book.copied[key] = name;

        const auto destination = mediaFolder.getChildFile (juce::String::fromUTF8 (name.c_str()));

        if (copy && ! (destination.existsAsFile() && destination.getSize() == source.getSize()))
        {
            if (say)
                say ("copying " + name);

            placed.copyFailed = ! source.copyFileTo (destination);
        }

        placed.name = name;
        placed.channels = channelsOf (copy ? destination : source);
        return placed;
    }

    bool holdsAShow (const juce::File& folder)
    {
        return doc::Bundle::manifestFile (folder).existsAsFile() || doc::Bundle::showFile (folder).existsAsFile();
    }

    SavedShow saveShow (doc::ShowDocument& document, const juce::File& folder, const std::string& what)
    {
        SavedShow saved;
        saved.problems = document.validate();

        if (! saved.problems.empty())
        {
            saved.error = "the show built from the " + what + " did not validate: " + saved.problems.front();
            return saved;
        }

        const auto written = doc::Bundle::save (folder, document);

        if (! written.ok)
        {
            saved.error = "the show could not be saved"
                          + (written.problems.empty() ? std::string {} : ": " + written.problems.front());
            return saved;
        }

        saved.ok = true;
        return saved;
    }
}
