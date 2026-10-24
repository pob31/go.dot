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

#include <wfg/engine/app/MachinePower.h>

namespace wfg::app
{
    const char* wordOf (Thermal thermal) noexcept
    {
        switch (thermal)
        {
            case Thermal::fair:     return "fair";
            case Thermal::serious:  return "serious";
            case Thermal::critical: return "critical";
            case Thermal::nominal:  break;
        }

        return "nominal";
    }

    const char* wordOf (PowerCap cap) noexcept
    {
        switch (cap)
        {
            case PowerCap::lowPowerMode:        return "lowPowerMode";
            case PowerCap::batterySaver:        return "batterySaver";
            case PowerCap::bestPowerEfficiency: return "bestPowerEfficiency";
            case PowerCap::powerSaverPlan:      return "powerSaverPlan";
            case PowerCap::processorLimited:    return "processorLimited";
            case PowerCap::powerSaver:          return "powerSaver";
            case PowerCap::none:                break;
        }

        return "none";
    }
}

//==============================================================================
//  WINDOWS. The Mac's is MachinePower_mac.mm; Linux is at the foot.
#if defined (_WIN32)

 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <powrprof.h>

 #include <cstring>
 #include <vector>

 //  Named by SDKs from Windows 11's on; the value is the same everywhere.
 #ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
  #define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
 #endif

namespace wfg::app
{
    namespace
    {
        /*  NEVER THROTTLED. ControlMask names what Go.dot decides, StateMask
            what it decides it to be: execution speed controlled and off means
            the process is never dropped to EcoQoS, which on a hybrid processor
            is also what keeps it off the efficiency cores; timer resolution
            controlled and off means its timer requests are honoured with the
            window minimised (Windows 11). Windows 10 knows no timer flag and
            refuses the pair, so it is asked for the speed alone. */
        bool optOutOfPowerThrottling()
        {
            PROCESS_POWER_THROTTLING_STATE state {};
            state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
            state.StateMask = 0;

            if (::SetProcessInformation (::GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof (state)))
                return true;

            state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            return ::SetProcessInformation (::GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof (state)) != 0;
        }

        std::wstring wide (const std::string& utf8)
        {
            const auto length = ::MultiByteToWideChar (CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
            std::wstring out (length > 0 ? static_cast<std::size_t> (length - 1) : 0, L'\0');

            if (length > 1)
                ::MultiByteToWideChar (CP_UTF8, 0, utf8.c_str(), -1, out.data(), length);

            return out;
        }

        /*  THE COMPUTER KEPT AWAKE: a system-required request, which is what
            powercfg /requests lists under the process with its reason. The
            display is not asked for; a show waiting for its GO does not need
            it lit. */
        HANDLE keepAwake (const std::string& reason)
        {
            const auto words = wide (reason);

            REASON_CONTEXT context {};
            context.Version = POWER_REQUEST_CONTEXT_VERSION;
            context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
            context.Reason.SimpleReasonString = const_cast<LPWSTR> (words.c_str());

            const auto request = ::PowerCreateRequest (&context);

            if (request == INVALID_HANDLE_VALUE)
                return nullptr;

            ::PowerSetRequest (request, PowerRequestSystemRequired);
            return request;
        }

        struct Declaration
        {
            HANDLE request = nullptr;
        };

        //  The power mode slider's "Best power efficiency", and the Power saver plan.
        constexpr GUID bestPowerEfficiencyOverlay { 0x961cc777, 0x2547, 0x4f9d, { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a } };
        constexpr GUID powerSaverPlan             { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a } };

        bool same (const GUID& a, const GUID& b) noexcept { return std::memcmp (&a, &b, sizeof (GUID)) == 0; }

        /*  The slider is read through a function powrprof.dll has exported
            since Windows 10 1709 but never documented, so it is looked up by
            name and its absence is "not at Best power efficiency". */
        using PowerGetEffectiveOverlaySchemeFn = DWORD (WINAPI*) (GUID*);

        PowerGetEffectiveOverlaySchemeFn overlaySchemeReader()
        {
            static const auto reader = []
            {
                if (const auto library = ::LoadLibraryW (L"powrprof.dll"))
                    return reinterpret_cast<PowerGetEffectiveOverlaySchemeFn> (
                        reinterpret_cast<void*> (::GetProcAddress (library, "PowerGetEffectiveOverlayScheme")));

                return PowerGetEffectiveOverlaySchemeFn (nullptr);
            }();

            return reader;
        }

