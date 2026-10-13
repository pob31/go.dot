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

#include <wfg/engine/process/PatchThread.h>

#include <memory>
#include <utility>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <process.h>
#else
 #include <pthread.h>
#endif

namespace wfg::process
{
    namespace
    {
        /*  What the new thread is handed, and owns from its first line: the
            starting side lets go of it only once the system has said the
            thread exists. */
        struct Start
        {
            std::function<void()> work;
        };

        void runStart (void* argument) noexcept
        {
            const std::unique_ptr<Start> start (static_cast<Start*> (argument));
            start->work();
        }

       #if defined (_WIN32)
        unsigned __stdcall threadEntry (void* argument) noexcept
        {
            runStart (argument);
            return 0;
        }
       #else
        void* threadEntry (void* argument) noexcept
        {
            runStart (argument);
            return nullptr;
        }
       #endif
    }

    bool startPatchThread (std::function<void()> work, std::size_t stackBytes)
    {
        auto start = std::make_unique<Start> (Start { std::move (work) });

       #if defined (_WIN32)
        /*  _beginthreadex rather than CreateThread, so the C library's own
            state for the thread is set up and taken down with it. Without
            STACK_SIZE_PARAM_IS_A_RESERVATION the size would be COMMITTED -
            eight megabytes of the machine's memory charged to every patch
            before it ran a block; with it, the size is the address space set
            aside and pages are committed only as Pd reaches them. */
        const auto handle = ::_beginthreadex (nullptr, static_cast<unsigned> (stackBytes), threadEntry, start.get(),
                                              STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (handle == 0)
            return false;
        static_cast<void> (start.release());    // the thread owns it now
        ::CloseHandle (reinterpret_cast<HANDLE> (handle));
        return true;
       #else
        pthread_attr_t attributes;
        if (::pthread_attr_init (&attributes) != 0)
            return false;

        //  A size the system refuses - under its minimum, or not whole pages
        //  where it asks for that - leaves the thread its default rather than
        //  no thread at all.
        ::pthread_attr_setstacksize (&attributes, stackBytes);
        ::pthread_attr_setdetachstate (&attributes, PTHREAD_CREATE_DETACHED);

        pthread_t thread {};
        const int made = ::pthread_create (&thread, &attributes, threadEntry, start.get());
        ::pthread_attr_destroy (&attributes);
        if (made != 0)
            return false;
        static_cast<void> (start.release());    // the thread owns it now
        return true;
       #endif
    }

    std::size_t stackBytesOfThisThread()
    {
       #if defined (_WIN32)
        ULONG_PTR low = 0, high = 0;
        ::GetCurrentThreadStackLimits (&low, &high);
        return static_cast<std::size_t> (high - low);
       #elif defined (__APPLE__)
        return ::pthread_get_stacksize_np (::pthread_self());
       #elif defined (__linux__)
        pthread_attr_t attributes;
        if (::pthread_getattr_np (::pthread_self(), &attributes) != 0)
            return 0;
        std::size_t bytes = 0;
        if (::pthread_attr_getstacksize (&attributes, &bytes) != 0)
            bytes = 0;
        ::pthread_attr_destroy (&attributes);
        return bytes;
       #else
        return 0;
       #endif
    }
}
