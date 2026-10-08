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
    LISTENING TO A DEVICE THAT CAN BE ASKED (namespace draft 45, O.10, YB).

    A device that describes itself over OSCQuery and offers LISTEN pushes the
    values it changes down a WebSocket to whoever asked for them - WFS-DIY
    every 30 ms, one value a message, and never to the machine whose write
    caused the change. Go.dot asks only for what it is recording: the addresses
    of the curves armed on a device whose `rx` is on, and nothing at all while
    no curve is armed (§4.9: it arrives knowing nothing and leaves knowing
    nothing). What comes down the socket goes into the HeardBox as a report of
    that device, and from there into the log as `mount.heard`, exactly as a
    datagram from its host would - so a replay needs no socket: what LISTEN
    yielded is in the log already.

    ONE THREAD OF ITS OWN, MountProbe's shape. The tick thread says what it
    wants - by device, its host, its query port and the addresses - and the
    thread does the rest: asks the device's HOST_INFO whether it listens and on
    which port, connects, sends a LISTEN for each address and an IGNORE for
    each one no longer wanted, and starts again after a second, two, then five
    when the device goes away. Each connection reads on a thread of its own
    (juce_simpleweb's client runs its own io context). What the tick thread
    reads back is a word per device for `mount/listen`.

    Nothing here is the show's: what is wanted is derived from what is armed,
    and the status is what the machine is doing.
*/

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace wfg::tree
{
    class HeardBox;

    namespace listenStatus
    {
        inline constexpr const char* off         = "off";
        inline constexpr const char* connecting  = "connecting";
        inline constexpr const char* listening   = "listening";
        inline constexpr const char* unsupported = "unsupported";
        inline constexpr const char* unreachable = "unreachable";
    }

    class MountListener
    {
    public:
        /*  What is wanted of one device: where its OSCQuery server is, and the
            addresses to be told about. */
        struct Device
        {
            std::string host;
            int queryPort = 0;
            std::set<std::string> addresses;

            bool operator== (const Device&) const = default;
        };

        /** By mount identifier. */
        using Wanted = std::map<std::string, Device>;

        explicit MountListener (HeardBox& boxToFill);
        ~MountListener();

        bool start();
        void stop();
        bool isRunning() const noexcept { return running.load(); }

        /*  WHAT IS WANTED NOW. Tick thread; a call with what was wanted already
            costs a comparison. A device left out is let go of. */
        void want (Wanted wanted);

        /*  The word for `mount/listen`: off, connecting, listening, unsupported
            (it answers, but does not offer LISTEN) or unreachable. Any thread. */
        std::string statusOf (const std::string& mountId) const;

        /*  The waits before trying a device again, after the first, second and
            every later failure. For a test, before `start`. */
        void setRetryDelays (std::chrono::milliseconds first, std::chrono::milliseconds second,
                             std::chrono::milliseconds later) noexcept;

    private:
        struct Session;

        void run();
        void tend (const std::string& mountId, const Device& device, Session& session);
        void setStatus (const std::string& mountId, const char* word);

        HeardBox& box;

        mutable std::mutex guard;
        std::condition_variable wake;
        Wanted wanted;
        bool changed = false;
        bool stopping = false;
        std::map<std::string, std::string> statuses;

        //  The manager thread's own, never touched by another.
        std::map<std::string, std::unique_ptr<Session>> sessions;

        std::chrono::milliseconds retryFirst { 1000 }, retrySecond { 2000 }, retryLater { 5000 };

        std::atomic<bool> running { false };
        std::thread thread;
    };
}
