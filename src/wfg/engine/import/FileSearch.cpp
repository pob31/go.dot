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

#include <wfg/engine/import/FileSearch.h>

#include <wfg/engine/import/ImportCommon.h>

namespace wfg::import
{
    namespace
    {
        //  The folders of a path, last first, each as the index keys a name.
        std::vector<juce::String> foldersOf (const juce::String& path)
        {
            juce::StringArray parts;
            parts.addTokens (path.replaceCharacter ('\\', '/'), "/", {});
            parts.removeEmptyStrings();

            std::vector<juce::String> out;

            for (int at = parts.size() - 2; at >= 0; --at)
                out.push_back (FolderIndex::keyOf (parts[at]));

            return out;
        }
    }

    juce::String FolderIndex::keyOf (const juce::String& name)
    {
        return folded (name).toLowerCase();
    }

    FolderIndex::FolderIndex (const juce::File& folder, const std::atomic<bool>* stop)
        : root (folder)
    {
        if (! folder.isDirectory())
            return;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            if (stop != nullptr && stop->load())
                break;

            const auto file = entry.getFile();

            if (file.getFullPathName().contains ("Backup") || file.getFileExtension() == ".asd")
                continue;

            byName.insert ({ keyOf (file.getFileName()), file });
            ++count;
        }
    }

    juce::File FolderIndex::find (const std::string& name, const std::string& remembered, std::int64_t size) const
    {
        const auto wanted = foldersOf (juce::String::fromUTF8 (remembered.c_str()));
        juce::File best;
        std::size_t bestScore = 0;
        auto any = false;

        for (const auto& spelled : spellings (name))
        {
            const auto [from, to] = byName.equal_range (keyOf (juce::String::fromUTF8 (spelled.c_str())));

            for (auto it = from; it != to; ++it)
            {
                const auto& file = it->second;

                if (size > 0 && file.getSize() != size)
                    continue;

                //  How many of its folders, from the nearest up, the remembered path shares.
                const auto has = foldersOf (file.getRelativePathFrom (root));
                std::size_t score = 0;

                while (score < wanted.size() && score < has.size() && wanted[score] == has[score])
                    ++score;

                if (! any || score > bestScore)
                {
                    best = file;
                    bestScore = score;
                    any = true;
                }
            }
        }

        return best;
    }

    juce::File findInFolders (const std::vector<FolderIndex>& folders, const std::string& name,
                              const std::string& remembered, std::int64_t size)
    {
        for (const auto& folder : folders)
            if (const auto found = folder.find (name, remembered, size); found != juce::File())
                return found;

        return {};
    }
}
