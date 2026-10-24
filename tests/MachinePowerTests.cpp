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

/*
    THIS COMPUTER'S POWER (namespace draft §108): what the system is told and
    what it says. The Mac and Windows declare and read; Linux (2026-10-24)
    declares over the system bus where the seat's policy lets it and reads
    sysfs, and each is checked for saying so truthfully. And the real-time
    class (app/RealtimeRights.h): asked for, and how it was got said.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/app/MachinePower.h>
#include <wfg/engine/app/RealtimeRights.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <string>
#include <thread>

#if defined (__linux__)
 #include <pthread.h>
 #include <sched.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

using namespace wfg;

TEST_CASE ("power: the process declares itself latency-critical where the system takes it, and reads what holds the computer back")
{
    const app::LatencyCritical critical { "Go.dot tests" };
    const auto power = app::readMachinePower();
    const std::string thermal = app::wordOf (power.thermal);
    const std::string cappedBy = app::wordOf (power.cappedBy);

    CHECK ((thermal == "nominal" || thermal == "fair" || thermal == "serious" || thermal == "critical"));

    //  lowPower and its reason agree, whatever the machine running this is doing.
    CHECK (power.lowPowerMode == (power.cappedBy != app::PowerCap::none));

   #if JUCE_MAC
    CHECK (critical.declared());
    CHECK ((cappedBy == "none" || cappedBy == "lowPowerMode"));
    CHECK_FALSE (power.onBattery);
   #elif JUCE_WINDOWS
    CHECK (critical.declared());
    CHECK (cappedBy != "lowPowerMode");
    CHECK (thermal == "nominal");
   #else
    /*  Linux: the declaration is power-profiles-daemon's hold and logind's
        inhibitor, both refused by polkit to a session that is not the
        seat's - the farm's SSH, a CI container - so declared() may be
        either, and is the same both times (checked below). What is read
        is Linux's own words only. */
    CHECK ((cappedBy == "none" || cappedBy == "powerSaver" || cappedBy == "processorLimited"));
   #endif

    //  The words, each its own, and the resting one for anything out of range.
    CHECK (std::string (app::wordOf (app::Thermal::nominal)) == "nominal");
    CHECK (std::string (app::wordOf (app::Thermal::fair)) == "fair");
    CHECK (std::string (app::wordOf (app::Thermal::serious)) == "serious");
    CHECK (std::string (app::wordOf (app::Thermal::critical)) == "critical");
    CHECK (std::string (app::wordOf (static_cast<app::Thermal> (9))) == "nominal");

    CHECK (std::string (app::wordOf (app::PowerCap::none)) == "none");
    CHECK (std::string (app::wordOf (app::PowerCap::lowPowerMode)) == "lowPowerMode");
    CHECK (std::string (app::wordOf (app::PowerCap::batterySaver)) == "batterySaver");
    CHECK (std::string (app::wordOf (app::PowerCap::bestPowerEfficiency)) == "bestPowerEfficiency");
    CHECK (std::string (app::wordOf (app::PowerCap::powerSaverPlan)) == "powerSaverPlan");
    CHECK (std::string (app::wordOf (app::PowerCap::processorLimited)) == "processorLimited");
    CHECK (std::string (app::wordOf (app::PowerCap::powerSaver)) == "powerSaver");
    CHECK (std::string (app::wordOf (static_cast<app::PowerCap> (9))) == "none");

    //  Declared twice over is two declarations, each ended on its own.
    {
        const app::LatencyCritical again { "Go.dot tests, again" };
        CHECK (again.declared() == critical.declared());
    }

    CHECK (app::readMachinePower() == power);
}

TEST_CASE ("real-time: a thread asks for the real-time class, says how it got it, and a thread is found by its name (app/RealtimeRights.h)")
{
    /*  The author, 2026-10-24, of Linux: what matters most there is not power
        but real-time rights, which a stock desktop user is refused - so the
        ask has a second route, RealtimeKit, and the answer says which took. */
    CHECK (std::string (app::wordOf (app::RealtimeBy::pending)) == "pending");
    CHECK (std::string (app::wordOf (app::RealtimeBy::none)) == "none");
    CHECK (std::string (app::wordOf (app::RealtimeBy::system)) == "system");
    CHECK (std::string (app::wordOf (app::RealtimeBy::rtkit)) == "rtkit");
    CHECK (std::string (app::wordOf (static_cast<app::RealtimeBy> (9))) == "pending");

    //  What the kernel keeps of a name: JUCE's ALSA thread is found by this.
    CHECK (app::kernelThreadName ("JUCE v8.0.13: ALSA") == "JUCE v8.0.13: A");
    CHECK (app::kernelThreadName ("short") == "short");

    std::atomic<int> got { static_cast<int> (app::RealtimeBy::pending) };
    std::thread asker ([&] { got.store (static_cast<int> (app::takeRealtime (app::RealtimeRank::clock, 20.0, 5.0))); });
    asker.join();
    const auto by = static_cast<app::RealtimeBy> (got.load());
    CHECK (by != app::RealtimeBy::pending);

   #if JUCE_MAC || JUCE_WINDOWS
    //  The thread's own rights always suffice; the other two routes are Linux's.
    CHECK (by == app::RealtimeBy::system);
    CHECK (app::takeRealtimeOf (1, app::RealtimeRank::sound) == app::RealtimeBy::none);
    CHECK (app::takeRealtimeOfThreadNamed ("anything", app::RealtimeRank::sound) == app::RealtimeBy::none);
   #else
    /*  The farm's user has the limit (system); a stock desktop has RealtimeKit
        (rtkit); CI's container has neither (none). Whichever, a thread found
        by its name gets the same, and the kernel agrees it took. */
    std::atomic<bool> release { false };
    std::atomic<long> threadId { 0 };
    std::thread named ([&]
    {
        pthread_setname_np (pthread_self(), "wfg rights test");
        threadId.store (static_cast<long> (syscall (SYS_gettid)));

        while (! release.load())
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    });

    while (threadId.load() == 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));

    const auto found = app::takeRealtimeOfThreadNamed ("wfg rights test", app::RealtimeRank::sound);
    CHECK (found == by);

    if (found != app::RealtimeBy::none)
    {
        const auto policy = sched_getscheduler (static_cast<pid_t> (threadId.load()));
        CHECK ((policy == SCHED_FIFO || policy == SCHED_RR));
    }

    CHECK (app::takeRealtimeOfThreadNamed ("no such thread", app::RealtimeRank::sound) == app::RealtimeBy::none);
    CHECK (app::takeRealtimeOf (0, app::RealtimeRank::sound) == app::RealtimeBy::none);

    release.store (true);
    named.join();
   #endif
}
