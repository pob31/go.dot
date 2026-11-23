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
    THE REAL-TIME CLASS, AND HOW A THREAD GETS IT (namespace draft §108, the
    Linux half; the author, 2026-10-24: "In a similar fashion to what we did
    for macOS and Windows are there performance settings that are Linux
    specific?" - and the thing that matters most there is not power at all).

    Every thread that makes or times the sound asks for the real-time class:
    the tick thread, the plugin child's worker, the no-card driver, and on
    Linux the ALSA callback thread JUCE starts as an ordinary one (raised
    from the message thread once it has said who it is). On macOS
    and Windows the ask always succeeds. On Linux it needs a limit
    (RLIMIT_RTPRIO) a stock desktop user does not have - Ubuntu grants it
    to the `pipewire` and `audio` groups, and puts nobody in them - and
    until now a refusal left the show's clock at the priority of everything
    else on the machine, and said nothing.

    So on Linux the ask has a second route, the one PipeWire itself takes on
    a stock install: RealtimeKit (rtkit-daemon, on the system bus of every
    desktop with PipeWire or PulseAudio) grants any user's thread the
    real-time class, no group and no admin, under its own cap (20 by
    default, where the limit's route gives 50 and above) and on condition
    that the process bounds its real-time run time (RLIMIT_RTTIME, 200 ms by
    default): a real-time thread of ours that ran 200 ms of CPU without once
    blocking would be killed. None does - the tick waits four times a
    millisecond, the workers wait on their lanes, the ALSA thread waits on
    the card - and the limit is set only when this route is taken.

    What was got is said: `RealtimeBy` is the tick thread's row
    `/godot/engine/realtimeBy`, and `none` is a warning of "This computer".
*/

#include <cstdint>
#include <string>

namespace wfg::app
{
    /*  WHO ASKS: the sound - an audio callback, a plugin worker, the no-card
        driver - sits above the clock, which sits above everything ordinary. */
    enum class RealtimeRank { clock, sound };

    /*  HOW THE REAL-TIME CLASS WAS GOT: by the system on the thread's own
        rights (the Mac's time-constraint policy, Windows' TIME_CRITICAL,
        Linux's SCHED_FIFO under the user's limit), by RealtimeKit on Linux
        where the limit refused, or not at all. `pending` is the row's
        resting word before the tick thread has asked. */
    enum class RealtimeBy { pending, none, system, rtkit };

    /** `pending`, `none`, `system`, `rtkit`: what the tree says. */
    const char* wordOf (RealtimeBy) noexcept;

    /*  THE CALLING THREAD raised to the real-time class. `periodMs` and
        `computationMs` are the Mac's time-constraint policy (a block's length
        and the budget inside it); Windows and Linux read neither. Called once,
        at the top of the thread, before any real-time work: on Linux the
        RealtimeKit route talks to the bus. Never from the audio callback. */
    RealtimeBy takeRealtime (RealtimeRank rank, double periodMs, double computationMs) noexcept;

    /*  ANOTHER THREAD OF THIS PROCESS, by its kernel thread id: Linux only,
        `none` elsewhere. For a thread something else started, which a thread
        of ours raises from outside. */
    RealtimeBy takeRealtimeOf (long threadId, RealtimeRank rank) noexcept;

    /*  THE THREAD WITH THIS NAME, as the kernel keeps it (the first fifteen
        bytes of what it was named), found under /proc/self/task: Linux only,
        `none` elsewhere or when no thread of that name exists. Not how the
        ALSA callback thread is found: JUCE names it "JUCE v8.0.13: ALSA",
        which is more than pthread_setname_np takes, so the name is dropped
        and the thread stays unnamed - it says its own id at its first block
        instead (DeviceAudioDriver::raiseCallbackThread). */
    RealtimeBy takeRealtimeOfThreadNamed (const std::string& kernelName, RealtimeRank rank) noexcept;

    /** What the kernel keeps of a thread's name: its first fifteen bytes. */
    std::string kernelThreadName (const std::string& name);

    /*  FOR A THREAD THAT NEVER SLEEPS (namespace draft §148): the plugin
        child's worker on Windows, which looks for its next request without a
        pause because the parent, on the audio thread, cannot wake it. Windows'
        multimedia class - what `takeRealtime` asks for there - is made for
        threads that work and then wait: one that uses its whole share of a
        processor is dropped to the lowest priorities for the rest of each
        period, and any ordinary thread that wants its core then has it.
        `timeCritical` is the plain top of the ordinary range instead, which
        nothing takes a share from - what the tick thread has had since
        2026-10-06. `timeCriticalAway` is that, with a wish for a performance
        core other than the machine's first processor, where its interrupts
        mostly land: a wish the system may pass over, never a pin. Elsewhere
        all three are `takeRealtime` for the sound. */
    enum class SpinClass { multimedia, timeCritical, timeCriticalAway };

    RealtimeBy takeRealtimeSpinning (SpinClass spinClass, double periodMs, double computationMs) noexcept;

    /*  WHERE THE CALLING THREAD IS (namespace draft §148): the processor it is
        on, ONE MORE than its number, or nought where the system does not say
        (macOS). Read from the thread's own block, no kernel call. */
    std::uint32_t processorNow() noexcept;

    /*  WHAT KIND OF CORE THAT IS, for a note: "performance" or "efficiency" on
        a machine with both (Windows' efficiency classes, the highest being the
        performance cores), and empty where there is one kind or nobody says.
        Asks the system once; never on a real-time thread. */
    std::string coreKindOf (std::uint32_t processorPlusOne);
}
