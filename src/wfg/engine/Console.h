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

    /** What a compiled client is, seen from here: something alive while the loop runs. */
    struct Client
    {
        virtual ~Client() = default;
    };

    /** What the console hands the client: both doors, and the way out. */
    struct ClientHost
    {
        Engine& engine;                         ///< the write door: submit (origin::window, …)
        const tree::ParameterTree& parameters;  ///< the read door: snapshot()
        std::function<void()> quit;             ///< ends the loop the way SIGINT does
        std::string themePath;                  ///< `--theme=<file>`, resolved; empty when not given
    };

    /** Builds the client, or returns nullptr having said why on stderr. */
    using ClientFactory = std::function<std::unique_ptr<Client> (const ClientHost&)>;

    /** Runs the Go.dot console front end. Returns the process exit code. */
    int runConsole (int argc, char** argv, ClientFactory makeClient = {});
}
