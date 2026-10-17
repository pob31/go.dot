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
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/oscquery/OscQueryClient.h>
#include <wfg/engine/pair/Hold.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/TreeCommands.h>

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

        //  A held machine asks its devices nothing (RD.1, AGG).
        if (pair::isHeld (hold))
            return;

        {
            const std::lock_guard<std::mutex> lock { guard };

            const auto waiting = std::find_if (queued.begin(), queued.end(),
                                               [&request] (const Job& each)
                                               { return ! each.discovering && each.request.mountId == request.mountId; });

            if (waiting != queued.end())
                waiting->request = std::move (request);
            else
                queued.push_back ({ std::move (request), false, {} });
        }

        wake.notify_one();
    }

    void MountFetcher::discover (Discover request)
    {
        if (request.mountId.empty() || request.host.empty() || request.queryPort <= 0)
            return;

        //  A held machine asks its devices nothing (RD.1, AGG).
        if (pair::isHeld (hold))
            return;

        {
            const std::lock_guard<std::mutex> lock { guard };

            const auto waiting = std::find_if (queued.begin(), queued.end(),
                                               [&request] (const Job& each)
                                               { return each.discovering && each.discovery.mountId == request.mountId; });

            if (waiting != queued.end())
                waiting->discovery = std::move (request);
            else
                queued.push_back ({ {}, true, std::move (request) });
        }

        wake.notify_one();
    }

    MountFetcher::Discovered MountFetcher::discoverNow (const Discover& request, int timeoutMilliseconds)
    {
        Discovered outcome;
        outcome.name = request.name;
        const auto where = "http://" + request.host + ":" + std::to_string (request.queryPort);

        const auto reply = oscquery::OscQueryClient::get (request.host, request.queryPort, "/", {}, timeoutMilliseconds);

        if (! reply.ok)
        {
            outcome.problem = "nothing answered at " + where + ": " + reply.error;
            return outcome;
        }

        if (reply.status != 200)
        {
            outcome.problem = where + " answered " + std::to_string (reply.status);
            return outcome;
        }

        /*  WHAT IT SAYS OF ITSELF: the port it takes OSC on and how, its name,
            whether a value can be asked of it. A server with no HOST_INFO is
            still a description - its OSC port is then typed afterwards. */
        if (const auto info = oscquery::OscQueryClient::get (request.host, request.queryPort, "/", "HOST_INFO",
                                                             timeoutMilliseconds);
            info.ok && info.status == 200)
        {
            if (const auto parsed = json::parse (info.body); parsed.ok() && parsed.value->isObject())
            {
                const auto& host = *parsed.value;

                if (const auto* port = host.find ("OSC_PORT"); port != nullptr && port->isNumber()
                      && port->asInt() > 0 && port->asInt() <= 65535)
                    outcome.oscPort = port->asInt();

                if (const auto* transport = host.find ("OSC_TRANSPORT"); transport != nullptr && transport->isString())
                    outcome.transport = transport->asString() == "TCP" ? "tcp" : "udp";

                if (const auto* named = host.find ("NAME"); outcome.name.empty() && named != nullptr && named->isString())
                    outcome.name = named->asString();

                if (const auto* extensions = host.find ("EXTENSIONS"); extensions != nullptr)
                    if (const auto* value = extensions->find ("VALUE"); value != nullptr && value->isBool() && value->asBool())
                        outcome.readback = "oscquery";
            }
        }

        if (outcome.name.empty())
            outcome.name = "OSCQuery at " + request.host;

        //  THE ROOTS ARE THE DESCRIPTION'S (AFK), each one a device may have.
        const auto roots = rootsOfNamespace (reply.body);

        if (roots.empty())
        {
            outcome.problem = "the description at " + where + " has no root to make a device at";
            return outcome;
        }

        for (const auto& root : roots)
        {
            if (root == "/godot")
            {
                outcome.problem = where + " is another Go.dot, not a device";
                return outcome;
            }

            if (! isUsableRoot (root))
            {
                outcome.problem = "the description at " + where + " has the root " + root
                                  + ", which no device may have";
                return outcome;
            }

            outcome.roots += (outcome.roots.empty() ? "" : " ") + root;
        }

        MountDeclaration declaration;
        declaration.id = request.mountId;
        declaration.prefix = outcome.roots;
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
        juce::TemporaryFile temporary (target);

        if (! folder.createDirectory()
            || ! temporary.getFile().replaceWithData (reply.body.data(), reply.body.size())
            || ! temporary.overwriteTargetFileWithTemporary())
        {
            outcome.problem = "could not write " + target.getFullPathName().toStdString();
            return outcome;
        }

        outcome.nodeCount = read.nodes.size();
        return outcome;
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
            Job job;

            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed) || ! queued.empty(); });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                job = std::move (queued.front());
                queued.pop_front();
            }

            /*  A DEVICE AT AN ADDRESS (§76, ARX): what was learned, on the
                record - the file, where it takes OSC, how it is asked, its
                name and roots - so a replay makes the same device. */
            if (job.discovering)
            {
                const auto& asked = job.discovery;
                const auto learned = discoverNow (asked, timeoutMs.load (std::memory_order_relaxed));

                if (engine != nullptr)
                    engine->submit ("mount:" + asked.mountId, "mount.discovered",
                                    { osc::Value::string (asked.mountId), osc::Value::string (fileFor (asked.mountId)),
                                      osc::Value::string (asked.host), osc::Value::string (learned.transport),
                                      osc::Value::int32 (learned.oscPort), osc::Value::int32 (asked.queryPort),
                                      osc::Value::string (learned.readback), osc::Value::string (learned.name),
                                      osc::Value::string (learned.roots),
                                      osc::Value::int32 (static_cast<std::int32_t> (learned.nodeCount)),
                                      osc::Value::string (learned.problem) });
                continue;
            }

            const auto& request = job.request;

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
