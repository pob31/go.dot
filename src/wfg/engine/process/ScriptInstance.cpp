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

#include <wfg/engine/process/ScriptInstance.h>
#include <wfg/engine/process/PatchThread.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <utility>

namespace wfg::process
{
    //==========================================================================
    ScriptOutput::ScriptOutput (std::size_t maxSentToUse, std::size_t maxPrintedToUse)
        : maxSent (maxSentToUse), maxPrinted (maxPrintedToUse)
    {
    }

    void ScriptOutput::send (Sent sent)
    {
        if (outbox.sent.size() >= maxSent)
        {
            ++outbox.dropped;
            return;
        }
        outbox.sent.push_back (std::move (sent));
    }

    void ScriptOutput::print (std::string line)
    {
        if (outbox.printed.size() < maxPrinted)
            outbox.printed.push_back (std::move (line));
    }

    Outbox ScriptOutput::take()
    {
        return std::exchange (outbox, Outbox {});
    }

    //==========================================================================
    namespace
    {
        enum Job : int { jobNone, jobMake, jobOpen, jobTick, jobClose };
    }

    struct ScriptInstance::Shared
    {
        Shared (MakeScriptLanguage makeToUse, Settings settingsToUse)
            : makeLanguage (std::move (makeToUse)),
              output (settingsToUse.maxSent, settingsToUse.maxPrinted)
        {
        }

        MakeScriptLanguage makeLanguage;

        std::mutex lock;
        std::condition_variable wake;
        std::condition_variable done;
        int job = jobNone;
        std::vector<Input> inputs;
        std::string text;
        std::uint64_t posted = 0;
        std::uint64_t finished = 0;
        bool running = false;
        bool quit = false;
        bool abandoned = false;
        bool leaveUnfreed = false;

        // Written on the thread while a job runs; the owner takes it only when
        // no job is running.
        ScriptOutput output;

        struct Facts
        {
            bool made = false;
            bool opened = false;
            std::string problem;
            std::vector<std::string> receives;
        };
        Facts work;
        Facts facts;

        // The interpreter: made, used and freed on the thread; `interrupt`
        // reaches it from another, under `languageLock`, which the thread
        // takes only to put one in or take one out.
        std::mutex languageLock;
        std::unique_ptr<ScriptLanguage> language;

        void replaceLanguage (std::unique_ptr<ScriptLanguage> next)
        {
            std::unique_ptr<ScriptLanguage> old;
            {
                const std::lock_guard<std::mutex> l (languageLock);
                old = std::exchange (language, std::move (next));
            }
            // Freed outside the lock, on this thread.
            old.reset();
        }
    };

    namespace
    {
        bool blank (const std::string& text)
        {
            for (const auto c : text)
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    return false;
            return true;
        }

        void runOpen (ScriptInstance::Shared& s, const std::string& source)
        {
            s.replaceLanguage (nullptr);
            s.work.opened = false;
            s.work.problem.clear();
            s.work.receives.clear();

            //  A script with nothing in it does nothing: nothing to open.
            if (blank (source))
                return;

            s.replaceLanguage (s.makeLanguage (s.output));
            if (s.language == nullptr)
            {
                s.work.problem = "the script's language could not be started";
                return;
            }

            std::string problem;
            s.work.opened = s.language->load (source, problem);
            s.work.problem = std::move (problem);
            s.work.receives = s.language->receives();

            //  A script that fails at its top level is not run: what it had
            //  said it hears is forgotten with it.
            if (! s.work.opened)
            {
                s.work.receives.clear();
                s.replaceLanguage (nullptr);
            }
        }

        void runTick (ScriptInstance::Shared& s, const std::vector<Input>& inputs)
        {
            if (s.language == nullptr)
                return;
            s.language->tick (inputs);
            s.work.receives = s.language->receives();
        }

