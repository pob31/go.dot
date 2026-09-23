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
    THREE THINGS ABOUT A PROCESS BY ITS PID, which JUCE does not offer and the
    sandbox needs (Phase 9a, §17.6): who this process is, whether another is
    still there, and putting one down. The child watches its parent's pid so a
    crashed engine leaves no orphan; the parent terminates a child that stopped
    answering, because a message on a pipe never reaches a process hung inside
    a plugin's code - the plugin scan learned that first.

    Raw OS calls under two #if branches, in the shape of spatcore's
    rt/RtThreadPriority.h; nothing here touches a value.
*/

#include <cstdint>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <csignal>
 #include <sys/types.h>
 #include <unistd.h>
#endif

namespace wfg::plugin::process
{
    inline std::int64_t currentId() noexcept
    {
       #if defined (_WIN32)
        return static_cast<std::int64_t> (::GetCurrentProcessId());
       #else
        return static_cast<std::int64_t> (::getpid());
       #endif
    }

    inline bool isAlive (std::int64_t pid) noexcept
    {
        if (pid <= 0)
            return false;

       #if defined (_WIN32)
        auto* handle = ::OpenProcess (SYNCHRONIZE, FALSE, static_cast<DWORD> (pid));

        if (handle == nullptr)
            return false;

        const auto alive = ::WaitForSingleObject (handle, 0) == WAIT_TIMEOUT;
        ::CloseHandle (handle);
        return alive;
       #else
        return ::kill (static_cast<pid_t> (pid), 0) == 0;
       #endif
    }

    /** Ends it without asking. Never this process. */
    inline void terminate (std::int64_t pid) noexcept
    {
        if (pid <= 0 || pid == currentId())
            return;

       #if defined (_WIN32)
        if (auto* handle = ::OpenProcess (PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD> (pid)))
        {
            ::TerminateProcess (handle, 1);
            ::WaitForSingleObject (handle, 2000);
            ::CloseHandle (handle);
        }
       #else
        ::kill (static_cast<pid_t> (pid), SIGKILL);
       #endif
    }
}
