// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <wfg/engine/net/SscV2.h>

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/json/JsonWrite.h>
#include <wfg/engine/net/Http.h>
#include <wfg/engine/net/TlsStream.h>
#include <wfg/engine/pair/Hold.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

namespace wfg::net
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int connectMs = 3000;
        constexpr int answerMs = 5000;
        constexpr std::size_t maxWaiting = 512;
        constexpr auto firstPause = std::chrono::milliseconds (500);
        constexpr auto longestPause = std::chrono::milliseconds (8000);

        /*  A stream that has said nothing for this long is taken for dead:
            a base station that went away without closing leaves a socket
            no read will ever end. */
        constexpr auto silence = std::chrono::seconds (60);

        const char* const subscriptions = "/api/ssc/state/subscriptions";

        bool sameWish (const SscV2Wanted& a, const SscV2Wanted& b)
        {
            return a.host == b.host && a.port == b.port && a.user == b.user && a.password == b.password
                && normalisedFingerprint (a.pin) == normalisedFingerprint (b.pin) && a.rx == b.rx && a.tx == b.tx
                && a.subscribe == b.subscribe;
        }

        /*  The rest of a response once its head is in: by its length, its
            chunks, or until the far end closes. */
        bool readBody (TlsStream& tls, std::string& buffer, const ResponseHead& head, std::string& body, std::string& problem)
        {
            const auto deadline = Clock::now() + std::chrono::milliseconds (answerMs);
            ChunkDecoder chunks;

            if (head.chunked)
                body = chunks.feed (buffer);
            else
                body = buffer;
            buffer.clear();

            const auto done = [&]
            {
                if (head.chunked)
                    return chunks.finished || ! chunks.problem.empty();
                if (head.contentLength >= 0)
                    return static_cast<long long> (body.size()) >= head.contentLength;
                return head.status == 204 || head.status == 304;
            };

            while (! done())
            {
                if (Clock::now() > deadline)
                {
                    problem = "the answer took too long";
                    return false;
                }

                std::string got;
                const auto n = tls.read (got, 50);

                if (n < 0)
                {
                    //  No length and no chunks: the close is the body's end.
                    if (! head.chunked && head.contentLength < 0)
                        return true;
                    problem = tls.problem();
                    return false;
                }

                body += head.chunked ? chunks.feed (got) : got;
            }

            if (head.contentLength >= 0 && static_cast<long long> (body.size()) > head.contentLength)
            {
                buffer = body.substr (static_cast<std::size_t> (head.contentLength));
                body.resize (static_cast<std::size_t> (head.contentLength));
            }

            if (! chunks.problem.empty())
            {
                problem = chunks.problem;
                return false;
            }

            return true;
        }

        std::optional<ResponseHead> readHead (TlsStream& tls, std::string& buffer, std::string& problem)
        {
            const auto deadline = Clock::now() + std::chrono::milliseconds (answerMs);

            for (;;)
            {
                if (auto head = takeHead (buffer, problem))
                    return head;

                if (! problem.empty())
                    return std::nullopt;

                if (Clock::now() > deadline)
                {
                    problem = "no answer came";
                    return std::nullopt;
                }

                if (tls.read (buffer, 50) < 0)
                {
                    problem = tls.problem();
                    return std::nullopt;
                }
            }
        }
    }

    struct SscV2Table::Worker
    {
        SscV2Wanted wanted;
        std::atomic<bool> stopping { false };
        std::atomic<bool> finished { false };

        mutable std::mutex lock;
        std::condition_variable woken;
        std::deque<std::string> outgoing;
        std::deque<SscV2Heard> heard;
        SscV2State state;
        std::thread thread;

        //  The worker's own, never touched by the tick.
        std::unique_ptr<TlsStream> stream;
        std::string streamBuffer;
        bool streamChunked = false;
        ChunkDecoder chunks;
        EventParser events;
        Clock::time_point lastHeard;
        std::unique_ptr<TlsStream> requests;
        std::string requestBuffer;
        std::deque<std::string> internal;
        bool closed = false;

        void setState (const char* now, std::string problem)
        {
            const std::lock_guard<std::mutex> held (lock);
            state.state = now;
            state.problem = std::move (problem);
        }

        void keep (std::string kind, std::string path, std::string text)
        {
            const std::lock_guard<std::mutex> held (lock);
            if (heard.size() < maxWaiting)
                heard.push_back ({ wanted.id, std::move (kind), std::move (path), std::move (text) });
        }

        void waitFor (std::chrono::milliseconds pause)
        {
            std::unique_lock<std::mutex> held (lock);
            woken.wait_for (held, pause, [this] { return stopping.load() || ! outgoing.empty(); });
        }

        /*  A CONNECTION, TRUSTED OR NOT AT ALL: the handshake, then the
            fingerprint against the pin - before anything is written. */
        std::unique_ptr<TlsStream> openTrusted (std::string& problem)
        {
            auto tls = std::make_unique<TlsStream>();

            if (! tls->open (wanted.host, wanted.port, connectMs, problem))
                return nullptr;

            {
                const std::lock_guard<std::mutex> held (lock);
                state.certificate = tls->fingerprint();
            }

            if (wanted.pin.empty())
            {
                problem = "not trusted yet: its certificate's SHA-256 is " + tls->fingerprint()
                        + " - trust it to go on";
                return nullptr;
            }

            if (normalisedFingerprint (wanted.pin) != normalisedFingerprint (tls->fingerprint()))
            {
                problem = "the certificate is not the one trusted: it is " + tls->fingerprint()
                        + " - another box, or this one reset";
                return nullptr;
            }

            return tls;
        }

        std::string requestText (const std::string& method, const std::string& path, const std::string& body,
                                 const std::string& accept) const
        {
            return requestOf (method, wanted.host, path, wanted.user, wanted.password, accept, body);
        }

        bool openStream (std::string& problem)
        {
            stream = openTrusted (problem);
            if (stream == nullptr)
                return false;

            streamBuffer.clear();

            if (! stream->write (requestText ("GET", subscriptions, {}, "text/event-stream")))
            {
                problem = stream->problem();
                stream.reset();
                return false;
            }

            const auto head = readHead (*stream, streamBuffer, problem);

            if (! head.has_value() || head->status != 200)
            {
                if (head.has_value())
                    problem = head->status == 401 ? std::string ("the device refused the login (HTTP 401)")
                                                  : "the subscription was refused: HTTP " + std::to_string (head->status)
                                                    + " " + head->reason;
                stream.reset();
                return false;
            }

            streamChunked = head->chunked;
            closed = false;
            chunks = ChunkDecoder {};
            events = EventParser {};
            lastHeard = Clock::now();
            hearStream (streamBuffer);
            streamBuffer.clear();
            return true;
        }

        void hearStream (const std::string& bytes)
        {
            const auto text = streamChunked ? chunks.feed (bytes) : bytes;

            for (const auto& event : events.feed (text))
            {
                if (event.name == "open")
                {
                    /*  THE SESSION, named: what to watch is added to it, and the
                        base station answers each with its values as they are
                        (SSCv2 2.3: "MUST report the initial values of the newly
                        added resources"). */
                    const auto parsed = json::parse (event.data);
                    const auto* uuid = parsed.ok() ? parsed.value->find ("sessionUUID") : nullptr;

                    if (uuid == nullptr || ! uuid->isString())
                        continue;

                    json::Array paths;
                    for (const auto& path : wanted.subscribe)
                        paths.emplace_back (path);

                    internal.push_back (std::string ("PUT ") + subscriptions + "/" + uuid->asString() + "/add\n"
                                        + json::write (json::Value (std::move (paths))));
                    continue;
                }

                //  The base station ending the session: opened again.
                if (event.name == "close")
                {
                    closed = true;
                    continue;
                }

                if (! event.data.empty())
                    keep ("event", {}, event.data);
            }
        }

        /*  ONE REQUEST, on the requests' own connection: the answer heard as
            a reply to its path, or said as an error. */
        void send (const std::string& rendered)
        {
            const auto newline = rendered.find ('\n');
            const auto line = rendered.substr (0, newline);
            const auto body = newline == std::string::npos ? std::string {} : rendered.substr (newline + 1);
            const auto space = line.find (' ');

            if (space == std::string::npos)
                return;

            const auto method = line.substr (0, space);
            const auto path = line.substr (space + 1);

            for (int attempt = 0; attempt < 2; ++attempt)
            {
                std::string problem;

                if (requests == nullptr)
                {
                    requests = openTrusted (problem);
                    requestBuffer.clear();

                    if (requests == nullptr)
                    {
                        keep ("error", path, problem);
                        return;
                    }
                }

                if (! requests->write (requestText (method, path, body, {})))
                {
                    requests.reset();
                    continue;
                }

                const auto head = readHead (*requests, requestBuffer, problem);
                std::string answer;

                if (! head.has_value() || ! readBody (*requests, requestBuffer, *head, answer, problem))
                {
                    requests.reset();

                    //  A kept-alive connection the far end had dropped: once more on a new one.
                    if (attempt == 0 && ! head.has_value())
                        continue;

                    keep ("error", path, problem);
                    return;
                }

                if (head->closes)
                    requests.reset();

                if (head->status >= 200 && head->status < 300)
                {
                    if (! answer.empty())
                        keep ("reply", path, answer);
                }
                else
                {
                    keep ("error", path, "HTTP " + std::to_string (head->status) + " " + head->reason
                                       + (answer.empty() ? std::string {} : ": " + answer.substr (0, 200)));
                }

                return;
            }

            keep ("error", path, "the device's connection went away");
        }

        void run()
        {
            auto pause = firstPause;

            while (! stopping)
            {
                if (wanted.rx && stream == nullptr)
                {
                    setState ("opening", {});
                    std::string problem;

                    if (! openStream (problem))
                    {
                        setState ("retrying", problem);
                        waitFor (pause);
                        pause = std::min (pause * 2, longestPause);
                        continue;
                    }

                    pause = firstPause;
                    setState ("open", {});
                }
                else if (! wanted.rx)
                {
                    const std::lock_guard<std::mutex> held (lock);
                    if (state.state == "closed")
                        state.state = "open";
                }

                if (stream != nullptr)
                {
                    std::string got;
                    const auto n = stream->read (got, 20);

                    if (n < 0 || closed || Clock::now() - lastHeard > silence)
                    {
                        setState ("retrying", n < 0 ? stream->problem()
                                                    : closed ? std::string ("the device closed the subscription")
                                                             : std::string ("the device went silent"));
                        stream.reset();
                        requests.reset();
                        waitFor (pause);
                        continue;
                    }

                    if (n > 0)
                    {
                        lastHeard = Clock::now();
                        hearStream (got);
                    }
                }

                std::string next;

                if (! internal.empty())
                {
                    next = std::move (internal.front());
                    internal.pop_front();
                }
                else
                {
                    const std::lock_guard<std::mutex> held (lock);
                    if (! outgoing.empty())
                    {
                        next = std::move (outgoing.front());
                        outgoing.pop_front();
                    }
                }

                if (! next.empty())
                    send (next);
                else if (stream == nullptr)
                    waitFor (std::chrono::milliseconds (50));
            }

            stream.reset();
            requests.reset();
            setState ("closed", {});
            finished = true;
        }

        void stop()
        {
            {
                const std::lock_guard<std::mutex> held (lock);
                stopping = true;
            }
            woken.notify_all();
        }
    };

    SscV2Table::SscV2Table() = default;

    SscV2Table::~SscV2Table()
    {
        for (auto& [id, worker] : workers)
            worker->stop();
        for (auto& worker : retired)
            worker->stop();
        for (auto& [id, worker] : workers)
            if (worker->thread.joinable())
                worker->thread.join();
        for (auto& worker : retired)
            if (worker->thread.joinable())
                worker->thread.join();
    }

    void SscV2Table::reap()
    {
        retired.erase (std::remove_if (retired.begin(), retired.end(), [] (const std::shared_ptr<Worker>& worker)
                                       {
                                           if (! worker->finished)
                                               return false;
                                           if (worker->thread.joinable())
                                               worker->thread.join();
                                           return true;
                                       }),
                       retired.end());
    }

    void SscV2Table::reconcile (const std::vector<SscV2Wanted>& wanted)
    {
        const std::lock_guard<std::mutex> held (lock);
        reap();

        std::map<std::string, const SscV2Wanted*> byId;
        for (const auto& wish : wanted)
            if (! wish.host.empty() && wish.port > 0)
                byId[wish.id] = &wish;

        for (auto it = workers.begin(); it != workers.end();)
        {
            const auto found = byId.find (it->first);

            if (found == byId.end() || ! sameWish (it->second->wanted, *found->second))
            {
                it->second->stop();
                retired.push_back (it->second);
                it = workers.erase (it);
            }
            else
            {
                ++it;
            }
        }

        for (const auto& [id, wish] : byId)
        {
            if (workers.count (id) != 0)
                continue;

            auto worker = std::make_shared<Worker>();
            worker->wanted = *wish;
            auto* raw = worker.get();
            worker->thread = std::thread ([raw] { raw->run(); });
            workers[id] = std::move (worker);
        }
    }

    bool SscV2Table::request (const std::string& id, const std::string& rendered)
    {
        if (pair::isHeld (hold))
            return true;

        const std::lock_guard<std::mutex> held (lock);
        const auto found = workers.find (id);

        if (found == workers.end() || ! found->second->wanted.tx)
            return false;

        auto& worker = *found->second;
        {
            const std::lock_guard<std::mutex> workerHeld (worker.lock);
            if (worker.outgoing.size() >= maxWaiting)
                return false;
            worker.outgoing.push_back (rendered);
        }
        worker.woken.notify_all();
        return true;
    }

    std::vector<SscV2Heard> SscV2Table::take (std::size_t perDevice)
    {
        std::vector<SscV2Heard> out;
        const std::lock_guard<std::mutex> held (lock);

        for (auto& [id, worker] : workers)
        {
            const std::lock_guard<std::mutex> workerHeld (worker->lock);
            for (std::size_t n = 0; n < perDevice && ! worker->heard.empty(); ++n)
            {
                out.push_back (std::move (worker->heard.front()));
                worker->heard.pop_front();
            }
        }

        return out;
    }

    SscV2State SscV2Table::stateOf (const std::string& id) const
    {
        const std::lock_guard<std::mutex> held (lock);
        const auto found = workers.find (id);

        if (found == workers.end())
            return {};

        const std::lock_guard<std::mutex> workerHeld (found->second->lock);
        return found->second->state;
    }
}
