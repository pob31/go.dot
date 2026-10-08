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
    THE SPACEMOUSE, READ BY THE ENGINE (namespace draft 45, O.11, ZE, ZF).

    A thread of Go.dot's own over hidapi: it opens the first 3Dconnexion puck
    it finds, reads its reports every twenty milliseconds at most, and keeps
    the newest state. The tick thread takes that state once a tick and turns
    the pushes into movement (`cue::rateStep`), one logged `curve.ride` a tick -
    so the hardware is read here and decided there, and a replay needs no
    puck.

    ALIVE IS THE THREAD READING, NOT THE PUCK TALKING. Every pass of the read
    loop - a report or a quiet twenty milliseconds - stamps a time, and a state
    older than 100 ms is no push at all: the puck unplugged, the thread
    stalled. A puck held still is still pushed, whether or not it says so
    again (ZE's reason for not using spatcore's driver).

    OPENED ONLY WHEN WANTED (ZF, §4.9): while a curve with a movement is
    armed. Otherwise the device is let go of, for 3Dconnexion's own driver or
    any other program. When that driver holds the puck the status says
    `driver`, and Go.dot closes it only when asked (`closeDriver`, on a thread
    of its own - it waits for the operating system).
*/

#include <wfg/engine/surface/PuckReport.h>

#include <memory>
#include <string>

namespace wfg::surface
{
    namespace spaceMouseStatus
    {
        inline constexpr const char* off       = "off";
        inline constexpr const char* searching = "searching";
        inline constexpr const char* connected = "connected";
        inline constexpr const char* driver    = "driver";
    }

    class SpaceMouse
    {
    public:
        SpaceMouse();
        ~SpaceMouse();

        bool start();
        void stop();

        /*  Whether the puck should be open now. Tick thread; cheap to call
            every tick. */
        void want (bool open);

        /*  The puck's newest state, and whether it is alive: open, and read
            within the last 100 ms. Tick thread. */
        struct Reading
        {
            PuckState state;
            bool live = false;
        };

        Reading read() const;

        /** off, searching, connected or driver - and the device's own name. Any thread. */
        std::string status() const;
        std::string name() const;

        /*  Close 3Dconnexion's driver, which is holding the puck, and look for
            it again. Asked of the operator first, always (ZF). Any thread. */
        void closeDriver();

        /*  Whether 3Dconnexion's driver is running, and closing it: spatcore's
            helpers (SpaceMouseDevice.h), transcribed. Windows and macOS; never
            elsewhere. */
        static bool driverRunning();
        static bool killDriver();

    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