        //  Documented, but in no header: CallNtPowerInformation's ProcessorInformation.
        struct ProcessorPowerInformation
        {
            ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
        };

        bool anyProcessorLimited()
        {
            const auto count = ::GetActiveProcessorCount (ALL_PROCESSOR_GROUPS);

            if (count == 0)
                return false;

            std::vector<ProcessorPowerInformation> info (count);
            const auto bytes = static_cast<ULONG> (sizeof (ProcessorPowerInformation) * count);

            if (::CallNtPowerInformation (ProcessorInformation, nullptr, 0, info.data(), bytes) != 0)
                return false;

            for (const auto& processor : info)
                if (processor.MaxMhz > 0 && processor.MhzLimit > 0 && processor.MhzLimit < processor.MaxMhz)
                    return true;

            return false;
        }
    }

    LatencyCritical::LatencyCritical (const std::string& reason)
    {
        if (! optOutOfPowerThrottling())
            return;

        auto* declaration = new Declaration();
        declaration->request = keepAwake (reason);
        token = declaration;
    }

    LatencyCritical::~LatencyCritical()
    {
        if (token == nullptr)
            return;

        auto* declaration = static_cast<Declaration*> (token);

        if (declaration->request != nullptr)
        {
            ::PowerClearRequest (declaration->request, PowerRequestSystemRequired);
            ::CloseHandle (declaration->request);
        }

        /*  The throttling opt-out is left as it is: the process ends with the
            declaration in every place one is held, and a child that outlived
            it would be a child that is still playing. */
        delete declaration;
        token = nullptr;
    }

    MachinePower readMachinePower()
    {
        MachinePower power;

        SYSTEM_POWER_STATUS status {};
        const auto haveStatus = ::GetSystemPowerStatus (&status) != 0;

        if (haveStatus)
            power.onBattery = status.ACLineStatus == 0;

        if (haveStatus && status.SystemStatusFlag == 1)
        {
            power.cappedBy = PowerCap::batterySaver;
        }
        else if (GUID overlay {}; overlaySchemeReader() != nullptr && overlaySchemeReader() (&overlay) == ERROR_SUCCESS
                                  && same (overlay, bestPowerEfficiencyOverlay))
        {
            power.cappedBy = PowerCap::bestPowerEfficiency;
        }
        else
        {
            GUID* active = nullptr;

            if (::PowerGetActiveScheme (nullptr, &active) == ERROR_SUCCESS && active != nullptr)
            {
                if (same (*active, powerSaverPlan))
                    power.cappedBy = PowerCap::powerSaverPlan;

                ::LocalFree (active);
            }

            if (power.cappedBy == PowerCap::none && anyProcessorLimited())
                power.cappedBy = PowerCap::processorLimited;
        }

        power.lowPowerMode = power.cappedBy != PowerCap::none;
        return power;
    }
}

//==============================================================================
//  LINUX (2026-10-24): power-profiles-daemon and logind over the system bus,
//  the processor, the battery and the heat from sysfs.
#elif ! defined (__APPLE__)

 #include <wfg/engine/app/SystemBus_linux.h>

 #include <dirent.h>
 #include <unistd.h>

 #include <cctype>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <vector>

namespace wfg::app
{
    namespace
    {
        /*  power-profiles-daemon, GNOME's and KDE's: renamed at 0.20 and
            still answering to the old name, so the new is asked first. */
        constexpr bus::Target profiles    { "org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles", "org.freedesktop.UPower.PowerProfiles" };
        constexpr bus::Target profilesOld { "net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles" };
        constexpr bus::Target login       { "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager" };

        struct Declaration
        {
            bus::Connection connection;
            const bus::Target* daemon = nullptr;
            dbus_uint32_t cookie = 0;
            bool held = false;
            int inhibitor = -1;
        };

