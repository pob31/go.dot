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
    A WAIT THAT ENDS WHEN IT SAYS IT WILL, for the tick thread (namespace draft
    §108, the Windows half; the author, 2026-10-22: "Is there anything to do to
    avoid glitches and other underruns?").

    The tick thread sleeps to two milliseconds short of the next tick and then
    waits in quarter-millisecond slices. A std::condition_variable's timed wait
    is what the system's timer makes it: on macOS and Linux that is tens of
    microseconds, but on Windows a timed wait ends on the next timer interrupt,
    and a process that has not asked for a finer one is promised no better than
    15.6 ms - and under Windows 11 a process whose window is minimised loses
    even what it asked for. So a quarter-millisecond slice became a wait of up
    to 15 ms, and that whole figure went straight to the tick's lateness on any
    machine where no other program happened to have raised the timer.

    On Windows this waits on a high-resolution waitable timer, about half a
    millisecond at worst and independent of what any program asked the system
    for, beside an event that ends the wait early. Elsewhere it is the
    condition variable it always was. One object per waiting thread; wake()
    from any other.
*/

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace wfg
{
    class FineWait
    {
    public:
        FineWait();
        ~FineWait();

        FineWait (const FineWait&) = delete;
        FineWait& operator= (const FineWait&) = delete;

        /*  Returns after `slice`, or at once on a wake() that came since the
            last wait returned - a wake is never lost between two waits. The
            caller holds no lock of its own across it and re-reads whatever it
            waits for afterwards. */
        void wait (std::chrono::nanoseconds slice) noexcept;

        /** Ends the wait in progress, or the next one. */
        void wake() noexcept;

        /** True where the system gave a timer finer than its ordinary tick. */
        bool fine() const noexcept { return highResolution; }

    private:
        bool highResolution = false;

       #if defined (_WIN32)
        void* timer = nullptr;
        void* event = nullptr;
       #else
        std::mutex mutex;
        std::condition_variable woken;
        bool pending = false;
       #endif
    };
}
