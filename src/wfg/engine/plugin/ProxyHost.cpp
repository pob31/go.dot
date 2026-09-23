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

#include <wfg/engine/plugin/ProxyHost.h>
#include <wfg/engine/plugin/ProcessUtil.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace wfg::plugin
{
    namespace
    {
        /*  THE CHILD, LAUNCHED WITHOUT INHERITING A HANDLE. JUCE's ChildProcess
            creates a Windows process with handle inheritance ON, and a socket
            is an inheritable handle: the first hosted serve on this machine
            handed its HTTP sockets to the plugin child, and every client
            waiting for the server to close a connection waited for a process
            that never would. So on Windows the child is created here, with
            inheritance off and no console window; on the other platforms
            JUCE's fork-and-exec is used, where the sockets carry
            close-on-exec. What the host needs of either is three things:
            whether it is running, its pid, and ending it. */
        struct ChildHandle
        {
            ChildHandle() = default;
            ~ChildHandle() { close(); }

            ChildHandle (const ChildHandle&) = delete;
            ChildHandle& operator= (const ChildHandle&) = delete;

            bool start (const juce::StringArray& command)
            {
               #if JUCE_WINDOWS
                juce::String line;

                for (const auto& word : command)
                {
                    if (line.isNotEmpty())
                        line += " ";

                    if (word.isEmpty() || word.containsAnyOf (" \t\""))
                        line += "\"" + word.replace ("\"", "\\\"") + "\"";
                    else
                        line += word;
                }

                std::vector<wchar_t> mutableLine (line.toWideCharPointer(),
                                                  line.toWideCharPointer() + line.length() + 1);

                STARTUPINFOW startup {};
                startup.cb = sizeof (startup);
                PROCESS_INFORMATION info {};

                if (! ::CreateProcessW (nullptr, mutableLine.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        nullptr, nullptr, &startup, &info))
                    return false;

                ::CloseHandle (info.hThread);
                handle = info.hProcess;
                pid = static_cast<std::int64_t> (info.dwProcessId);
                return true;
               #else
                process = std::make_unique<juce::ChildProcess>();

                if (! process->start (command, 0))
                {
                    process.reset();
                    return false;
                }

                pid = 0;
                return true;
               #endif
            }

            bool isRunning() const
            {
               #if JUCE_WINDOWS
                return handle != nullptr && ::WaitForSingleObject (handle, 0) == WAIT_TIMEOUT;
               #else
                return process != nullptr && process->isRunning();
               #endif
            }

            void kill()
            {
               #if JUCE_WINDOWS
                if (handle != nullptr)
                    ::TerminateProcess (handle, 1);
               #else
                if (process != nullptr)
                    process->kill();
               #endif
            }

            void close()
            {
               #if JUCE_WINDOWS
                if (handle != nullptr)
                {
                    ::CloseHandle (handle);
                    handle = nullptr;
                }
               #else
                process.reset();
               #endif
            }

            std::int64_t pid = 0;

           #if JUCE_WINDOWS
            HANDLE handle = nullptr;
           #else
            std::unique_ptr<juce::ChildProcess> process;
           #endif
        };
    }

    std::int64_t proxyDeadlineFor (int sampleRate, int blockSize, std::int64_t requestedMicroseconds) noexcept
    {
        if (requestedMicroseconds > 0)
            return requestedMicroseconds;

        if (sampleRate <= 0 || blockSize <= 0)
            return ProxyLane::defaultDeadlineMicroseconds;

        const auto blockPeriodUs = static_cast<std::int64_t> (blockSize) * 1000000 / sampleRate;
        return std::max<std::int64_t> (1, std::min<std::int64_t> (ProxyLane::defaultDeadlineMicroseconds,
                                                                  blockPeriodUs / 4));
    }

    //==============================================================================
    struct ProxyHost::Impl
    {
        enum class State { unloaded, loading, loaded, failed };

        Impl (ProxySpec specToUse, std::vector<ProxyLane*> lanesToUse, PluginTable* tableToUse)
            : spec (std::move (specToUse)), lanes (std::move (lanesToUse)), table (tableToUse)
        {
            deadlineUs = proxyDeadlineFor (spec.sampleRate, spec.maxSamples, spec.deadlineMicroseconds);
        }

        //======================================================================
        static const char* wordFor (State state) noexcept
        {
            switch (state)
            {
                case State::loading: return "loading";
                case State::loaded:  return "loaded";
                case State::failed:  return "failed";
                case State::unloaded: break;
            }

            return "unloaded";
        }

        PluginTable::Status statusNow() const
        {
            PluginTable::Status status;
            status.state = wordFor (state);
            status.problem = problem;
            status.latencySamples = latencySamples;
            status.paramCount = paramCount;
            return status;
        }

        /** Writes the table and tells the tree, when something a reader could see moved. */
        void publish()
        {
            if (table != nullptr && table->set (spec.pluginId, statusNow()) && changed)
                changed();
        }

        //======================================================================
        bool makeRegion (std::string& why)
        {
            const juce::File folder { juce::String (spec.regionFolder) };
            folder.createDirectory();

            /*  Named by the entry and by this process, so two engines on one
                machine - a show and a test - never share a file. */
            regionFile = folder.getChildFile ("proxy-" + juce::String (spec.pluginId) + "-"
                                              + juce::String (process::currentId()) + ".shm");

            const auto bytes = region::regionBytes (spec.channels, spec.maxSamples, spec.lanes);

            {
                juce::FileOutputStream out (regionFile);

                if (! out.openedOk())
                {
                    why = "could not create the shared region at " + regionFile.getFullPathName().toStdString();
                    return false;
                }

                out.setPosition (0);
                out.truncate();

                std::vector<char> zeros (std::min<std::size_t> (bytes, 1u << 20), 0);

                for (std::size_t written = 0; written < bytes;)
                {
                    const auto chunk = std::min (zeros.size(), bytes - written);
                    out.write (zeros.data(), chunk);
                    written += chunk;
                }
            }

            mapping = std::make_unique<juce::MemoryMappedFile> (regionFile, juce::MemoryMappedFile::readWrite, false);

            if (mapping->getData() == nullptr || mapping->getSize() < bytes)
            {
                why = "could not map the shared region at " + regionFile.getFullPathName().toStdString();
                mapping.reset();
                regionFile.deleteFile();
                return false;
            }

            /*  LAID OUT ONCE, HERE. The atomics are constructed in place over
                the zeroed file; the child never constructs, it reads. */
            auto* base = mapping->getData();
            header = new (base) region::Header {};
            header->magic.store (region::magic, std::memory_order_relaxed);
            header->version.store (region::version, std::memory_order_relaxed);
            header->channels.store (static_cast<std::uint32_t> (spec.channels), std::memory_order_relaxed);
            header->maxSamples.store (static_cast<std::uint32_t> (spec.maxSamples), std::memory_order_relaxed);
            header->lanes.store (static_cast<std::uint32_t> (spec.lanes), std::memory_order_relaxed);
            header->sampleRate.store (static_cast<std::uint32_t> (spec.sampleRate), std::memory_order_relaxed);
            header->blockSize.store (static_cast<std::uint32_t> (spec.maxSamples), std::memory_order_relaxed);
            header->layoutHash.store (region::layoutHashFor (spec.channels, spec.maxSamples, spec.lanes),
                                      std::memory_order_release);

            for (int i = 0; i < spec.lanes; ++i)
            {
                auto* lane = new (region::laneAt (base, spec.channels, spec.maxSamples, i)) region::Lane {};

                for (auto& value : lane->params)
                    value.store (region::useBaseline, std::memory_order_relaxed);
            }

            for (std::size_t i = 0; i < lanes.size() && i < static_cast<std::size_t> (spec.lanes); ++i)
            {
                if (lanes[i] == nullptr)
                    continue;

                auto* lane = region::laneAt (base, spec.channels, spec.maxSamples, static_cast<int> (i));
                lanes[i]->setDeadlineMicroseconds (deadlineUs);
                lanes[i]->setCallEnabled (false);
                lanes[i]->bind (header, lane, region::audioOf (lane), spec.channels, spec.maxSamples);
            }

            return true;
        }

        void clearChildFields()
        {
            if (header == nullptr)
                return;

            header->childShouldExit.store (0, std::memory_order_relaxed);
            header->childReady.store (0, std::memory_order_relaxed);
            header->childFailed.store (0, std::memory_order_relaxed);
            header->catalogueReady.store (0, std::memory_order_relaxed);
            std::memset (header->problem, 0, sizeof (header->problem));
        }

        bool launch (std::string& why)
        {
            endChild();
            clearChildFields();

            for (auto* lane : lanes)
                if (lane != nullptr)
                {
                    lane->setCallEnabled (false);
                    lane->clearMisses();
                }

            juce::StringArray command;
            command.add (spec.launch.executable.empty()
                             ? juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName()
                             : juce::String (spec.launch.executable));

            for (const auto& word : spec.launch.leadingArgs)
                command.add (juce::String (word));

            command.add ("--region=" + regionFile.getFullPathName());
            command.add ("--plugin=" + juce::String (spec.identifier));
            command.add ("--instances=" + juce::String (spec.lanes));
            command.add ("--channels=" + juce::String (spec.channels));
            command.add ("--parent-pid=" + juce::String (process::currentId()));

            if (! spec.presetPath.empty())
                command.add ("--preset=" + juce::String (spec.presetPath));

            /*  No pipes for its output: nothing reads them, and a full pipe
                is what hangs a child that prints. */
            child = std::make_unique<ChildHandle>();

            if (! child->start (command))
            {
                child.reset();
                why = "could not start the plugin host process " + command[0].toStdString();
                return false;
            }

            launchedAt = juce::Time::getMillisecondCounter();
            state = State::loading;
            problem.clear();
            publish();
            return true;
        }

        /** Tells it to leave, gives it a moment, and ends what is still there. */
        void endChild()
        {
            if (child == nullptr)
                return;

            if (header != nullptr)
                header->childShouldExit.store (1, std::memory_order_release);

            for (int waited = 0; waited < 40 && child->isRunning(); ++waited)
                juce::Thread::sleep (25);

            if (child->isRunning())
                child->kill();

            child.reset();
        }

        //======================================================================
        void fail (const std::string& sentence)
        {
            for (auto* lane : lanes)
                if (lane != nullptr)
                    lane->setCallEnabled (false);

            /*  A child that stopped answering is put down now, so the restart
                launches into a clean region rather than beside a hung twin -
                and put down at once, not asked: this runs on the message
                thread, and a second spent waiting politely on a process that
                is not answering would be a second of no arms and no window. */
            if (child != nullptr)
            {
                child->kill();
                child.reset();
            }

            const auto now = juce::Time::getMillisecondCounter();
            failures.erase (std::remove_if (failures.begin(), failures.end(),
                                            [now] (std::uint32_t at) { return now - at > static_cast<std::uint32_t> (failureWindowMs); }),
                            failures.end());
            failures.push_back (now);

            state = State::failed;
            problem = sentence;

            if (failures.size() == 1)
            {
                restartDueAt = std::max<std::uint32_t> (1, now + static_cast<std::uint32_t> (restartDelayMs));
                problem += "; every voice plays dry through it; restarting in two seconds";
            }
            else
            {
                restartDueAt = 0;
                problem += "; every voice plays dry through it; failed again inside a minute, so it stays"
                           " down until plugin.restart";
            }

            publish();

            if (failed)
                failed (spec.pluginId, problem);
        }

        void becomeLoaded()
        {
            latencySamples = static_cast<int> (header->latencySamples.load (std::memory_order_relaxed));
            paramCount = static_cast<int> (std::min<std::uint32_t> (header->paramCount.load (std::memory_order_relaxed),
                                                                    static_cast<std::uint32_t> (region::maxParams)));

            for (auto* lane : lanes)
                if (lane != nullptr)
                {
                    lane->clearMisses();
                    lane->setCallEnabled (true);
                }

            state = State::loaded;
            problem.clear();
            publish();
        }

        void poll()
        {
            if (header == nullptr)
                return;

            const auto now = juce::Time::getMillisecondCounter();

            if (state == State::loading)
            {
                if (header->childFailed.load (std::memory_order_acquire) != 0)
                {
                    header->problem[region::problemChars - 1] = 0;
                    const std::string why (header->problem);
                    fail (why.empty() ? "the plugin host could not bring the plugin up" : why);
                    return;
                }

                if (header->childReady.load (std::memory_order_acquire) != 0)
                {
                    becomeLoaded();
                    return;
                }

                if (child == nullptr || ! child->isRunning())
                {
                    fail ("the plugin host process died while loading");
                    return;
                }

                if (now - launchedAt > static_cast<std::uint32_t> (readyTimeoutMs))
                    fail ("the plugin host did not come up in five seconds");

                return;
            }

            if (state == State::loaded)
            {
                auto anyEnabled = false;

                for (std::size_t i = 0; i < lanes.size(); ++i)
                {
                    auto* lane = lanes[i];

                    if (lane == nullptr)
                        continue;

                    anyEnabled = anyEnabled || lane->isEnabled();

                    if (lane->consecutiveMisses() >= static_cast<std::uint32_t> (ProxyLane::missesBeforeFailure))
                    {
                        fail ("the plugin stopped answering: " + std::to_string (ProxyLane::missesBeforeFailure)
                              + " blocks late in a row on voice " + std::to_string (i + 1));
                        return;
                    }
                }

                header->wantSpin.store (anyEnabled ? 1u : 0u, std::memory_order_relaxed);

                if (child == nullptr || ! child->isRunning())
                    fail ("the plugin host process died");

                return;
            }

            if (state == State::failed && restartDueAt != 0 && now - restartDueAt < 0x7fffffffu)
            {
                restartDueAt = 0;
                std::string why;

                if (! launch (why))
                {
                    state = State::failed;
                    problem = why;
                    publish();
                }
            }
        }

        //======================================================================
        void stop()
        {
            endChild();

            for (auto* lane : lanes)
                if (lane != nullptr)
                {
                    lane->setCallEnabled (false);
                    lane->unbind();
                }

            header = nullptr;
            mapping.reset();
            regionFile.deleteFile();

            if (state != State::unloaded)
            {
                state = State::unloaded;
                problem.clear();
                publish();
            }
        }

        //======================================================================
        ProxySpec spec;
        std::vector<ProxyLane*> lanes;
        PluginTable* table = nullptr;
        FailureHandler failed;
        ChangeHandler changed;

        juce::File regionFile;
        std::unique_ptr<juce::MemoryMappedFile> mapping;
        region::Header* header = nullptr;
        std::unique_ptr<ChildHandle> child;

        State state = State::unloaded;
        std::string problem;
        int latencySamples = 0;
        int paramCount = 0;
        std::int64_t deadlineUs = ProxyLane::defaultDeadlineMicroseconds;

        std::uint32_t launchedAt = 0;
        std::uint32_t restartDueAt = 0;
        std::vector<std::uint32_t> failures;
    };

    //==============================================================================
    ProxyHost::ProxyHost (ProxySpec spec, std::vector<ProxyLane*> lanes, PluginTable* table)
        : impl (std::make_unique<Impl> (std::move (spec), std::move (lanes), table))
    {
    }

    ProxyHost::~ProxyHost()
    {
        stop();
    }

    void ProxyHost::onFailed (FailureHandler handler)   { impl->failed = std::move (handler); }
    void ProxyHost::onChanged (ChangeHandler handler)   { impl->changed = std::move (handler); }

    bool ProxyHost::start (std::string& problem)
    {
        problem.clear();

        if (impl->spec.lanes <= 0 || impl->spec.channels <= 0 || impl->spec.maxSamples <= 0)
        {
            problem = "a proxy needs at least one lane, one channel and a block size";
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        if (impl->header == nullptr && ! impl->makeRegion (problem))
        {
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        if (! impl->launch (problem))
        {
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        return true;
    }

    void ProxyHost::poll()
    {
        impl->poll();
    }

    bool ProxyHost::restart (std::string& problem)
    {
        problem.clear();

        if (impl->header == nullptr)
            return start (problem);

        /*  Asked for by name: the count of failures that gates the automatic
            restart starts again. */
        impl->failures.clear();
        impl->restartDueAt = 0;

        if (! impl->launch (problem))
        {
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        return true;
    }

    void ProxyHost::stop()
    {
        impl->stop();
    }

    //==============================================================================
    const std::string& ProxyHost::pluginId() const noexcept     { return impl->spec.pluginId; }
    PluginTable::Status ProxyHost::status() const               { return impl->statusNow(); }
    std::string ProxyHost::regionPath() const                   { return impl->regionFile.getFullPathName().toStdString(); }
    bool ProxyHost::childIsRunning() const                      { return impl->child != nullptr && impl->child->isRunning(); }
    std::int64_t ProxyHost::childPid() const noexcept           { return impl->child != nullptr ? impl->child->pid : 0; }
    std::int64_t ProxyHost::deadlineMicroseconds() const noexcept { return impl->deadlineUs; }
    region::Header* ProxyHost::header() noexcept                { return impl->header; }

    void ProxyHost::killChild()
    {
        if (impl->child != nullptr)
            impl->child->kill();
    }
}
