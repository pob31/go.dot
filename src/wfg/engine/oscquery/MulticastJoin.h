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
    A MULTICAST GROUP ON EVERY INTERFACE (namespace draft §76, ASA).

    juce::DatagramSocket joins a group on one interface - the one it was bound
    to, or the system's default - and sends to a group from the default alone.
    A show machine has a control network and an audio network at least, and
    the default is not always the one the devices are on: so the socket is
    joined on each interface's address, and each datagram to the group is sent
    from each in turn. Three socket options, nothing else, kept here so that
    the system's socket headers - <winsock2.h> above all, and the macros it
    brings - are included in this one file.

    The handle is juce::DatagramSocket::getRawSocketHandle()'s.
*/

#include <string>

namespace wfg::oscquery::multicast
{
    /** Joins `group` on the interface whose address is `interfaceAddress`. */
    bool join (int handle, const std::string& group, const std::string& interfaceAddress);

    /** Datagrams to a group leave from the interface whose address is `interfaceAddress`. */
    bool sendFrom (int handle, const std::string& interfaceAddress);

    /*  How many hops a datagram to a group may make: 255, as RFC 6762 §11
        asks, so a responder that checks it does not discard the question. */
    bool setHops (int handle, int hops);
}
