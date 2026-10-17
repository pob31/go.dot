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
    A NEWER GO.DOT, AS THE WINDOW READS IT (namespace draft §78).

    The author, 2026-10-17: "an automatic update checking for newer version on
    GH". What he chose: say so and link to the release page, never download or
    install (ASO); real releases only - GitHub's "latest", which never names a
    pre-release or a draft (ASP); a Help menu and a quiet mark on the title
    line, never a pop-up (ASQ); on by default, at most once a day at launch,
    a switch on each machine to turn it off (ASR).

    WFS-DIY's UpdateChecker is the model (its Source/UpdateChecker.h, audited
    2026-09-29), and this is its reading half, std only like the rest of
    model/: what a version is, which is newer, what of GitHub's answer is
    believed, and what this machine remembers between launches. The asking
    is ui/UpdateCheck's.

    A VERSION IS THREE NUMBERS (the author, 2026-10-17: "no beta in the
    version number"). A tag that is not a plain `vX.Y.Z` is not a version,
    and nothing that is not one is ever offered (ASV).
*/

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wfg::client::model::updates
{
    /** Where the window asks, unless the launch names another (`--update-url`, for a test). */
    inline constexpr const char* latestUrl = "https://api.github.com/repos/pob31/go.dot/releases/latest";

    /** The only pages offered as a link: this project's own releases (ASU). */
    inline constexpr const char* ownReleases = "https://github.com/pob31/go.dot/releases/";

    /** Every release, for the Help menu's item when nothing newer is known. */
    inline constexpr const char* releasesPage = "https://github.com/pob31/go.dot/releases";

    /** A day between two asks at launch (ASR); a person asking from the menu is never held to it. */
    inline constexpr std::int64_t secondsBetweenAsks = 24 * 60 * 60;

    /** GitHub's reply is tens of kilobytes; anything past this is not a release (ASU). */
    inline constexpr std::size_t largestReply = 1u << 20;

    struct Version
    {
        int major = 0, minor = 0, patch = 0;

        bool operator== (const Version&) const = default;
    };

    /** `1.2.3` or `v1.2.3`, three numbers and nothing else; anything else is no version. */
    std::optional<Version> readVersion (std::string_view text);

    /** `1.2.3`. */
    std::string versionText (const Version& version);

    /** Whether `candidate` comes after `running`, the three numbers compared in order. */
    bool isNewer (const Version& candidate, const Version& running);

    /** What GitHub's answer says, when it is a release of this project. */
    struct Release
    {
        Version version;
        std::string page;   ///< under ownReleases, always
    };

    /*  GITHUB'S ANSWER, BELIEVED ONLY WHERE IT IS SAFE TO (ASU): `tag_name` as a
        plain version, `html_url` only under this project's releases, never a
        draft or a pre-release, and nothing larger than `largestReply`. Anything
        else is no release - a refusal to guess, not an error to show. */
    std::optional<Release> readRelease (std::string_view reply);

    /*  WHAT AN ASK CAME BACK WITH. `reached`: GitHub answered as it answers - a
        release, or none published yet (404). A refusal or an error page (403 when
        the hourly allowance is spent, 5xx), no connection, or a reply past
        `largestReply` is no answer, so it never makes the window forget what it
        found before. `status` is the HTTP status, -1 for none; a file a test
        wrote is read as a 200. */
    struct Answer
    {
        bool reached = false;
        std::optional<Release> release;
    };

    Answer answerFor (int status, std::string_view body);

    /*  WHAT A TOOL SAID, ON LINUX (ASY): Go.dot speaks no TLS, and JUCE's Linux
        web stream speaks plain http alone, so the window asks through the
        system's own program. curl, given `--write-out "\n%{http_code}"`, prints
        the body and then the status on a line of its own - `000` when it could
        not connect. wget prints the body alone and says by its exit code only
        whether it got a 200: anything else is no answer, as it cannot say which
        status it had. Output with no status line is curl not having run. */
    Answer answerFromCurl (std::string_view output);
    Answer answerFromWget (int exitCode, std::string_view output);

    /*  WHAT THIS MACHINE REMEMBERS, in `updates.txt` beside `last-show.txt`:
        whether to ask at launch, when it last asked, the newest release it
        heard of, and the one somebody said not now to. Read from lines
        `<key> <value>`; a line it does not know is passed over, so an older
        Go.dot reads a newer one's file. Never a node in the tree and never a
        record: what the machine happened to find is not something anybody
        decided (§4.10). */
    struct Memory
    {
        bool askAtLaunch = true;
        std::int64_t lastAsked = 0;     ///< seconds since 1970, 0 never
        std::string found;              ///< the newest version heard of, `1.2.3`, or empty
        std::string foundPage;          ///< its release page
        std::string dismissed;          ///< the version somebody said not now to
    };

    Memory readMemory (std::string_view text);
    std::string writeMemory (const Memory& memory);

    /*  WHETHER A LAUNCH ASKS: the switch on, and a day since the last ask - or a
        clock that has gone back past it, which would otherwise never ask again. */
    bool dueToAsk (const Memory& memory, std::int64_t now);

    /*  AN ANSWER KEPT: the time, and the release when there is one. No release -
        none published yet, or nothing believable - forgets what was found, so a
        release taken down stops being offered. */
    void remember (Memory& memory, std::int64_t now, const std::optional<Release>& release);

    /*  THE MARK ON THE TITLE LINE (ASQ): the version to name, when what was found
        is newer than what runs and nobody has said not now to it; else nothing. */
    std::optional<Version> markFor (const Memory& memory, const Version& running);

    /*  WHAT A CHECK ASKED FROM THE MENU SAYS AT THE FOOT, in words, whatever it
        found (ASW): newer, nothing newer, or no answer. */
    std::string checkedWords (bool reached, const std::optional<Release>& release, const Version& running);

    /** The mark's words: "Go.dot 1.2.3 is out". */
    std::string markWords (const Version& version);
}
