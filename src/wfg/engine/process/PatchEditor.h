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
    A PATCH OPENED IN PD OR PLUGDATA, AS A PROGRAM OF ITS OWN (namespace draft
    §51, ACD, ACN, ACO; PC.7).

    What the canvas at the foot cannot do yet - a subpatch entered, an array
    drawn, a GUI box's properties - is done in Pure Data's own editor, or in
    plugdata's. The patch is written to a file of its own in the engine's cache
    and that program is started on it, apart from Go.dot: nothing it does - a
    crash, a quit, a window closed - can reach the show. Each save it makes is
    noticed and becomes one `node.set` of the cue's patch with the origin `pd`,
    which opens the running patch again: an edit there takes effect when it is
    saved, not as it is typed.

    WHICH PROGRAM. `WFG_PD` names one; else plugdata where it is installed - the
    author asked for something prettier than Pd's own window - else the Pd
    Go.dot downloaded for itself, else a Pd installed on the machine. Where none
    is found, Pd 0.56-5 - the Pd Go.dot runs patches with - is downloaded on
    first use into Go.dot's own folder, as FFmpeg is (`pd.install`): a zip on
    Windows, unpacked by JUCE, and on the Mac by the system's `ditto`, which
    keeps an application's links and its programs runnable. On Linux the
    system's package is the way (`puredata` or `plugdata`), which the .deb
    recommends.

    WHAT A REPLAY DOES: `process.edit` and `pd.install` are records, taken and
    ignored where nothing is handed in - a replay opens no window and downloads
    nothing; the saves that came back are in the log as their `node.set`s.
*/

#include <wfg/engine/plugin/ChildLaunch.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wfg { class CommandRegistry; }
namespace wfg::doc { class ShowDocument; }

namespace wfg::process::editor
{
    /*  The program a patch is edited in, and what to call it. */
    struct Found
    {
        std::string program;
        std::string name;       // "plugdata", "Pd 0.56-5", "Pd"
    };

    std::optional<Found> find();

    /*  Where Go.dot's own Pd lives: `<application data>/Go.dot/pd`. */
    std::string installFolder();

    /*  Whether this machine has a Pd to download, and from where, in words. */
    bool canDownload();
    std::string downloadSource();

    struct InstallStatus
    {
        std::string state;      ///< empty, downloading, unpacking, checking, done, failed
        int percent = 0;
        std::string problem;
    };

    /*  `pd.install`'s work, on a thread of its own, one at a time. */
    class Installer
    {
    public:
        explicit Installer (std::string folder = {}) : into (std::move (folder)) {}
        ~Installer();

        Installer (const Installer&) = delete;
        Installer& operator= (const Installer&) = delete;

        bool start();
        InstallStatus status() const;

    private:
        void run();
        void set (const char* state, int percent, const std::string& problem = {});

        std::string into;
        mutable std::mutex lock;
        InstallStatus current;
        std::thread worker;
        std::atomic<bool> stopping { false };
    };

    /*  THE PATCHES OUT FOR EDITING: a file each, the program it was opened in,
        and what was last read from it. Tick thread. */
    class Watch
    {
    public:
        explicit Watch (std::string folder) : files (std::move (folder)) {}

        /*  Writes the patch to the cue's file and starts `program` on it -
            again, for a cue already out, which brings a closed window back.
            Empty when it was started; else why not, in one sentence. */
        std::string open (const std::string& program, const std::string& cueId, const std::string& text);

        /*  What was saved since the last call: each cue and its new text. A
            file's time is read on every call, so the caller paces it. */
        std::vector<std::pair<std::string, std::string>> saved();

        std::size_t watching() const noexcept { return out.size(); }

        /*  The file a cue's patch is written to. */
        std::string fileFor (const std::string& cueId) const;

    private:
        struct Out
        {
            std::string cue;
            std::string file;
            std::int64_t modified = 0;
            std::string text;
            std::unique_ptr<plugin::ChildLaunch> child;
        };

        std::string files;
        std::vector<Out> out;
    };

    /*  `process.edit` and `pd.install`. Null pieces are what a replay and a
        tree dump have: the commands are taken and do nothing. `find` is asked
        at the command, so a Pd installed while Go.dot runs is found. */
    void registerCommands (CommandRegistry& registry, const doc::ShowDocument& document,
                           Watch* watch, Installer* installer);
}
