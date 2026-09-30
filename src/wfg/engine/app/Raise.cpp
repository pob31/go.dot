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

//  Windows and Linux. The Mac's is Raise_mac.mm, which needs AppKit.
#if ! defined (__APPLE__)

#include <wfg/engine/app/Raise.h>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
#endif

namespace wfg::app
{
    long currentProcessId() noexcept
    {
       #if defined (_WIN32)
        return static_cast<long> (GetCurrentProcessId());
       #else
        return static_cast<long> (getpid());
       #endif
    }

   #if defined (_WIN32)
    namespace
    {
        /*  THE LARGEST of the process's visible, unowned top-level windows:
            the show's main window, rather than its settings or a monitor it
            has open beside it, which are smaller - and never one of the
            hidden windows JUCE keeps for its messages. */
        struct Search
        {
            DWORD process = 0;
            HWND best = nullptr;
            long long area = 0;
        };

        BOOL CALLBACK consider (HWND window, LPARAM parameter)
        {
            auto& search = *reinterpret_cast<Search*> (parameter);

            DWORD owner = 0;
            GetWindowThreadProcessId (window, &owner);

            if (owner != search.process || ! IsWindowVisible (window)
                  || GetWindow (window, GW_OWNER) != nullptr)
                return TRUE;

            RECT bounds {};

            if (! GetWindowRect (window, &bounds))
                return TRUE;

            const auto area = static_cast<long long> (bounds.right - bounds.left)
                                * static_cast<long long> (bounds.bottom - bounds.top);

            if (area > search.area)
            {
                search.area = area;
                search.best = window;
            }

            return TRUE;
        }
    }

    bool raiseProcessWindows (long processId)
    {
        Search search;
        search.process = static_cast<DWORD> (processId);
        EnumWindows (consider, reinterpret_cast<LPARAM> (&search));

        if (search.best == nullptr)
            return false;

        if (IsIconic (search.best))
            ShowWindow (search.best, SW_RESTORE);

        /*  Windows lets a process put another's window in front only when it
            was itself started by the one in front - which a double-click is,
            and a launcher that passes it on is. Refused, it flashes the
            window's taskbar button instead, which still says where the show is. */
        SetForegroundWindow (search.best);
        return true;
    }
   #else
    bool raiseProcessWindows (long)
    {
        return false;
    }
   #endif
}

#endif
