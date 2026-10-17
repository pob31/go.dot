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

#include <wfg/engine/oscquery/MulticastJoin.h>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
#else
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
#endif

#include <cstring>

namespace wfg::oscquery::multicast
{
    namespace
    {
       #if defined (_WIN32)
        using Handle = SOCKET;
        using Length = int;
       #else
        using Handle = int;
        using Length = socklen_t;
       #endif

        Handle handleOf (int handle) noexcept
        {
            return static_cast<Handle> (handle);
        }

        bool addressOf (const std::string& text, in_addr& out) noexcept
        {
            return inet_pton (AF_INET, text.c_str(), &out) == 1;
        }
    }

    bool join (int handle, const std::string& group, const std::string& interfaceAddress)
    {
        ip_mreq request;
        std::memset (&request, 0, sizeof (request));

        if (handle < 0 || ! addressOf (group, request.imr_multiaddr) || ! addressOf (interfaceAddress, request.imr_interface))
            return false;

        return setsockopt (handleOf (handle), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char*> (&request), static_cast<Length> (sizeof (request))) == 0;
    }

    bool sendFrom (int handle, const std::string& interfaceAddress)
    {
        in_addr address;
        std::memset (&address, 0, sizeof (address));

        if (handle < 0 || ! addressOf (interfaceAddress, address))
            return false;

        return setsockopt (handleOf (handle), IPPROTO_IP, IP_MULTICAST_IF,
                           reinterpret_cast<const char*> (&address), static_cast<Length> (sizeof (address))) == 0;
    }

    bool setHops (int handle, int hops)
    {
        if (handle < 0)
            return false;

       #if defined (_WIN32)
        const DWORD value = static_cast<DWORD> (hops);
       #else
        const unsigned char value = static_cast<unsigned char> (hops);
       #endif

        return setsockopt (handleOf (handle), IPPROTO_IP, IP_MULTICAST_TTL,
                           reinterpret_cast<const char*> (&value), static_cast<Length> (sizeof (value))) == 0;
    }
}
