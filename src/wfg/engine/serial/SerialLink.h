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
    A SERIAL PORT ON THIS MACHINE (namespace draft §51, ACR; PC.10).

    An Arduino on USB is a serial port: COM3 on Windows, /dev/cu.usbmodem14101
    on a Mac, /dev/ttyACM0 on Linux. A `Link` is one of them opened at a speed,
    eight bits, no parity, one stop bit, no flow control - what an Arduino's
    `Serial.begin (115200)` speaks. It is read and written by ONE thread, the
    port's own (SerialTable), never the tick: a read waits at most as long as it
    is told, a write at most a fifth of a second.

    The system's ports are Windows' and POSIX's own calls, not asio's: the asio
    Go.dot carries is built with its serial port switched off, for the HTTP
    server's sake, and a port is a small thing. A test hands SerialTable a fake
    `Opener` instead and touches no hardware.

    OPENING ONE RESETS AN ARDUINO - the port raises DTR, which an Uno and its
    kind take as a reset - so it is opened once and kept open, and a port that
    fails is tried again after a pause that grows (SerialTable), never every
    tick.

    Never inherited by a program Go.dot starts: Windows' handle is made not to
    be, and POSIX's descriptor is opened close-on-exec.
*/

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::serial
{
    class Link
    {
    public:
        virtual ~Link() = default;

        /*  The bytes that arrived, waiting at most `wait` for the first; empty
            when none did. nullopt when the port has failed - unplugged, most
            likely - and `problem` says so; the link is then done with. */
        virtual std::optional<std::string> read (std::chrono::milliseconds wait) = 0;

        /*  False when the port has failed. */
        virtual bool write (const std::string& bytes) = 0;

        virtual std::string problem() const = 0;
    };

    /*  Opens `path` at `baud`, or says why it could not in `problem`. */
    using Opener = std::function<std::unique_ptr<Link> (const std::string& path, int baud, std::string& problem)>;

    /*  This machine's ports. */
    std::unique_ptr<Link> openSystemPort (const std::string& path, int baud, std::string& problem);

    /*  The ports this machine has now, each a path and a few words about it:
        Windows' COM ports from the registry, a Mac's /dev/cu.*, Linux's
        /dev/serial/by-id names and the ttyACM and ttyUSB they point at. */
    struct SystemPort
    {
        std::string path;
        std::string about;
    };

    std::vector<SystemPort> systemPorts();

    /*  The speeds every system here opens. */
    bool isStandardBaud (int baud) noexcept;
}
