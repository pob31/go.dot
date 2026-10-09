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
    THE SHOW'S SERIAL PORTS, OPEN (namespace draft §51, ACR; PC.10).

    Each port the show declares and gives a path has a thread of its own that
    opens it, reads what arrives into lines and writes the lines it is handed;
    the tick only ever takes and hands lines under a lock, and never waits on a
    port. A line ends at a new line, a carriage return before it dropped - what
    an Arduino's `Serial.println` sends; a line longer than 4096 bytes is cut
    there. Between two takes a port keeps at most 512 lines, and counts the rest.

    A port that will not open, or fails while open - unplugged, most likely -
    is tried again after half a second, then one, two, four, and every eight
    seconds after that: opening an Arduino resets it, so never every tick. Its
    state says which, and its problem why, in words.

    `HeardLines` is what the show heard, kept by tick: the lines become
    `serial.heard` records on the tick (serve), whose handler notes them here,
    so a replay - which opens no port - holds the same lines and the same last
    line, and a process cue's patch hears each line the tick after it was
    noted, as it hears a device (Runner::advanceProcesses).
*/

#include <wfg/engine/serial/SerialLink.h>
#include <wfg/engine/serial/Slip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wfg::serial
{
    /*  What the show declares for one port. */
    struct Wanted
    {
        std::string id;
        std::string path;
        int baud = 115200;
        std::string framing = "lines";
        bool rx = true;
        bool tx = true;
    };

    /*  How one port is, for the tree. */
    struct PortState
    {
        std::string state = "closed";     // closed, opening, open, retrying
        std::string problem;
        std::uint64_t dropped = 0;
    };

    class SerialTable
    {
    public:
        explicit SerialTable (Opener opener = openSystemPort);
        ~SerialTable();

        SerialTable (const SerialTable&) = delete;
        SerialTable& operator= (const SerialTable&) = delete;

        /*  The ports as the show declares them now. One whose path, speed or
            framing changed is closed and opened again; one no longer declared,
            or with no path, is closed; rx and tx change in place. Tick thread;
            never waits for a port's thread. */
        void reconcile (const std::vector<Wanted>& wanted);

        /*  The lines each port read since the last take, oldest first, at most
            `perPort` from each - the rest wait for the next take. */
        std::vector<std::pair<std::string, std::vector<std::string>>> takeLines (std::size_t perPort);

        /*  A line out, its new line added. False when the port is not
            declared, does not transmit, is not open, or reads packets. */
        bool send (const std::string& id, const std::string& line);

        /*  A PORT WHOSE FRAMING IS SLIP (PC.11) reads and writes OSC packets
            rather than lines - for a device on it: the packets it read since
            the last take, oldest first, at most `perPort` from each; and one
            packet out, framed. */
        std::vector<std::pair<std::string, std::vector<std::vector<std::uint8_t>>>> takePackets (std::size_t perPort);
        bool sendPacket (const std::string& id, const std::vector<std::uint8_t>& packet);

        PortState stateOf (const std::string& id) const;

    private:
        struct Worker;
        Opener opener;
        std::map<std::string, std::shared_ptr<Worker>> workers;
        std::vector<std::shared_ptr<Worker>> retired;
        mutable std::mutex lock;          // workers, against stateOf from another thread

        void retire (std::shared_ptr<Worker> worker);
        void reap();
    };

    //==========================================================================
    /*  The lines the show heard, by tick: noted by `serial.heard`'s handler,
        read by the Runner and the tree. Tick thread only. */
    class HeardLines
    {
    public:
        void note (const std::string& id, const std::string& line, std::int64_t tick);

        /*  Every line noted after `tick`, oldest first, as (port, line). */
        std::vector<std::pair<std::string, std::string>> after (std::int64_t tick) const;

        /*  The last line a port said, or empty. */
        std::string lastLine (const std::string& id) const;

        /*  Forgets lines older than `tick`. */
        void forgetBefore (std::int64_t tick);

    private:
        struct Heard
        {
            std::string id;
            std::string line;
            std::int64_t tick = 0;
        };
        std::vector<Heard> heard;
        std::map<std::string, std::string> last;
    };

    /*  A line as a patch hears it: split on spaces and commas, numbers as
        numbers - each word with whether it read as one. */
    struct Word
    {
        bool isNumber = false;
        double number = 0.0;
        std::string text;
    };

    std::vector<Word> wordsOfLine (const std::string& line);
}
