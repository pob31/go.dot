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

#include <wfg/engine/tree/MountFetcher.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/oscquery/OscQueryClient.h>
#include <wfg/engine/tree/Mount.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wfg::tree
{
    MountFetcher::~MountFetcher()
    {
        stop();
    }

    bool MountFetcher::start()
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);

        thread = std::thread ([this] { run(); });
        return true;
    }

    void MountFetcher::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        //  Under the lock, as MountProbe::stop explains.
        {
            const std::lock_guard<std::mutex> lock { guard };
            stopping.store (true, std::memory_order_relaxed);
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_relaxed);

        const std::lock_guard<std::mutex> lock { guard };
        queued.clear();
    }

    void MountFetcher::fetch (Request request)
    {
        if (request.mountId.empty() || request.host.empty() || request.queryPort <= 0 || request.prefix.empty())
            return;

        {
            const std::lock_guard<std::mutex> lock { guard };

            const auto waiting = std::find_if (queued.begin(), queued.end(),
                                               [&request] (const Request& each) { return each.mountId == request.mountId; });

            if (waiting != queued.end())
                *waiting = std::move (request);
            else
                queued.push_back (std::move (request));
        }

        wake.notify_one();
    }

    MountFetcher::Outcome MountFetcher::fetchNow (const Request& request, int timeoutMilliseconds)
    {
        Outcome outcome;
        const auto where = "http://" + request.host + ":" + std::to_string (request.queryPort) + request.prefix;

        const auto reply = oscquery::OscQueryClient::get (request.host, request.queryPort, request.prefix, {},
                                                          timeoutMilliseconds);

        if (! reply.ok)
        {
            outcome.problem = "could not read " + where + ": " + reply.error;
            return outcome;
        }

        if (reply.status != 200)
        {
            outcome.problem = where + " answered " + std::to_string (reply.status);
            return outcome;
        }

        MountDeclaration declaration;
        declaration.id = request.mountId;
        declaration.prefix = request.prefix;
        declaration.namespaceFile = fileFor (request.mountId);

        const auto read = readNamespace (declaration, reply.body);

        if (! read.ok)
        {
            outcome.problem = "the description at " + where + " did not read: "
                              + (read.problems.empty() ? std::string ("it holds no node") : read.problems.front());
            return outcome;
        }

        const auto folder = request.bundleFolder.getChildFile ("namespaces");
        const auto target = request.bundleFolder.getChildFile (fileFor (request.mountId));

        /*  ATOMICALLY: a temporary beside the target, then moved over it - so a
            reader of the bundle never sees half a description, and a failure
            leaves the old one whole. */
        juce::TemporaryFile temporary (target);

        if (! folder.createDirectory()
            || ! temporary.getFile().replaceWithText (juce::String::fromUTF8 (reply.body.data(),
                                                                              static_cast<int> (reply.body.size())))
            || ! temporary.overwriteTargetFileWithTemporary())
        {
            outcome.problem = "could not write " + target.getFullPathName().toStdString();
            return outcome;
        }

        outcome.nodeCount = read.nodes.size();
        return outcome;
    }

    void MountFetcher::run()
    {
        for (;;)
        {
            Request request;

            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed) || ! queued.empty(); });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                request = std::move (queued.front());
                queued.pop_front();
            }

            //  Outside the lock: this is the part that takes seconds.
            const auto outcome = fetchNow (request, timeoutMs.load (std::memory_order_relaxed));

            if (engine == nullptr)
                continue;

            engine->submit ("mount:" + request.mountId, "mount.described",
                            { osc::Value::string (request.mountId), osc::Value::string (fileFor (request.mountId)),
                              osc::Value::int32 (static_cast<std::int32_t> (outcome.nodeCount)),
                              osc::Value::string (outcome.problem) });
        }
    }
}