        /*  THE PERFORMANCE PROFILE HELD for as long as the connection lives,
            which is what `powerprofilesctl list-holds` shows under Go.dot's
            name and reason: the processor's governor at full performance
            whatever the user's own choice, as the Mac's activity and
            Windows' throttling opt-out are. Refused where the hardware offers
            no performance profile, and by polkit to a session that is not
            the seat's (an SSH session); then the reading below still says
            what the active profile is. */
        bool holdPerformance (Declaration& declaration, const std::string& reason)
        {
            const char* profile = "performance";
            const char* why = reason.c_str();
            const char* application = "go.dot";

            for (const auto* daemon : { &profiles, &profilesOld })
            {
                auto* request = bus::methodCall (*daemon, "HoldProfile");

                if (request == nullptr)
                    continue;

                dbus_message_append_args (request, DBUS_TYPE_STRING, &profile, DBUS_TYPE_STRING, &why,
                                          DBUS_TYPE_STRING, &application, DBUS_TYPE_INVALID);
                const auto reply = bus::call (declaration.connection, request, 2000);

                if (! reply.ok())
                    continue;

                dbus_uint32_t cookie = 0;

                if (dbus_message_get_args (reply.get(), nullptr, DBUS_TYPE_UINT32, &cookie, DBUS_TYPE_INVALID))
                {
                    declaration.daemon = daemon;
                    declaration.cookie = cookie;
                    return true;
                }
            }

            return false;
        }

        /*  THE COMPUTER KEPT AWAKE: logind's inhibitor, which `systemd-inhibit
            --list` shows with Go.dot's reason, blocks the idle suspend GNOME
            and KDE ask logind for; the display may still sleep. "sleep:idle"
            first, "sleep" alone where the seat's policy refuses the pair. The
            returned descriptor is the inhibitor: closing it ends it. */
        int inhibitSleep (Declaration& declaration, const std::string& reason)
        {
            const char* who = "Go.dot";
            const char* why = reason.c_str();
            const char* mode = "block";

            for (const char* what : { "sleep:idle", "sleep" })
            {
                auto* request = bus::methodCall (login, "Inhibit");

                if (request == nullptr)
                    continue;

                dbus_message_append_args (request, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who,
                                          DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);
                const auto reply = bus::call (declaration.connection, request, 2000);

                if (! reply.ok())
                    continue;

                int descriptor = -1;

                if (dbus_message_get_args (reply.get(), nullptr, DBUS_TYPE_UNIX_FD, &descriptor, DBUS_TYPE_INVALID) && descriptor >= 0)
                    return descriptor;
            }

            return -1;
        }

        //  A small sysfs file, as text with its newline gone; empty where absent.
        std::string readText (const std::string& path)
        {
            auto* file = std::fopen (path.c_str(), "r");

            if (file == nullptr)
                return {};

            char buffer[128] {};
            const auto read = std::fread (buffer, 1, sizeof (buffer) - 1, file);
            std::fclose (file);
            buffer[read] = '\0';

            if (auto* newline = std::strchr (buffer, '\n'))
                *newline = '\0';

            return buffer;
        }

        std::int64_t readNumber (const std::string& path)
        {
            const auto text = readText (path);
            return text.empty() ? -1 : std::strtoll (text.c_str(), nullptr, 10);
        }

        std::vector<std::string> entriesOf (const char* folder, const char* prefix)
        {
            std::vector<std::string> out;

            if (auto* dir = opendir (folder))
            {
                while (const auto* entry = readdir (dir))
                    if (std::strncmp (entry->d_name, prefix, std::strlen (prefix)) == 0)
                        out.push_back (std::string (folder) + "/" + entry->d_name);

                closedir (dir);
            }

            return out;
        }

        /*  THE PROCESSOR, from cpufreq: an energy preference of "power" is the
            power-saver profile by hand (a governor of "powersave" is not - it
            is intel_pstate's everyday one); a scaling maximum under the
            processor's own maximum is a clock someone capped, which is
            Windows' MhzLimit under MaxMhz. */
        PowerCap processorCap()
        {
            auto cap = PowerCap::none;

            for (const auto& cpu : entriesOf ("/sys/devices/system/cpu", "cpu"))
            {
                if (cpu.size() < 5 || ! std::isdigit (static_cast<unsigned char> (cpu[cpu.find_last_of ('/') + 4])))
                    continue;

                const auto cpufreq = cpu + "/cpufreq/";

                if (readText (cpufreq + "energy_performance_preference") == "power")
                    return PowerCap::powerSaver;

                const auto scalingMax = readNumber (cpufreq + "scaling_max_freq");
                const auto hardwareMax = readNumber (cpufreq + "cpuinfo_max_freq");

                if (scalingMax > 0 && hardwareMax > 0 && scalingMax < hardwareMax)
                    cap = PowerCap::processorLimited;
            }

            return cap;
        }

