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
//  LINUX: nothing to declare, nothing to read.
#elif ! defined (__APPLE__)

namespace wfg::app
{
    LatencyCritical::LatencyCritical (const std::string&) {}
    LatencyCritical::~LatencyCritical() = default;

    MachinePower readMachinePower() { return {}; }
}

#endif
