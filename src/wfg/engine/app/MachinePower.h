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

#pragma once

/*
    THIS COMPUTER'S POWER, AND WHAT GO.DOT TELLS IT (namespace draft §108; the
    author, 2026-10-22: "Is there a way to make sure the CPU on the mac stays
    on full power and the cores used are the performance ones?", and the same
    day of Windows: "Is there anything to do to avoid glitches and other
    underruns?").

    Two things on each system. The system throttles a process it thinks idle
    or hidden - naps it, coalesces its timers, drops it to its economy
    scheduling class - and will put the computer to sleep under a show that
    is only waiting for its next GO; a process that says it is latency-critical
    is spared all of that. And states no app can override but any can read:
    what caps the processor, and on the Mac how hot the machine says it is.
    Both are said where they are used - the main window's warnings - rather
    than found on the night with a stopwatch.

    The Mac: NSProcessInfo's latency-critical activity; Low Power Mode and
    the thermal state. Windows: the process opted out of power throttling, so
    it is never dropped to EcoQoS and, on a hybrid processor, is placed on the
    performance cores, and its timer requests honoured with the window
    minimised; a power request that keeps the computer awake; Battery saver,
    the power mode slider, the Power saver plan and a processor limited below
    its full speed; running on the battery. Windows says nothing of heat.
    Linux declares nothing and reads as unhindered.

    STD ONLY, so that AppKit stays in the .mm and windows.h in the .cpp.
*/

#include <string>

namespace wfg::app
{
    /*  THE PROCESS DECLARED LATENCY-CRITICAL for as long as this lives: the
        engine while it serves, the plugin and video children for their whole
        life. On macOS this is NSProcessInfo's activity with
        NSActivityLatencyCritical and NSActivityUserInitiated: no App Nap, no
        timer coalescing, no idle system sleep, no automatic termination. On
        Windows it is the process's power throttling switched off (never
        EcoQoS, timer resolution kept) and a system-required power request:
        the computer does not idle-sleep, the display may. `reason` is what
        the system shows beside the process: pmset -g assertions on the Mac,
        powercfg /requests on Windows. */
    class LatencyCritical
    {
    public:
        explicit LatencyCritical (const std::string& reason);
        ~LatencyCritical();

        LatencyCritical (const LatencyCritical&) = delete;
        LatencyCritical& operator= (const LatencyCritical&) = delete;

        /** True only where the system took the declaration. */
        bool declared() const noexcept { return token != nullptr; }

    private:
        void* token = nullptr;
    };

    /*  HOW HOT THE SYSTEM SAYS THE COMPUTER IS, in its own four steps: at
        `serious` it has already slowed the processor. The Mac's; Windows has
        no such reading and says nominal. */
    enum class Thermal { nominal, fair, serious, critical };

    /** `nominal`, `fair`, `serious`, `critical`: what the tree says. */
    const char* wordOf (Thermal) noexcept;

    /*  WHAT CAPS THE PROCESSOR, named as the system names it, so the warning
        can say which setting to flip: the Mac's Low Power Mode; Windows'
        Battery saver, its power mode slider at Best power efficiency, the
        Power saver plan, or a processor whose current limit sits below its
        full speed (a plan's processor maximum state under 100 %, or a limit
        the firmware applied). The first that holds, in this order. */
    enum class PowerCap { none, lowPowerMode, batterySaver, bestPowerEfficiency, powerSaverPlan, processorLimited };

    /** `none`, `lowPowerMode`, `batterySaver`, `bestPowerEfficiency`, `powerSaverPlan`, `processorLimited`. */
    const char* wordOf (PowerCap) noexcept;

    struct MachinePower
    {
        /** Something caps the processor: `cappedBy` says what. */
        bool lowPowerMode = false;
        PowerCap cappedBy = PowerCap::none;

        /** Running on the battery (Windows; false where it cannot be asked). */
        bool onBattery = false;

        Thermal thermal = Thermal::nominal;

        bool operator== (const MachinePower&) const = default;
    };

    /*  What the system says now. The message thread, once a second: a few
        property reads, never a wait. Unhindered where it cannot be asked. */
    MachinePower readMachinePower();
}
