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

#include <wfg/engine/surface/SpaceMouse.h>

#include <juce_core/juce_core.h>

#include <hidapi/hidapi/hidapi.h>

/*  The process list, for 3Dconnexion's driver: Windows' own, after JUCE, in this
    one translation unit. */
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <tlhelp32.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>

namespace wfg::surface
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int readTimeoutMs = 20;
        constexpr auto aliveFor = std::chrono::milliseconds { 100 };
        constexpr auto searchEvery = std::chrono::seconds { 1 };

        std::string narrowed (const wchar_t* text)
        {
            if (text == nullptr)
                return {};

            return juce::String (text).toStdString();
        }

       #if JUCE_WINDOWS
        /*  spatcore's list: the service, the cores, SmartFocus and the
            configuration program, any of which may hold the puck. */
        juce::StringArray driverProcesses()
        {
            const char* names[] { "3DxService.exe", "3DxWinCore64.exe", "3DxWinCore.exe",
                                  "3DxSmartFocus.exe", "3Dconnexion.exe" };
            juce::StringArray found;

            const auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

            if (snapshot == INVALID_HANDLE_VALUE)
                return found;

            PROCESSENTRY32W entry {};
            entry.dwSize = sizeof (entry);

            if (Process32FirstW (snapshot, &entry))
            {
                do
                {
                    const juce::String exe (entry.szExeFile);

                    for (const auto* name : names)
                        if (exe.equalsIgnoreCase (name))
                            found.addIfNotAlreadyThere (exe);
                }
                while (Process32NextW (snapshot, &entry));
            }

            CloseHandle (snapshot);
            return found;
        }
       #endif
    }

    //==============================================================================
    struct SpaceMouse::State
    {
        mutable std::mutex guard;
        std::condition_variable wake;

        bool wanted = false;
        bool stopping = false;
        std::string status = spaceMouseStatus::off;
        std::string deviceName;

        PuckState puck;
        bool open = false;
        Clock::time_point aliveAt {};
        Clock::time_point searchAt {};

        std::atomic<bool> running { false };
        std::atomic<bool> killing { false };
        std::thread reader;
        std::thread killer;

        hid_device* device = nullptr;

        void setStatus (const char* word, std::string named = {})
        {
            const std::lock_guard<std::mutex> held { guard };
            status = word;
            deviceName = std::move (named);
        }

        void close()
        {
            if (device != nullptr)
            {
                hid_close (device);
                device = nullptr;
            }

            const std::lock_guard<std::mutex> held { guard };
            open = false;
            puck = {};
        }

        //  Opens the first puck that will open: true when one did.
        void search()
        {
            auto* const list = hid_enumerate (puckVendor, 0x0000);
            auto any = false;
            std::string named;

            for (auto* at = list; at != nullptr && device == nullptr; at = at->next)
            {
                any = true;
                device = hid_open_path (at->path);

                if (device != nullptr)
                    named = narrowed (at->product_string);
            }

            hid_free_enumeration (list);

            if (device != nullptr)
            {
                hid_set_nonblocking (device, 0);

                {
                    const std::lock_guard<std::mutex> held { guard };
                    open = true;
                    puck = {};
                    aliveAt = Clock::now();
                }

                setStatus (spaceMouseStatus::connected, named.empty() ? std::string ("SpaceMouse") : named);
                return;
            }

            /*  THERE AND NOT OURS: 3Dconnexion's driver has it, the ordinary
                case on a machine that has ever had 3DxWare installed. */
            setStatus (any && SpaceMouse::driverRunning() ? spaceMouseStatus::driver : spaceMouseStatus::searching);
        }

        void run()
        {
            hid_init();

            for (;;)
            {
                bool want = false;

                {
                    std::unique_lock<std::mutex> held { guard };

                    if (stopping)
                        break;

                    want = wanted;

                    if (! want || device == nullptr)
                    {
                        const auto searchDue = want && Clock::now() >= searchAt;

                        if (! searchDue)
                        {
                            wake.wait_for (held, std::chrono::milliseconds { 100 });

                            if (stopping)
                                break;

                            want = wanted;
                        }
                    }
                }

                if (! want)
                {
                    if (device != nullptr)
                        close();

                    setStatus (spaceMouseStatus::off);
                    continue;
                }

                if (device == nullptr)
                {
                    {
                        const std::lock_guard<std::mutex> held { guard };

                        if (Clock::now() < searchAt)
                            continue;

                        searchAt = Clock::now() + searchEvery;
                    }

                    search();
                    continue;
                }

                unsigned char buffer[64] {};
                const auto read = hid_read_timeout (device, buffer, sizeof (buffer), readTimeoutMs);

                if (read < 0)
                {
                    //  Unplugged, or taken: let go, and look again.
                    close();
                    setStatus (spaceMouseStatus::searching);
                    continue;
                }

                const std::lock_guard<std::mutex> held { guard };

                if (read > 0)
                    readPuckReport (buffer, static_cast<std::size_t> (read), puck);

                aliveAt = Clock::now();
            }

            close();
            setStatus (spaceMouseStatus::off);
            hid_exit();
        }
    };

    //==============================================================================
    SpaceMouse::SpaceMouse() : state (std::make_unique<State>()) {}

    SpaceMouse::~SpaceMouse() { stop(); }

    bool SpaceMouse::start()
    {
        if (state->running.exchange (true))
            return false;

        {
            const std::lock_guard<std::mutex> held { state->guard };
            state->stopping = false;
        }

        state->reader = std::thread ([this] { state->run(); });
        return true;
    }

    void SpaceMouse::stop()
    {
        if (! state->running.load())
            return;

        {
            const std::lock_guard<std::mutex> held { state->guard };
            state->stopping = true;
        }

        state->wake.notify_all();

        if (state->reader.joinable())
            state->reader.join();

        if (state->killer.joinable())
            state->killer.join();

        state->running.store (false);
    }

    void SpaceMouse::want (bool open)
    {
        {
            const std::lock_guard<std::mutex> held { state->guard };

            if (state->wanted == open)
                return;

            state->wanted = open;
        }

        state->wake.notify_all();
    }

    SpaceMouse::Reading SpaceMouse::read() const
    {
        const std::lock_guard<std::mutex> held { state->guard };

        Reading out;
        out.state = state->puck;
        out.live = state->open && Clock::now() - state->aliveAt < aliveFor;
        return out;
    }

    std::string SpaceMouse::status() const
    {
        const std::lock_guard<std::mutex> held { state->guard };
        return state->status;
    }

    std::string SpaceMouse::name() const
    {
        const std::lock_guard<std::mutex> held { state->guard };
        return state->deviceName;
    }

    void SpaceMouse::closeDriver()
    {
        /*  ONE AT A TIME, and never waited for here: the caller is the tick
            thread, and a kill waits seconds for the operating system. */
        if (state->killing.exchange (true))
            return;

        if (state->killer.joinable())
            state->killer.join();       // finished: `killing` was false

        state->killer = std::thread ([this]
        {
            SpaceMouse::killDriver();
            state->killing.store (false);

            //  And look again straight away, the puck being free now.
            {
                const std::lock_guard<std::mutex> held { state->guard };
                state->searchAt = Clock::now();
            }

            state->wake.notify_all();
        });
    }

    //==============================================================================
    bool SpaceMouse::driverRunning()
    {
       #if JUCE_WINDOWS
        return ! driverProcesses().isEmpty();
       #elif JUCE_MAC
        juce::ChildProcess pgrep;

        if (! pgrep.start ("pgrep -x 3DconnexionHelper"))
            return false;

        pgrep.waitForProcessToFinish (2000);
        return pgrep.readAllProcessOutput().trim().isNotEmpty();
       #else
        return false;
       #endif
    }

    bool SpaceMouse::killDriver()
    {
       #if JUCE_WINDOWS
        auto allKilled = true;

        for (const auto& name : driverProcesses())
        {
            juce::ChildProcess taskkill;

            if (taskkill.start ("taskkill /F /IM \"" + name + "\""))
                taskkill.waitForProcessToFinish (3000);
            else
                allKilled = false;
        }

        return allKilled;
       #elif JUCE_MAC
        auto ok = true;
        const auto agent = juce::String ("Library/LaunchAgents/com.3dconnexion.3DconnexionHelper.plist");
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

        for (const auto& plist : { "/" + agent, home.getChildFile (agent).getFullPathName() })
        {
            juce::ChildProcess launchctl;

            if (launchctl.start (juce::StringArray { "launchctl", "unload", plist }))
                launchctl.waitForProcessToFinish (3000);
            else
                ok = false;
        }

        juce::ChildProcess killall;

        if (killall.start (juce::StringArray { "killall", "3DconnexionHelper" }))
            killall.waitForProcessToFinish (3000);
        else
            ok = false;

        return ok;
       #else
        return true;
       #endif
    }
}
