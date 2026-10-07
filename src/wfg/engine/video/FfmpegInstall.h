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
    FFMPEG, DOWNLOADED ON FIRST USE (namespace draft 37.5, WN, the author's
    pick): the installers stay as they are, and when a movie needs FFmpeg and
    none is found, Go.dot fetches a build into its own folder - the user's
    application data, `Go.dot/ffmpeg` - which `ffmpeg::find` looks in after
    what is beside Go.dot or named by `WFG_FFMPEG`, and before the path.

    THE BUILDS (proposed): Windows, BtbN's GPL shared build of FFmpeg 8.1;
    Linux, BtbN's static one, unpacked by the system's `tar`; the Mac, Martin
    Riedl's signed builds for the machine's processor. Each is the latest of
    its line - those links move with every rebuild and carry no checksum that
    would survive one - so what arrives is checked by running it.

    `Installer` does it on a thread of its own, one at a time, for `serve`;
    `ffmpeg.install` asks for it. Its state is a readout.
*/

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace wfg { class CommandRegistry; }

namespace wfg::video::ffmpeg
{
    /*  Where a downloaded FFmpeg lives: `<application data>/Go.dot/ffmpeg`. */
    std::string installFolder();

    /*  Whether this machine has a build to download, and from where, in words. */
    bool canDownload();
    std::string downloadSource();

    struct InstallStatus
    {
        std::string state;      ///< empty, downloading, unpacking, checking, done, failed
        int percent = 0;
        std::string problem;
    };

    class Installer
    {
    public:
        /*  Into `folder`, or `installFolder()` when it is empty - a test's
            own, so a test never writes where the user's FFmpeg lives. */
        explicit Installer (std::string folder = {}) : into (std::move (folder)) {}
        ~Installer();

        Installer (const Installer&) = delete;
        Installer& operator= (const Installer&) = delete;

        /*  Begins, unless it is under way or done. False when this machine
            has nothing to download. */
        bool start();

        InstallStatus status() const;

    private:
        void run();
        void set (const char* state, int percent, const std::string& problem = {});

        std::string into;
        mutable std::mutex lock;
        InstallStatus current;
        std::atomic<bool> stopping { false };
        std::thread thread;
    };

    /*  `ffmpeg.install`: begins the download where serve has an installer;
        taken and ignored by every verb that replays a log. */
    void registerInstallCommands (CommandRegistry& registry, Installer* installer);
}