        void threadMain (std::shared_ptr<ScriptInstance::Shared> s)
        {
            numbersInCOnThisThread();

            std::unique_lock<std::mutex> l (s->lock);
            for (;;)
            {
                s->wake.wait (l, [&] { return s->job != jobNone || s->quit; });
                if (s->job == jobNone)
                    break;

                const int job = s->job;
                auto inputs = std::move (s->inputs);
                auto text = std::move (s->text);
                s->job = jobNone;
                s->running = true;
                l.unlock();

                if (job == jobMake)
                {
                    s->work.made = true;
                }
                else if (job == jobOpen)
                {
                    runOpen (*s, text);
                }
                else if (job == jobTick)
                {
                    runTick (*s, inputs);
                }
                else if (job == jobClose)
                {
                    s->replaceLanguage (nullptr);
                    s->work = {};
                }

                l.lock();
                s->facts = s->work;
                s->running = false;
                ++s->finished;
                s->done.notify_all();
                if (s->abandoned)
                    break;
            }

            //  Left by its owner: what is left of the interpreter is freed here,
            //  on its own thread, before the thread ends.
            l.unlock();
            s->replaceLanguage (nullptr);
        }
    }

    //==========================================================================
    ScriptInstance::ScriptInstance (MakeScriptLanguage makeLanguage, Settings settings)
        : shared (std::make_shared<Shared> (std::move (makeLanguage), settings))
    {
        /*  On a thread with a patch's stack (PatchThread.h): QuickJS and Lua
            each guard against a recursion that runs away, and stop it with an
            error long before eight megabytes. */
        if (! startPatchThread ([s = shared] { threadMain (s); }))
            throw std::system_error (std::make_error_code (std::errc::resource_unavailable_try_again),
                                     "a script's thread could not be started");
    }

    ScriptInstance::~ScriptInstance()
    {
        std::unique_lock<std::mutex> l (shared->lock);
        const bool idle = ! shared->running && shared->job == jobNone;
        if (idle && shared->facts.made && ! shared->leaveUnfreed)
        {
            shared->job = jobClose;
            ++shared->posted;
            shared->wake.notify_all();
            shared->done.wait (l, [&] { return shared->finished == shared->posted; });
        }
        if (! idle)
        {
            shared->abandoned = true;
            l.unlock();
            interrupt();
            l.lock();
        }
        shared->quit = true;
        shared->wake.notify_all();
    }

    bool ScriptInstance::post (int kind, std::vector<Input> inputs, std::string text)
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        if (shared->running || shared->job != jobNone)
            return false;
        if (kind == jobMake && shared->facts.made)
            return false;
        if (kind != jobMake && ! shared->facts.made)
            return false;
        if (kind == jobTick && ! shared->facts.opened)
            return false;
        shared->job = kind;
        shared->inputs = std::move (inputs);
        shared->text = std::move (text);
        ++shared->posted;
        shared->wake.notify_all();
        return true;
    }

    bool ScriptInstance::make() { return post (jobMake, {}, {}); }
    bool ScriptInstance::open (std::string source) { return post (jobOpen, {}, std::move (source)); }
    bool ScriptInstance::tick (std::vector<Input> inputs) { return post (jobTick, std::move (inputs), {}); }
    bool ScriptInstance::close() { return post (jobClose, {}, {}); }

    void ScriptInstance::leaveUnfreed()
    {
        {
            const std::lock_guard<std::mutex> l (shared->lock);
            shared->leaveUnfreed = true;
        }
        interrupt();
    }

    bool ScriptInstance::interrupt()
    {
        const std::lock_guard<std::mutex> l (shared->languageLock);
        if (shared->language != nullptr)
            shared->language->interrupt();
        return true;
    }

    bool ScriptInstance::finished (std::chrono::microseconds limit)
    {
        std::unique_lock<std::mutex> l (shared->lock);
        return shared->done.wait_for (l, limit, [&] { return shared->finished == shared->posted; });
    }

    bool ScriptInstance::busy() const
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        return shared->running || shared->job != jobNone;
    }

    Outbox ScriptInstance::takeOutbox()
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        if (shared->running || shared->job != jobNone)
            return {};
        return shared->output.take();
    }

    bool ScriptInstance::made() const
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.made;
    }

    bool ScriptInstance::opened() const
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.opened;
    }

    std::string ScriptInstance::problem() const
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.problem;
    }

    std::vector<std::string> ScriptInstance::sends() const
    {
        return {};
    }

    std::vector<std::string> ScriptInstance::receives() const
    {
        const std::lock_guard<std::mutex> l (shared->lock);
        return shared->facts.receives;
    }
}
