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
    A PATCH'S THREAD, AND THE STACK PD EXPECTS (namespace draft §51, ACT; §51.9).

    Each patch runs on a thread of its own (PdInstance.h), and Pd is a program
    that recurses: a message down a chain of objects is a call inside a call,
    and an abstraction opened inside another is a whole patch read inside the
    reading of its parent. Pd's own guards against a recursion that runs away
    - a thousand messages deep, a thousand loads deep - are counts, and they
    were set for the stack Pd itself runs on, a main thread of eight megabytes
    on Linux and macOS. A std::thread gets what the system gives any thread,
    which is far less on two of the three: half a megabyte on macOS, one on
    Windows. There a recursion Pd would have stopped with a line in its
    console runs off the end of the stack first, and that ends Go.dot and the
    show with it - which is how the stale load list of §51.9 showed itself on
    the macOS and Windows runners while Linux printed the depth message.

    std::thread cannot be asked for a size, so a patch's thread is started by
    the system's own call with eight megabytes asked for: reserved and not
    committed on Windows, and on every system only touched as deep as Pd goes,
    so ten patches cost address space, not memory. The thread is detached, as
    PdInstance's always was: nothing waits for it to end, and one stuck inside
    Pd (ACJ) is left to run until Go.dot ends.
*/

#include <cstddef>
#include <functional>

namespace wfg::process
{
    /*  What a patch's thread asks for: the main thread's stack on Linux and
        macOS, where Pd's depth guards were written. */
    inline constexpr std::size_t patchThreadStackBytes = std::size_t { 8 } * 1024 * 1024;

    /*  Starts `work` on a new, detached thread with a stack of `stackBytes`;
        false when the system would not start one. An exception leaving `work`
        ends the program, as one leaving a std::thread's function does. */
    bool startPatchThread (std::function<void()> work, std::size_t stackBytes = patchThreadStackBytes);

    /*  THE CALLING THREAD'S NUMBERS IN C, the process's left as they are
        (ACQ): Pd and QuickJS read and write numbers through the C library, and
        under a French locale "2.5" would read as 2. Called first thing on a
        patch's or a script's own thread; the rest of Go.dot keeps its locale. */
    void numbersInCOnThisThread();

    /*  How large the calling thread's stack is, as the system set it aside;
        nought where the system cannot say. What the tests hold the patch
        threads to. */
    std::size_t stackBytesOfThisThread();
}
