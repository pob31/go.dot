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

#include <wfg/engine/osc/SocketBuffers.h>

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
 #include <sys/socket.h>
#endif

namespace wfg::osc
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

        void ask (Handle handle, int option, int bytes) noexcept
        {
            //  Refused or trimmed by the system, the read below says what is there.
            (void) setsockopt (handle, SOL_SOCKET, option, reinterpret_cast<const char*> (&bytes),
                               static_cast<Length> (sizeof (bytes)));
        }

        int read (Handle handle, int option) noexcept
        {
            int bytes = 0;
            auto length = static_cast<Length> (sizeof (bytes));

            if (getsockopt (handle, SOL_SOCKET, option, reinterpret_cast<char*> (&bytes), &length) != 0)
                return 0;

            return bytes;
        }
    }

    SocketBuffers askForBuffers (int handle, int receiveBytes, int sendBytes)
    {
        if (handle < 0)
            return {};

        const auto socket = static_cast<Handle> (handle);

        ask (socket, SO_RCVBUF, receiveBytes);
        ask (socket, SO_SNDBUF, sendBytes);

        return { read (socket, SO_RCVBUF), read (socket, SO_SNDBUF) };
    }
}
