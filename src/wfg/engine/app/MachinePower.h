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
    on full power and the cores used are the performance ones?").

    Two things, both the Mac's. The system throttles a process it thinks idle
    or hidden - naps it, coalesces its timers - and will put the computer to
    sleep under a show that is only waiting for its next GO; a process that
    says it is latency-critical is spared all of that. And two states no app
    can override but any can read: Low Power Mode, which caps the processor,
    and the thermal state, which slows it once the machine is hot. Both are
    said where they are used - the main window's warnings - rather than found
    on the night with a stopwatch.

    Elsewhere nothing is declared and the computer reads as unhindered.

    STD ONLY, so that AppKit stays in the .mm.
*/

#include <string>

namespace wfg::app
{
    /*  THE PROCESS DECLARED LATENCY-CRITICAL for as long as this lives: the
        engine while it serves, the plugin and video children for their whole
        life. On macOS this is NSProcessInfo's activity with
        NSActivityLatencyCritical and NSActivityUserInitiated: no App Nap, no
        timer coalescing, no idle system sleep, no automatic termination.
        `reason` is what the system shows beside the process. */
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
        `serious` it has already slowed the processor. */
    enum class Thermal { nominal, fair, serious, critical };

    /** `nominal`, `fair`, `serious`, `critical`: what the tree says. */
    const char* wordOf (Thermal) noexcept;

    struct MachinePower
    {
        /** Low Power Mode on: the processor is capped. */
        bool lowPowerMode = false;
        Thermal thermal = Thermal::nominal;

        bool operator== (const MachinePower&) const = default;
    };

    /*  What the system says now. The message thread, once a second: a
        property read, never a wait. Unhindered where it cannot be asked. */
    MachinePower readMachinePower();
}