        //  On the battery when there is one and no mains supply is online.
        bool onBattery()
        {
            auto battery = false, mains = false;

            for (const auto& supply : entriesOf ("/sys/class/power_supply", ""))
            {
                const auto type = readText (supply + "/type");

                if (type == "Battery")
                    battery = true;
                else if (type == "Mains" && readNumber (supply + "/online") == 1)
                    mains = true;
            }

            return battery && ! mains;
        }

        /*  THE HEAT, from the thermal zones' own trip points: past a passive
            trip the kernel has begun to slow the processor (fair), past a hot
            trip it slows it hard (serious), at the critical trip it is about
            to shut the machine down. A trip at or below zero is one the
            driver left unset (a Wi-Fi zone says -274000) and is no trip. */
        Thermal thermalFromZones()
        {
            auto worst = Thermal::nominal;

            for (const auto& zone : entriesOf ("/sys/class/thermal", "thermal_zone"))
            {
                const auto temperature = readNumber (zone + "/temp");

                if (temperature <= 0)
                    continue;

                for (int trip = 0; trip < 16; ++trip)
                {
                    const auto stem = zone + "/trip_point_" + std::to_string (trip) + "_";
                    const auto type = readText (stem + "type");

                    if (type.empty())
                        break;

                    const auto at = readNumber (stem + "temp");

                    if (at <= 0 || temperature < at)
                        continue;

                    const auto reached = type == "critical" ? Thermal::critical
                                       : type == "hot"      ? Thermal::serious
                                       : type == "passive"  ? Thermal::fair
                                                            : Thermal::nominal;
                    if (reached > worst)
                        worst = reached;
                }
            }

            return worst;
        }
    }

    LatencyCritical::LatencyCritical (const std::string& reason)
    {
        auto* declaration = new Declaration();

        if (declaration->connection.open())
        {
            declaration->held = holdPerformance (*declaration, reason);
            declaration->inhibitor = inhibitSleep (*declaration, reason);
        }

        if (! declaration->held && declaration->inhibitor < 0)
        {
            delete declaration;
            return;
        }

        token = declaration;
    }

    LatencyCritical::~LatencyCritical()
    {
        if (token == nullptr)
            return;

        auto* declaration = static_cast<Declaration*> (token);

        if (declaration->held && declaration->daemon != nullptr)
        {
            if (auto* request = bus::methodCall (*declaration->daemon, "ReleaseProfile"))
            {
                dbus_message_append_args (request, DBUS_TYPE_UINT32, &declaration->cookie, DBUS_TYPE_INVALID);
                bus::call (declaration->connection, request, 1000);
            }
        }

        if (declaration->inhibitor >= 0)
            close (declaration->inhibitor);

        //  And the connection closes with it, which releases both again.
        delete declaration;
        token = nullptr;
    }

    MachinePower readMachinePower()
    {
        MachinePower power;

        /*  The daemon's word first: the active profile - which, under our own
            hold, is performance - and whether it has had to degrade
            performance for heat. One connection kept for the process, on the
            message thread, which is the only caller. */
        static const bus::Connection connection;

        if (connection.open())
        {
            for (const auto* daemon : { &profiles, &profilesOld })
            {
                const auto active = bus::stringProperty (connection, *daemon, "ActiveProfile");

                if (! active.has_value())
                    continue;

                if (*active == "power-saver")
                    power.cappedBy = PowerCap::powerSaver;

                if (bus::stringProperty (connection, *daemon, "PerformanceDegraded").value_or ("") == "high-operating-temperature")
                    power.thermal = Thermal::serious;

                break;
            }
        }

        if (power.cappedBy == PowerCap::none)
            power.cappedBy = processorCap();

        power.lowPowerMode = power.cappedBy != PowerCap::none;
        power.onBattery = onBattery();

        if (const auto zones = thermalFromZones(); zones > power.thermal)
            power.thermal = zones;

        return power;
    }
}

#endif
