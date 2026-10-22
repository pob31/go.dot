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

#include <wfg/engine/clock/FineWait.h>

#include <algorithm>
#include <cstdint>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

 #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
 #endif
#endif

namespace wfg
{
   #if defined (_WIN32)
    FineWait::FineWait()
    {
        /*  The high-resolution timer is Windows 10 1803's; on anything older
            the flag is refused and the ordinary waitable timer stands in,
            which is the condition variable's granularity and says so in
            fine(). The event is auto-reset: one wake ends one wait. */
        timer = ::CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highResolution = timer != nullptr;

        if (timer == nullptr)
            timer = ::CreateWaitableTimerExW (nullptr, nullptr, 0, TIMER_ALL_ACCESS);

        event = ::CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }

    FineWait::~FineWait()
    {
        if (timer != nullptr) ::CloseHandle (timer);
        if (event != nullptr) ::CloseHandle (event);
    }

    void FineWait::wait (std::chrono::nanoseconds slice) noexcept
    {
        if (timer == nullptr || event == nullptr)
        {
            ::Sleep (static_cast<DWORD> (std::max<std::int64_t> (1, slice.count() / 1000000)));
            return;
        }

        //  Relative, in hundreds of nanoseconds, and at least one of them so a zero slice still yields.
        LARGE_INTEGER due;
        due.QuadPart = -std::max<std::int64_t> (1, slice.count() / 100);
        ::SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE);

        HANDLE handles[] { event, timer };
        ::WaitForMultipleObjects (2, handles, FALSE, INFINITE);
    }

    void FineWait::wake() noexcept
    {
        if (event != nullptr)
            ::SetEvent (event);
    }

   #else

    //  A condition variable's timed wait is tens of microseconds here: fine already.
    FineWait::FineWait() { highResolution = true; }
    FineWait::~FineWait() = default;

    void FineWait::wait (std::chrono::nanoseconds slice) noexcept
    {
        std::unique_lock<std::mutex> lock { mutex };
        woken.wait_for (lock, slice, [this] { return pending; });
        pending = false;
    }

    void FineWait::wake() noexcept
    {
        {
            const std::lock_guard<std::mutex> lock { mutex };
            pending = true;
        }

        woken.notify_all();
    }

   #endif
}
