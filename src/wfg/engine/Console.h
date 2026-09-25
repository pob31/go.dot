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
    The `wfg` command line.

    Replaces the Phase 0 Boot.h, whose job was to prove the toolchain; this one
    drives the engine. The surface grows one verb per subphase - `serve`,
    `validate`, `canon` and `schema` arrive with the document and the OSCQuery
    server - and every verb that writes anything a human or a diff will read
    accepts --wfg-locale, so the same binary can be run twice under two locales
    and the results compared (the cross-cutting rule in the development plan).

    Vendor-free, like Engine.h, and for the same reason.

    THE WINDOW IS HANDED IN, NOT LINKED. `wfg serve <bundle> --window` opens
    the compiled client over the engine it runs inside (namespace draft
    §14.16), and the client library links this one - so this one cannot link
    the client back without the arrow pointing both ways. Instead main() hands
    runConsole a factory, and a build that hands none answers `--window` with
    a sentence. The factory gets the two doors the client is allowed - the
    engine to submit to, the tree to read snapshots from - and a way to end
    the loop; nothing it is not.
*/

#include <functional>
#include <memory>
#include <string>

namespace wfg
{
    class Engine;
    namespace tree { class ParameterTree; }
    namespace audio { class MediaInfo; }

    /** What a compiled client is, seen from here: something alive while the loop runs. */
    struct Client
    {
        virtual ~Client() = default;
    };

    /** What the console hands the client: the doors, and the way out. */
    struct ClientHost
    {
        Engine& engine;                         ///< the write door: submit (origin::window, …)
        const tree::ParameterTree& parameters;  ///< the read door: snapshot()

        /*  THE SECOND READ DOOR, and it is the SAME one the page uses.

            What a file sounds like is not in the parameter tree and never will
            be: a pyramid is tens of kilobytes of bytes per file, which is what
            `GET /media/<hash>/timbre` exists to serve and what §3.30 designed
            it for. A page must fetch that over a socket. A client in the same
            process does not have to, and asking it to would be a socket opened
            to talk to itself.

            IT IS NOT A REACH PAST THE TICK THREAD, which is what §14.16's
            second rule is actually about. `MediaInfo::snapshot()` is an
            immutable table published by the ANALYSER thread under a short
            mutex, for any reader - the HTTP thread reads exactly this, on every
            request, and has since PR 5.8. A window reading it is one more
            reader of a table built to have several, not a new way into
            anything the tick thread owns.

            Null when the console has none, which no configuration does today
            but a future verb might; every reader checks. */
        const audio::MediaInfo* media = nullptr;

        std::function<void()> quit;             ///< ends the loop the way SIGINT does
        std::string themePath;                  ///< `--theme=<file>`, resolved; empty when not given

        /*  ANOTHER SHOW IN ANOTHER WINDOW (author, 2026-09-18: "there can be
            several windows, each one for an individual project at the same
            time"). One engine holds one document, and `document.load` was
            ruled out in Phase 5 as a process restart - so a second show is a
            second PROCESS, started with this one's own flags and its own
            ports, and this is the console's to do: it has the flags, and a
            client should not know what a command line looks like.

            `createNew` makes an empty show in the folder first, which must
            then be empty or absent. Answers a sentence for the reader when it
            could not, and nothing when the window is on its way. */
        std::function<std::string (const std::string& folder, bool createNew)> openWindow;

        /*  A PLUGIN'S OWN WINDOW (author, 2026-09-25) is opened by the client
            in a helper process, and the helper makes its plugin from the
            description this machine's scan wrote - which the engine has and
            the tree does not carry, being tens of lines of XML a plugin.
            Empty when the scan does not know the identifier. */
        std::function<std::string (const std::string& identifier)> describePlugin;

        /** Where the helpers' shared regions go: the engine's cache, under editor/. */
        std::string pluginWorkFolder;
    };

    /** Builds the client, or returns nullptr having said why on stderr. */
    using ClientFactory = std::function<std::unique_ptr<Client> (const ClientHost&)>;

    /** Runs the Go.dot console front end. Returns the process exit code. */
    int runConsole (int argc, char** argv, ClientFactory makeClient = {});
}
