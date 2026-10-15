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

#include <wfg/engine/process/ScriptEditing.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/process/ScriptText.h>

#include <juce_core/juce_core.h>


#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <shellapi.h>
#endif

namespace wfg::process::scriptedit
{
    namespace
    {
        juce::File fileOf (const std::string& path)
        {
            return juce::File (juce::String::fromUTF8 (path.c_str()));
        }

        std::int64_t modifiedOf (const std::string& file)
        {
            const auto f = fileOf (file);
            return f.existsAsFile() ? f.getLastModificationTime().toMilliseconds() : 0;
        }

        std::string read (const std::string& file)
        {
            juce::MemoryBlock bytes;
            if (! fileOf (file).loadFileAsData (bytes))
                return {};
            return { static_cast<const char*> (bytes.getData()), bytes.getSize() };
        }
    }

    std::string openInTextEditor (const std::string& path)
    {
       #if defined (_WIN32)
        const auto wide = juce::String::fromUTF8 (path.data(), static_cast<int> (path.size())).toWideCharPointer();

        //  "edit" first: a .js file's "open" would RUN it (Windows Script Host).
        auto done = reinterpret_cast<std::intptr_t> (::ShellExecuteW (nullptr, L"edit", wide, nullptr, nullptr, SW_SHOWNORMAL));
        if (done > 32)
            return {};

        if (done == SE_ERR_NOASSOC)
        {
            done = reinterpret_cast<std::intptr_t> (::ShellExecuteW (nullptr, L"open", wide, nullptr, nullptr, SW_SHOWNORMAL));
            if (done > 32)
                return {};
        }
        return "no program on this machine edits " + path;
       #else
        juce::ChildProcess child;
       #if defined (__APPLE__)
        const juce::StringArray command { "open", "-t", juce::String::fromUTF8 (path.data(), static_cast<int> (path.size())) };
       #else
        const juce::StringArray command { "xdg-open", juce::String::fromUTF8 (path.data(), static_cast<int> (path.size())) };
       #endif
        if (! child.start (command, 0))
            return "no program on this machine edits " + path;
        return {};
       #endif
    }

    std::string Watch::fileFor (const std::string& cueId, const std::string& language) const
    {
        return fileOf (files).getChildFile (juce::String (cueId) + (isLua (language) ? ".lua" : ".js"))
                             .getFullPathName().toStdString();
    }

    std::string Watch::open (const std::string& cueId, const std::string& language, const std::string& text)
    {
        const auto file = fileFor (cueId, language);

        (void) fileOf (files).createDirectory();
        //  JUCE deletes a file replaced with no bytes: an empty script is a file made empty.
        const auto written = text.empty() ? (fileOf (file).deleteFile() && fileOf (file).create().wasOk())
                                          : fileOf (file).replaceWithData (text.data(), text.size());
        if (! written)
            return "the script could not be written to " + file;

        out[cueId] = Out { file, modifiedOf (file), text };

        if (! opening)
            return {};
        return opening (file);
    }

    std::vector<std::pair<std::string, std::string>> Watch::saved()
    {
        std::vector<std::pair<std::string, std::string>> changed;

        for (auto& [cue, one] : out)
        {
            const auto now = modifiedOf (one.file);
            if (now == 0 || now == one.modified)
                continue;

            one.modified = now;
            auto text = read (one.file);
            if (text == one.text)
                continue;

            one.text = text;
            changed.emplace_back (cue, std::move (text));
        }
        return changed;
    }

    void registerCommands (CommandRegistry& registry, const doc::ShowDocument& document, Watch* watch)
    {
        registry.add (machineLocal ({ "script.edit",
                        "Opens a script cue's source in this machine's own editor for its language - edited, never"
                        " run; each save there comes back as the cue's source (namespace draft 66, AMU). Refused"
                        " while the show is locked, and editor-missing where no program edits such a file.",
                        { { "cue", 's', false } },
                        false,
                        [&document, watch] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid() || cue.getType().toString() != "Script")
                                return Outcome::rejected (reason::unknownId);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            if (watch == nullptr)
                                return Outcome::ok (args);

                            const auto base = "/godot/cue/" + cueId + "/";
                            const auto language = document.getAttribute (base + "language").value_or (std::string {});
                            const auto text = document.getAttribute (base + "source").value_or (std::string {});

                            if (const auto problem = watch->open (cueId, language, text); ! problem.empty())
                                return Outcome::rejected ("editor-missing");

                            return Outcome::ok (args);
                        } }));
    }
}
