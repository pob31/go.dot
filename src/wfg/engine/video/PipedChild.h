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
    A CHILD WHOSE OUTPUT IS READ (namespace draft 37.6, F.1): FFmpeg, sending
    a file's description or its frames down a pipe.

    It inherits exactly three handles and nothing else - its input from the
    null device, its output into the pipe this end reads, its errors into a
    file - for `ChildLaunch`'s reason: serve's sockets are inheritable on
    Windows, and a child holding one keeps a client waiting for a connection
    that never closes. On Windows that is an explicit handle list; elsewhere
    `posix_spawn`, where the sockets carry close-on-exec.

    One reader, one thread. Reads block; `kill` from another thread ends one.
    NAMES NO JUCE TYPE. The command is words, UTF-8.
*/

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::video
{
    class PipedChild
    {
    public:
        PipedChild();
        ~PipedChild();     ///< kills a child still running, and waits for it

        PipedChild (const PipedChild&) = delete;
        PipedChild& operator= (const PipedChild&) = delete;

        /*  The executable's full path, then its words; what it says on its
            error stream goes to `errorFile`, or nowhere when that is empty.
            False when the process could not be made. */
        bool start (const std::vector<std::string>& command, const std::string& errorFile = {});

        /*  Up to `size` bytes of its output: how many came, nought at its
            end, below nought on a fault. Blocks until some come. */
        std::int64_t read (void* into, std::size_t size);

        /*  Exactly `size` bytes, or false at its end before they all came. */
        bool readExactly (void* into, std::size_t size);

        /*  Everything until its end, at most `limit` bytes. */
        std::string readAll (std::size_t limit = 16u << 20);

        /*  Its exit code once it has ended, waiting up to `milliseconds`;
            -1 while it runs on. */
        int wait (int milliseconds);

        bool isRunning() const;

        /** Ends it without asking; a read blocked on it returns. */
        void kill();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
