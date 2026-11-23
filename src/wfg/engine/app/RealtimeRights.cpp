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

#include <wfg/engine/app/RealtimeRights.h>

namespace wfg::app
{
    const char* wordOf (RealtimeBy by) noexcept
    {
        switch (by)
        {
            case RealtimeBy::none:    return "none";
            case RealtimeBy::system:  return "system";
            case RealtimeBy::rtkit:   return "rtkit";
            case RealtimeBy::pending: break;
        }

        return "pending";
    }

    std::string kernelThreadName (const std::string& name)
    {
        return name.substr (0, 15);
    }
}

//==============================================================================
//  LINUX: the user's limit, then RealtimeKit.
#if defined (__linux__)

 #include <wfg/engine/app/SystemBus_linux.h>

 #include <dirent.h>
 #include <sched.h>
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>

 #include <algorithm>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>

namespace wfg::app
{
    namespace
    {
        constexpr bus::Target rtkit { "org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1", "org.freedesktop.RealtimeKit1" };

        /*  SCHED_FIFO's range is 1..99. The clock takes the middle, which is
            what spatcore gave it and sits under the 88 PipeWire runs its own
            threads at; the sound ten above the clock, so a callback at work
            is never held off a core by a tick at work. */
        int systemPriority (RealtimeRank rank) noexcept
        {
            const auto lo = sched_get_priority_min (SCHED_FIFO);
            const auto hi = sched_get_priority_max (SCHED_FIFO);
            const auto mid = (lo >= 0 && hi >= lo) ? (lo + hi) / 2 : 1;
            return rank == RealtimeRank::sound ? std::min (hi, mid + 10) : mid;
        }

        bool bySystem (long threadId, RealtimeRank rank) noexcept
        {
            sched_param param {};
            param.sched_priority = systemPriority (rank);
            return sched_setscheduler (static_cast<pid_t> (threadId), SCHED_FIFO, &param) == 0;
        }

        /*  THE RUN-TIME BOUND RealtimeKit INSISTS ON, set no larger than the
            daemon allows and never loosened: a process that already bounds
            itself tighter keeps its bound. */
        void boundRealtimeRunTime (std::int64_t microseconds) noexcept
        {
            if (microseconds <= 0)
                return;

            rlimit now {};

            if (getrlimit (RLIMIT_RTTIME, &now) != 0)
                return;

            const auto wanted = static_cast<rlim_t> (microseconds);

            if (now.rlim_cur != RLIM_INFINITY && now.rlim_cur <= wanted)
                return;

            rlimit bound { wanted, wanted };

            if (now.rlim_max != RLIM_INFINITY && now.rlim_max < wanted)
                bound = { now.rlim_max, now.rlim_max };

            setrlimit (RLIMIT_RTTIME, &bound);
        }

        bool byRealtimeKit (long threadId, RealtimeRank rank) noexcept
        {
            const bus::Connection connection;

            if (! connection.open())
                return false;

            //  No daemon, no property: the one read that says whether to go on.
            const auto cap = bus::integerProperty (connection, rtkit, "MaxRealtimePriority", 1000);

            if (! cap.has_value() || *cap < 1)
                return false;

            boundRealtimeRunTime (bus::integerProperty (connection, rtkit, "RTTimeUSecMax", 1000).value_or (200000));

            //  The sound at the cap, the clock five under it, never below 1.
            const auto priority = static_cast<dbus_uint32_t> (rank == RealtimeRank::sound ? *cap : std::max<std::int64_t> (1, *cap - 5));
            const auto thread = static_cast<dbus_uint64_t> (threadId);

            auto* request = bus::methodCall (rtkit, "MakeThreadRealtime");

            if (request == nullptr)
                return false;

            dbus_message_append_args (request, DBUS_TYPE_UINT64, &thread, DBUS_TYPE_UINT32, &priority, DBUS_TYPE_INVALID);
            const auto reply = bus::call (connection, request, 2000);

            if (! reply.ok())
                return false;

            //  The daemon's word, and the kernel's: both must say so.
            const auto policy = sched_getscheduler (static_cast<pid_t> (threadId));
            return policy == SCHED_RR || policy == SCHED_FIFO;
        }
    }

    RealtimeBy takeRealtimeOf (long threadId, RealtimeRank rank) noexcept
    {
        if (threadId <= 0)
            return RealtimeBy::none;

        if (bySystem (threadId, rank))
            return RealtimeBy::system;

        if (byRealtimeKit (threadId, rank))
            return RealtimeBy::rtkit;

        return RealtimeBy::none;
    }

    RealtimeBy takeRealtime (RealtimeRank rank, double, double) noexcept
    {
        return takeRealtimeOf (static_cast<long> (syscall (SYS_gettid)), rank);
    }

    RealtimeBy takeRealtimeOfThreadNamed (const std::string& kernelName, RealtimeRank rank) noexcept
    {
        if (kernelName.empty())
            return RealtimeBy::none;

        auto* tasks = opendir ("/proc/self/task");

        if (tasks == nullptr)
            return RealtimeBy::none;

        long found = 0;

        while (const auto* entry = readdir (tasks))
        {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
                continue;

            const auto path = std::string ("/proc/self/task/") + entry->d_name + "/comm";

            if (auto* file = std::fopen (path.c_str(), "r"))
            {
                char comm[32] {};
                const auto read = std::fread (comm, 1, sizeof (comm) - 1, file);
                std::fclose (file);
                comm[read] = '\0';

                if (auto* newline = std::strchr (comm, '\n'))
                    *newline = '\0';

                if (kernelName == comm)
                {
                    found = std::strtol (entry->d_name, nullptr, 10);
                    break;
                }
            }
        }

        closedir (tasks);
        return found > 0 ? takeRealtimeOf (found, rank) : RealtimeBy::none;
    }
}

