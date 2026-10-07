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

#include <wfg/engine/ImportChanges.h>
#include <wfg/engine/TemplateChanges.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg
{
    class Engine;
    namespace tree { class ParameterTree; }
    namespace audio { class MediaInfo; class TakePictures; }
    namespace monitor { class TrafficTap; }

    /** What a compiled client is, seen from here: something alive while the loop runs. */
    struct Client
    {
        virtual ~Client() = default;

        /*  The system asked Go.dot to quit - ⌘Q, the Dock, a Windows shutdown:
            the window's own close, which asks first and refuses in show mode,
            rather than the end of the process there and then. */
        virtual void requestClose() {}

        /*  This window's show was asked for again - a double-click on it, or
            Open show... on it from another window: come forward. */
        virtual void bringToFront() {}
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
            then be empty or absent. Without it, `folder` may be the show's
            `.wfg` as well as its folder. Answers a sentence for the reader when it
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

        /*  THE THIRD READ DOOR (Phase 9c, stage 9c.4): each sampling channel's
            take as a picture - its peaks and every layer's, for the take panel
            at the foot. The media table's shape and its argument: an immutable
            set, built from the recorders' peaks, which are atomics the audio
            thread writes and any thread reads, never anything the tick thread
            owns. Built on the thread that asks, at most every forty
            milliseconds (audio/TakePictures.h). Null with no audio side; every
            reader checks. */
        const audio::TakePictures* takes = nullptr;

        /*  OPEN THE SHOW SETTINGS AS SOON AS THERE IS A WINDOW (author,
            2026-09-30: "On a new project or a app start with no project to
            load I would open the show settings window"): `--show-settings`,
            given by New for the show it has just made and by the launchers
            when they open the empty show beside them - where the first thing
            anybody needs is the interface to play through. Never carried to
            another show, which already has one. */
        bool openSettingsAtStart = false;

        /*  THIS WINDOW IS ON THE LAUNCHER'S EMPTY SHOW (`--yield-to-opened`),
            which gives way (author, 2026-09-30): while nothing has been done
            in it, the show that New or Open starts takes its place, and this
            window goes, rather than being left beside it. Only the launchers
            pass the flag, and only for the empty show they chose. */
        bool emptyShowAtStart = false;

        /*  THE FOURTH DOOR (author, 2026-09-30: a network monitor "similar to
            the one in WFS-DIY"): what crossed the wire, OSC and MIDI, in and
            out. Not a reach past the tick thread either - it is a ring the
            socket, page, MIDI and sending threads write bytes into and nothing
            the model owns, drained by the window alone and only while it
            listens (monitor/TrafficTap.h). Null with no engine sockets; every
            reader checks. */
        monitor::TrafficTap* traffic = nullptr;

        /*  LINUX: .wfg SHOWS OPEN WITH THIS COPY (app/Associate.h) - the File
            menu's "Open .wfg files with this Go.dot", which is `wfg associate`
            from the window. Answers a sentence for the foot. Empty elsewhere:
            Windows has its installer and the Mac its app. */
        std::function<std::string()> associate {};

        /*  A PERFORMANCE AND ITS SHOW'S TEMPLATE (namespace draft §25): what
            this window's performance has that the template has not, bringing
            picks of it back, and giving a show with no template this
            performance as one. All three read the documents from the disk
            (document/Template.h) - what is not saved is not compared - and are
            the window's form of `wfg template` (§4.11). */
        std::function<TemplateComparison()> compareWithTemplate {};
        std::function<TemplateUpdate (const std::vector<TemplatePick>&, bool copySounds)> updateTemplate {};
        std::function<TemplateUpdate()> makeTemplate {};

        /*  AN ABLETON LIVE SET IMPORTED (namespace draft §29): the scenes of the
            sets picked, read for the window's list - the template's, the
            newest set's unless one is named - and the import itself, the
            window's form of `wfg import-als` (§4.11). Both read files and the
            import writes a new folder; neither touches this window's show, so
            either may run on whichever thread asks, and the import is asked
            off the message thread, saying where it has got to as it goes. */
        std::function<ImportScenes (const std::vector<std::string>& sets)> readImportScenes {};
        std::function<ImportResult (const ImportRequest&, const std::function<void (const std::string&)>& progress)>
            importSets {};

        /*  THE FIFTH DOOR: THE CANVASES AS THEY ARE (namespace draft 40, the
            author's video monitor). While a monitor window is open the
            renderer draws every canvas small, about ten times a second, and
            the window reads the latest of each here - a picture of what is up,
            like the network monitor's lines, and nothing the show decided, so
            no command and no record. Either may be called from the message
            thread; both are empty when the engine has no video. */
        struct CanvasPicture
        {
            std::string canvasId;
            int width = 0, height = 0;
            long long sample = -1;
            std::vector<unsigned char> rgb;     ///< rows from the top-left, three bytes a pixel
        };

        std::function<void (bool wanted)> monitorCanvases {};
        std::function<std::vector<CanvasPicture>()> canvasPictures {};
    };

    /** Builds the client, or returns nullptr having said why on stderr. */
    using ClientFactory = std::function<std::unique_ptr<Client> (const ClientHost&)>;

    /** Runs the Go.dot console front end. Returns the process exit code. */
    int runConsole (int argc, char** argv, ClientFactory makeClient = {});
}
