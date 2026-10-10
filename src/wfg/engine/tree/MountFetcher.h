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
    THE THREAD THAT FETCHES A PROCESSOR'S DESCRIPTION (namespace draft §56,
    decision AEM): when a processor declares itself with a query port, Go.dot
    asks `GET http://host:queryPort<prefix>`, checks the answer as a namespace
    file, keeps it in the bundle as `namespaces/<id>.json`, and reports.

    MountProbe's shape and MountProbe's reason: an HTTP GET to a box that has
    gone away costs its whole timeout, and the tick thread does not wait. The
    answer comes back as a command - `mount.described` - applied on the tick it
    arrives and logged, so `wfg replay` loads the same file at the same moment
    with no network in the room.

    THE FILE IS WRITTEN BEFORE THE RECORD IS SUBMITTED, so the record can never
    name a file that is not there yet. A fetch that fails writes nothing and
    keeps whatever description the device had: the record carries the problem,
    and the device's problem cell says it.

    IT NEVER TOUCHES THE MODEL. It holds a host, a port, a root and a folder, and
    it speaks to an Engine.
*/

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace wfg
{
    class Engine;
}

namespace wfg::tree
{
    class MountFetcher
    {
    public:
        explicit MountFetcher (Engine& engineToReportTo) noexcept : engine (&engineToReportTo) {}
        ~MountFetcher();

        MountFetcher (const MountFetcher&) = delete;
        MountFetcher& operator= (const MountFetcher&) = delete;

        struct Request
        {
            std::string mountId {};
            std::string host {};
            int queryPort = 0;
            std::string prefix {};
            juce::File bundleFolder {};
        };

        /*  Starts the thread. Idempotent; false if it was already running. */
        bool start();

        /** Stops it and joins. The destructor calls it. */
        void stop();

        /*  Tick thread. Queues one fetch; a request for a device already
            waiting replaces that one (the newest host and port win). */
        void fetch (Request request);

        /** How long one GET is given. */
        void setTimeout (int milliseconds) noexcept { timeoutMs = milliseconds; }

        /** The file a device's description is kept in, bundle-relative. */
        static std::string fileFor (const std::string& mountId) { return "namespaces/" + mountId + ".json"; }

        /*  The fetch itself, blocking, without the thread: the GET, the check,
            the file, and the arguments of the `mount.described` record. Public
            for the tests. */
        struct Outcome
        {
            std::size_t nodeCount = 0;
            std::string problem {};
        };

        static Outcome fetchNow (const Request& request, int timeoutMilliseconds);

    private:
        void run();

        Engine* engine = nullptr;

        std::mutex guard;
        std::condition_variable wake;
        std::deque<Request> queued;

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };
        std::atomic<int> timeoutMs { 5000 };

        std::thread thread;
    };
}
