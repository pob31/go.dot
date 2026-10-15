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
    A SCRIPT OPENED IN THIS MACHINE'S OWN EDITOR (namespace draft §66, AMU).

    `script.edit` writes a script cue's source to a file of its own - `<cue>.js`
    or `<cue>.lua`, in the engine's cache - and opens it in the program this
    machine EDITS such a file with; each save there comes back as an edit to the
    cue, as a save in Pd does (ACN). Machine-local, as `process.edit` is: never
    sent to a backup, and no step of undo of its own.

    EDITED, NEVER RUN: on Windows a .js file's "open" is Windows Script Host,
    which would run the script on this machine. So Windows is asked for the
    file's "edit" verb - Notepad for a .js unless somebody chose otherwise -
    and "open" only for a type that has no "edit"; the Mac's default text
    editor (`open -t`); and on Linux what `xdg-open` names for it.
*/

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wfg { class CommandRegistry; }
namespace wfg::doc { class ShowDocument; }

namespace wfg::process::scriptedit
{
    /*  Opens a file to be edited; empty when asked, else why not. */
    using Opener = std::function<std::string (const std::string& path)>;

    /*  This machine's editor, as above. */
    std::string openInTextEditor (const std::string& path);

    class Watch
    {
    public:
        explicit Watch (std::string folder, Opener opener = openInTextEditor)
            : files (std::move (folder)), opening (std::move (opener)) {}

        /*  Writes the source to the cue's file and opens it. Empty when asked;
            else why not, in one sentence. */
        std::string open (const std::string& cueId, const std::string& language, const std::string& text);

        /*  What was saved since the last call: each cue and its new text - a
            file whose time moved and whose text is not what Go.dot last wrote
            or took. The caller paces it. */
        std::vector<std::pair<std::string, std::string>> saved();

        /*  The file a cue's source is written to, in its language. */
        std::string fileFor (const std::string& cueId, const std::string& language) const;

        std::size_t watching() const noexcept { return out.size(); }

    private:
        struct Out
        {
            std::string file;
            std::int64_t modified = 0;
            std::string text;
        };

        std::string files;
        Opener opening;
        std::map<std::string, Out> out;     // by cue
    };

    void registerCommands (CommandRegistry& registry, const doc::ShowDocument& document, Watch* watch);
}
