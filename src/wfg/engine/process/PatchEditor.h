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

        bool isPd() const { return name.rfind ("Pd", 0) == 0; }
    };

    std::optional<Found> find();

    /*  HOW THE PROGRAM IS STARTED ON A PATCH (the author, 2026-10-15). Pd is
        told where Go.dot's ready-made patches are, and the show's folder - the
        places Go.dot's own Pd looks (`-path`) - so a `[go.avg]` is a box and
        not a broken one. Pd's window also loads the plugins it finds there,
        and Go.dot's `pd/godot-screen-plugin.tcl` fits it to a scaled screen:
        menus that Pd sets at eleven pixels, and the patch zoomed. plugdata is
        handed the file alone. */
    std::vector<std::string> commandFor (const Found& program, const std::string& file,
                                         const std::vector<std::string>& searchPaths);

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
        explicit Watch (std::string folder, std::vector<std::string> searchPathsToUse = {})
            : files (std::move (folder)), searchPaths (std::move (searchPathsToUse)) {}

        /*  Writes the patch to the cue's file and starts `program` on it -
            again, for a cue already out, which brings a closed window back.
            Empty when it was started; else why not, in one sentence. A
            program with no name starts nothing (a test's). */
        std::string open (const Found& program, const std::string& cueId, const std::string& text);

        /*  Every half second. THE PATCH'S WINDOW BROUGHT IN FRONT: Pd's
            windows belong to a program Pd starts, which Windows does not let
            take the front, so they opened behind Go.dot's; each window still
            to come - a title beginning `<cue>.pd` - is raised when it shows,
            for ten seconds after its opening (Windows only). And a patch asked
            for while the one Pd was still starting is handed to it once it
            listens. */
        void tend();

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
            std::unique_ptr<plugin::ChildLaunch> child;     // plugdata's; Pd's is the one below
            std::int64_t raiseUntil = 0;        // ms; nought once raised
        };

        /*  ONE PD FOR EVERY PATCH (the author, 2026-10-15: "There are as many
            console windows as opened patches and you can't tell which console
            belongs to which patch"). The first patch starts it, told in its
            environment (GODOT_PD_EDITOR) a port and a word; Go.dot's
            `pd/godot-open-plugin.tcl` listens there, on this machine only,
            for a line that begins with the word: `open <file>` brings the
            patch's window in front, or opens it; `reopen <file>`, asked when
            the patch changed in Go.dot since Pd had it, opens it afresh. Each
            later patch is such a line. A Pd that has gone, or that does not
            answer once it has had time to start, is replaced by a new one. */
        struct OnePd
        {
            std::unique_ptr<plugin::ChildLaunch> child;
            int port = 0;
            std::string word;
            std::int64_t started = 0;           // ms
        };

        bool startPd (const Found& program, const std::string& file);
        bool askPd (const std::string& verb, const std::string& file) const;
        bool pdStarting() const;

        std::string files;
        std::vector<std::string> searchPaths;
        std::vector<Out> out;
        OnePd pd;
        std::vector<std::pair<std::string, std::string>> waiting;     // verb and file, asked while the Pd started
    };

    /*  `process.edit` and `pd.install`. Null pieces are what a replay and a
        tree dump have: the commands are taken and do nothing. `find` is asked
        at the command, so a Pd installed while Go.dot runs is found. */
    void registerCommands (CommandRegistry& registry, const doc::ShowDocument& document,
                           Watch* watch, Installer* installer);
}
