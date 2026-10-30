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

/*  A DEVICE'S OWN DATAGRAM SOCKET AS A LINK (namespace draft §124, BES).

    Go.dot's one OSC socket reads OSC; a Sennheiser receiver on SSC answers
    JSON, a datagram each, to the port the question came from. So such a
    device gets a socket of its own, bound to any free port, sending to
    `host:port` and reading what that host sends back - run by a third
    `SerialTable` whose framing is `raw`, every read one datagram, so it is
    kept open, read and written on its own thread as a connection is. A
    datagram has no connection to lose: the link stays open while the socket
    does, and a device that is off is a device that never answers. */

#include <wfg/engine/serial/SerialLink.h>

#include <memory>
#include <string>

namespace wfg::serial
{
    /*  Opens a socket that sends to and hears from `hostPort`, spelled
        `host:port`, or says why it could not in `problem`. `baud` is taken
        and ignored, so that this is an `Opener`. */
    std::unique_ptr<Link> openUdpLink (const std::string& hostPort, int baud, std::string& problem);
}
