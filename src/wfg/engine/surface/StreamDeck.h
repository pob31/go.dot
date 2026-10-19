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
    THE STREAM DECKS, READ AND DRAWN BY THE ENGINE (namespace draft §95, SD.2).

    One thread of Go.dot's own over hidapi, for every deck on the machine, the
    SpaceMouse's way (SpaceMouse.h): it lists the decks plugged in, opens the
    ones the show asks for and lets the others go, reads what their hands did
    and keeps it for the tick, and draws and sends what the tick asked each
    key to show. So the hardware is read here and decided in the bridge
    (DeckBridge), and a replay needs no deck.

    NEVER THE TICK THREAD'S TIME. A key's picture is drawn, encoded and cut
    into reports here; the tick only says what it should show, and the newest
    word for a key replaces one not yet sent. A whole XL is thirty-two
    pictures and a few hundred milliseconds of USB, and the GO path never
    waits on it.

    A DECK ARRIVES KNOWING NOTHING AND LEAVES KNOWING NOTHING (PRD §4.9): opened,
    it is reset and told so, and the bridge draws every key again; let go of,
    it is reset to Elgato's own picture before it is closed. Unplugged, it is
    closed and looked for again.

    ELGATO'S APP HOLDS THE DECKS when it runs: a deck that is plugged in and
    will not open is `busy`, and Go.dot closes the app only when the operator
    asks (`closeApp`), as it closes 3Dconnexion's driver.

    `DeckHost` is what the bridge sees of all this, so a test drives the bridge
    with a deck of its own making.
*/

#include <wfg/engine/surface/DeckPicture.h>
#include <wfg/engine/surface/StreamDeckReport.h>

#include <memory>
#include <string>
#include <vector>

namespace wfg::surface
{
    /*  What happened at a deck since the tick last asked. */
    struct DeckEvent
    {
        enum class Kind { opened, closed, input };

        Kind kind = Kind::input;
        std::string serial;
        DeckInput input;
    };

    class DeckHost
    {
    public:
        virtual ~DeckHost() = default;

        /*  A DECK ON THE MACHINE, open or not. A serial is its identity: the
            one thing that survives a different USB port. */
        struct Plugged
        {
            std::string serial;
            DeckSpec spec;
            bool open = false;
            bool busy = false;      // plugged in and would not open: another program has it
        };

        /** Every deck plugged in, as last looked. Any thread. */
        virtual std::vector<Plugged> plugged() const = 0;

        /** The decks that should be open now, by serial; every other one is
            let go of. Tick thread; cheap. */
        virtual void want (const std::vector<std::string>& serials) = 0;

        /** What happened since the last call, in order. Tick thread. */
        virtual std::vector<DeckEvent> drain() = 0;

        /** What a key, a part of the strip or the screen should show now. The
            newest replaces anything not yet sent. Tick thread. */
        virtual void key (const std::string& serial, int key, const KeyPicture& picture) = 0;
        virtual void zone (const std::string& serial, int zone, const ZonePicture& picture) = 0;
        virtual void screen (const std::string& serial, const ScreenPicture& picture) = 0;
        virtual void brightness (const std::string& serial, int percent) = 0;

        /** Whether Elgato's own app is running, as last looked. Any thread. */
        virtual bool appRunning() const = 0;

        /** Close Elgato's app, on a thread of its own: asked of the operator,
            always. Any thread. */
        virtual void closeApp() = 0;
    };

    //==========================================================================
    class StreamDecks final : public DeckHost
    {
    public:
        StreamDecks();
        ~StreamDecks() override;

        bool start();
        void stop();

        std::vector<Plugged> plugged() const override;
        void want (const std::vector<std::string>& serials) override;
        std::vector<DeckEvent> drain() override;
        void key (const std::string& serial, int key, const KeyPicture& picture) override;
        void zone (const std::string& serial, int zone, const ZonePicture& picture) override;
        void screen (const std::string& serial, const ScreenPicture& picture) override;
        void brightness (const std::string& serial, int percent) override;
        bool appRunning() const override;
        void closeApp() override;

        /*  Elgato's app, looked for and closed: spatcore's helpers
            (StreamDeckDevice.h), transcribed. Windows and macOS; never
            elsewhere. */
        static bool elgatoAppRunning();
        static bool killElgatoApp();

    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
