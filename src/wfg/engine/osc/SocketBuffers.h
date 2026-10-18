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
    A SOCKET'S TWO BUFFERS, ASKED LARGER (namespace draft §88, AVA).

    juce::DatagramSocket leaves each at the larger of the system's default and
    64 KB, and says nothing more about them. A processor's capture of seventy
    inputs is 190 datagrams of up to 1,200 bytes sent back to back - 228 KB -
    and a datagram the receive buffer has no room for is dropped by the system
    before Go.dot's reading thread can take it: 28 messages missing from a cue
    and nothing said. Two socket options, nothing else, kept here so that the
    system's socket headers are included in this one file, as
    oscquery/MulticastJoin.cpp keeps its own.

    The handle is juce::DatagramSocket::getRawSocketHandle()'s. The system may
    give less than it is asked for (Linux halves the ceiling `rmem_max` sets);
    what it gave is read back, and less is not a failure.
*/

namespace wfg::osc
{
    struct SocketBuffers
    {
        int receive = 0;    ///< bytes, as the system reports them after the ask
        int send = 0;
    };

    /*  Asks for `receiveBytes` and `sendBytes` and answers what the socket has
        afterwards: nought for both when the handle is not a socket. */
    SocketBuffers askForBuffers (int handle, int receiveBytes, int sendBytes);
}
