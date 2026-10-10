// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*  A TCP CONNECTION AS A LINK (namespace draft §57, AFJ; DP.6).

    A device reached over TCP - an Eos on 3032, a Yamaha console's RCP on
    49280, a grandMA2's telnet on 30000 - is a stream, and a stream is what
    the serial table already runs: one thread per link that opens, reads,
    writes what the tick thread queued, and when the far end goes away
    closes, waits half a second, one, two, four, then eight, and opens again.
    So a TCP link is a `serial::Link` whose "path" is `host:port` and whose
    baud means nothing, handed to a second `SerialTable` keyed by the mount's
    identifier; the backoff, the framing and the state words come with it.

    The socket is JUCE's StreamingSocket, as the OSC port is its
    DatagramSocket: connected with a bounded wait, read with a bounded wait,
    written whole. A read that finds the stream closed is the failure the
    worker retries on. */

#include <wfg/engine/serial/SerialLink.h>

#include <memory>
#include <string>

namespace wfg::serial
{
    /*  Opens a TCP connection to `hostPort`, spelled `host:port`, or says why
        it could not in `problem`. `baud` is taken and ignored, so that this is
        an `Opener` and a `SerialTable` can run it. */
    std::unique_ptr<Link> openTcpLink (const std::string& hostPort, int baud, std::string& problem);

    /*  `host:port` from a host and a port, and back: the one spelling both
        ends of the table use. */
    std::string hostPortOf (const std::string& host, int port);
    bool splitHostPort (const std::string& hostPort, std::string& host, int& port);
}