//==============================================================================
//  macOS AND WINDOWS: the thread's own rights always suffice; spatcore asks.
#else

 #include <spatcore/rt/RtThreadPriority.h>

namespace wfg::app
{
    RealtimeBy takeRealtime (RealtimeRank, double periodMs, double computationMs) noexcept
    {
        return spatcore::rt::setCurrentThreadAudioPriority (periodMs, computationMs) ? RealtimeBy::system : RealtimeBy::none;
    }

    RealtimeBy takeRealtimeOf (long, RealtimeRank) noexcept                           { return RealtimeBy::none; }
    RealtimeBy takeRealtimeOfThreadNamed (const std::string&, RealtimeRank) noexcept { return RealtimeBy::none; }
}

#endif

//==============================================================================
//  THE THREAD THAT NEVER SLEEPS, and where a thread is (namespace draft §148).
#if defined (_WIN32)

 #include <vector>

namespace wfg::app
{
    namespace
    {
        /*  Every core's efficiency class and the processors it holds, group
            nought only: a machine with more than sixty-four processors is no
            laptop, and has one kind. Empty when the system will not say. */
        struct CoreClass
        {
            KAFFINITY processors = 0;
            int efficiency = 0;
        };

        std::vector<CoreClass> coreClasses()
        {
            DWORD length = 0;
            ::GetLogicalProcessorInformationEx (RelationProcessorCore, nullptr, &length);

            if (length == 0)
                return {};

            std::vector<unsigned char> storage (length);

            if (! ::GetLogicalProcessorInformationEx (RelationProcessorCore,
                                                       reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX> (storage.data()),
                                                       &length))
                return {};

            std::vector<CoreClass> cores;

            //  The entries are as long as each says, not as long as the structure that names them all.
            for (DWORD at = 0; at + 2 * sizeof (DWORD) <= length;)
            {
                const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*> (storage.data() + at);

                if (info->Size == 0 || at + info->Size > length)
                    break;

                if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount > 0
                      && info->Processor.GroupMask[0].Group == 0)
                    cores.push_back ({ info->Processor.GroupMask[0].Mask, static_cast<int> (info->Processor.EfficiencyClass) });

                at += info->Size;
            }

            return cores;
        }

        int highestClass (const std::vector<CoreClass>& cores) noexcept
        {
            auto highest = 0;

            for (const auto& core : cores)
                highest = core.efficiency > highest ? core.efficiency : highest;

            return highest;
        }

        /*  AWAY FROM PROCESSOR NOUGHT (namespace draft §148): a wish, not a
            pin - the thread's ideal processor set to a performance core other
            than the first, one chosen by the process's number so two children
            wish for different ones. The system still runs the thread wherever
            it sees fit; this is where it looks first. */
        void wishAwayFromFirst() noexcept
        {
            const auto cores = coreClasses();
            const auto highest = highestClass (cores);
            std::vector<BYTE> wished;

            for (const auto& core : cores)
                if (core.efficiency == highest)
                    for (BYTE n = 1; n < 64; ++n)
                        if ((core.processors & (KAFFINITY { 1 } << n)) != 0)
                            wished.push_back (n);

            if (wished.empty())
                return;

            PROCESSOR_NUMBER ideal {};
            ideal.Group = 0;
            ideal.Number = wished[static_cast<std::size_t> (::GetCurrentProcessId() / 4u) % wished.size()];
            ::SetThreadIdealProcessorEx (::GetCurrentThread(), &ideal, nullptr);
            ::Sleep (1);        // off the processor once, so the wish is acted on before the work begins
        }
    }

    RealtimeBy takeRealtimeSpinning (SpinClass spinClass, double periodMs, double computationMs) noexcept
    {
        if (spinClass == SpinClass::multimedia)
            return takeRealtime (RealtimeRank::sound, periodMs, computationMs);

        const auto raised = ::SetThreadPriority (::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;

        if (spinClass == SpinClass::timeCriticalAway)
            wishAwayFromFirst();

        return raised ? RealtimeBy::system : RealtimeBy::none;
    }

    std::uint32_t processorNow() noexcept
    {
        return static_cast<std::uint32_t> (::GetCurrentProcessorNumber()) + 1u;
    }

    std::string coreKindOf (std::uint32_t processorPlusOne)
    {
        if (processorPlusOne == 0 || processorPlusOne > 64)
            return {};

        const auto cores = coreClasses();
        const auto highest = highestClass (cores);
        const auto bit = KAFFINITY { 1 } << (processorPlusOne - 1u);
        auto lowest = 255, own = -1;

        for (const auto& core : cores)
        {
            lowest = core.efficiency < lowest ? core.efficiency : lowest;

            if ((core.processors & bit) != 0)
                own = core.efficiency;
        }

        if (own < 0 || lowest >= highest)
            return {};

        return own == highest ? "performance" : "efficiency";
    }
}

#else

 #if defined (__linux__)
  #include <sched.h>
 #endif

namespace wfg::app
{
    RealtimeBy takeRealtimeSpinning (SpinClass, double periodMs, double computationMs) noexcept
    {
        return takeRealtime (RealtimeRank::sound, periodMs, computationMs);
    }

    std::uint32_t processorNow() noexcept
    {
       #if defined (__linux__)
        const auto processor = ::sched_getcpu();
        return processor < 0 ? 0u : static_cast<std::uint32_t> (processor) + 1u;
       #else
        return 0;
       #endif
    }

    std::string coreKindOf (std::uint32_t)
    {
        return {};
    }
}

#endif
