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

#include <wfg/engine/serial/SerialTable.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <deque>

namespace wfg::serial
{
    namespace
    {
        constexpr std::size_t maxLine = 4096;
        constexpr std::size_t maxWaiting = 512;
        constexpr auto firstPause = std::chrono::milliseconds (500);
        constexpr auto longestPause = std::chrono::milliseconds (8000);
        constexpr auto readWait = std::chrono::milliseconds (10);
    }

    struct SerialTable::Worker
    {
        Wanted wanted;
        Opener opener;
        std::atomic<bool> rx { true };
        std::atomic<bool> tx { true };
        std::atomic<bool> stopping { false };
        std::atomic<bool> finished { false };

        mutable std::mutex lock;
        std::condition_variable woken;
        std::deque<std::string> lines;      // read, waiting for the tick
        std::deque<std::string> outgoing;   // handed, waiting for the port
        PortState state;
        bool open = false;
        std::string partial;

        std::thread thread;

        void run()
        {
            auto pause = firstPause;
            std::unique_ptr<Link> link;

            while (! stopping)
            {
                if (link == nullptr)
                {
                    setState ("opening", {});
                    std::string problem;
                    link = opener (wanted.path, wanted.baud, problem);
                    if (link == nullptr)
                    {
                        setState ("retrying", problem.empty() ? std::string ("it would not open") : problem);
                        waitFor (pause);
                        pause = std::min (pause * 2, longestPause);
                        continue;
                    }
                    pause = firstPause;
                    {
                        const std::lock_guard<std::mutex> held (lock);
                        partial.clear();
                        outgoing.clear();
                        open = true;
                    }
                    setState ("open", {});
                }

                const auto got = link->read (readWait);
                bool failed = ! got.has_value();

                if (got.has_value() && ! got->empty() && rx)
                    heard (*got);

                if (! failed)
                {
                    std::deque<std::string> toSend;
                    {
                        const std::lock_guard<std::mutex> held (lock);
                        toSend.swap (outgoing);
                    }
                    for (const auto& line : toSend)
                        if (! link->write (line))
                        {
                            failed = true;
                            break;
                        }
                }

                if (failed)
                {
                    const auto why = link->problem();
                    link.reset();
                    {
                        const std::lock_guard<std::mutex> held (lock);
                        open = false;
                    }
                    setState ("retrying", why.empty() ? std::string ("the port went away") : why);
                    waitFor (pause);
                    pause = std::min (pause * 2, longestPause);
                }
            }

            link.reset();
            {
                const std::lock_guard<std::mutex> held (lock);
                open = false;
            }
            setState ("closed", {});
            finished = true;
        }

        void heard (const std::string& bytes)
        {
            const std::lock_guard<std::mutex> held (lock);
            for (const char c : bytes)
            {
                if (c == '\n' || partial.size() >= maxLine)
                {
                    if (! partial.empty() && partial.back() == '\r')
                        partial.pop_back();
                    if (lines.size() < maxWaiting)
                        lines.push_back (partial);
                    else
                        ++state.dropped;
                    partial.clear();
                    if (c == '\n')
                        continue;
                }
                partial.push_back (c);
            }
        }

        void setState (const char* now, std::string problem)
        {
            const std::lock_guard<std::mutex> held (lock);
            state.state = now;
            state.problem = std::move (problem);
        }

        void waitFor (std::chrono::milliseconds pause)
        {
            std::unique_lock<std::mutex> held (lock);
            woken.wait_for (held, pause, [this] { return stopping.load(); });
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

    SerialTable::SerialTable (Opener openerToUse) : opener (std::move (openerToUse)) {}

    SerialTable::~SerialTable()
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

    void SerialTable::retire (std::shared_ptr<Worker> worker)
    {
        worker->stop();
        retired.push_back (std::move (worker));
    }

    void SerialTable::reap()
    {
        for (auto at = retired.begin(); at != retired.end();)
        {
            if ((*at)->finished)
            {
                if ((*at)->thread.joinable())
                    (*at)->thread.join();
                at = retired.erase (at);
            }
            else
            {
                ++at;
            }
        }
    }

    void SerialTable::reconcile (const std::vector<Wanted>& wanted)
    {
        const std::lock_guard<std::mutex> held (lock);
        reap();

        for (auto at = workers.begin(); at != workers.end();)
        {
            const auto found = std::find_if (wanted.begin(), wanted.end(),
                                             [&at] (const Wanted& w) { return w.id == at->first; });
            const auto& was = at->second->wanted;
            if (found == wanted.end() || found->path.empty() || found->path != was.path
                  || found->baud != was.baud || found->framing != was.framing)
            {
                retire (at->second);
                at = workers.erase (at);
            }
            else
            {
                at->second->rx = found->rx;
                at->second->tx = found->tx;
                ++at;
            }
        }

        for (const auto& w : wanted)
        {
            if (w.path.empty() || workers.count (w.id) != 0)
                continue;

            auto worker = std::make_shared<Worker>();
            worker->wanted = w;
            worker->opener = opener;
            worker->rx = w.rx;
            worker->tx = w.tx;
            worker->thread = std::thread ([raw = worker.get()] { raw->run(); });
            workers.emplace (w.id, std::move (worker));
        }
    }

    std::vector<std::pair<std::string, std::vector<std::string>>> SerialTable::takeLines (std::size_t perPort)
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> out;
        const std::lock_guard<std::mutex> held (lock);
        for (auto& [id, worker] : workers)
        {
            std::vector<std::string> taken;
            {
                const std::lock_guard<std::mutex> workerHeld (worker->lock);
                while (! worker->lines.empty() && taken.size() < perPort)
                {
                    taken.push_back (std::move (worker->lines.front()));
                    worker->lines.pop_front();
                }
            }
            if (! taken.empty())
                out.emplace_back (id, std::move (taken));
        }
        return out;
    }

    bool SerialTable::send (const std::string& id, const std::string& line)
    {
        const std::lock_guard<std::mutex> held (lock);
        const auto found = workers.find (id);
        if (found == workers.end() || ! found->second->tx)
            return false;

        auto& worker = *found->second;
        const std::lock_guard<std::mutex> workerHeld (worker.lock);
        if (! worker.open)
            return false;
        if (worker.outgoing.size() >= maxWaiting)
        {
            ++worker.state.dropped;
            return false;
        }
        worker.outgoing.push_back (line + "\n");
        return true;
    }

    PortState SerialTable::stateOf (const std::string& id) const
    {
        const std::lock_guard<std::mutex> held (lock);
        const auto found = workers.find (id);
        if (found == workers.end())
            return {};
        const std::lock_guard<std::mutex> workerHeld (found->second->lock);
        return found->second->state;
    }

    //==========================================================================
    void HeardLines::note (const std::string& id, const std::string& line, std::int64_t tick)
    {
        heard.push_back ({ id, line, tick });
        last[id] = line;
        if (heard.size() > 4096)
            heard.erase (heard.begin(), heard.begin() + static_cast<std::ptrdiff_t> (heard.size() - 4096));
    }

    std::vector<std::pair<std::string, std::string>> HeardLines::after (std::int64_t tick) const
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& h : heard)
            if (h.tick > tick)
                out.emplace_back (h.id, h.line);
        return out;
    }

    std::string HeardLines::lastLine (const std::string& id) const
    {
        const auto found = last.find (id);
        return found == last.end() ? std::string {} : found->second;
    }

    void HeardLines::forgetBefore (std::int64_t tick)
    {
        heard.erase (std::remove_if (heard.begin(), heard.end(), [tick] (const Heard& h) { return h.tick < tick; }),
                     heard.end());
    }

    std::vector<Word> wordsOfLine (const std::string& line)
    {
        std::vector<Word> out;
        std::string word;
        const auto flush = [&]
        {
            if (word.empty())
                return;
            Word w;
            w.text = word;
            if (const auto number = osc::parseDouble (word))
            {
                w.isNumber = true;
                w.number = *number;
            }
            out.push_back (std::move (w));
            word.clear();
        };
        for (const char c : line)
        {
            if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == ';')
                flush();
            else
                word.push_back (c);
        }
        flush();
        return out;
    }
}
