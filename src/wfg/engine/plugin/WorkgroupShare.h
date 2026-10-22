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
    THE INTERFACE'S AUDIO WORKGROUP, HANDED TO THE PLUGIN CHILD (macOS,
    2026-10-22; the author: "Let's add it for the plugin host child process").

    The voice child answers every block inside the audio callback's deadline -
    the engine's thread spins on its answer - so its worker is part of the
    work that keeps the sound on time, and Apple's word for that (AudioToolbox's
    AudioWorkInterval.h) is that such a thread joins the device's workgroup. On
    Apple silicon that is what keeps it on a performance core beside the IO
    thread rather than wherever the scheduler parks an unrelated deadline. The
    tick thread joins it in-process (TickThread::setWorkgroups); the child is
    another process, and a workgroup crosses only as a Mach send right.

    THE PARENT. The device layer shares whatever workgroup the running device
    has, and nothing when it stops. JUCE does not hand out the os_workgroup_t
    behind its AudioWorkgroup, so the parent asks CoreAudio for each device's
    and keeps the one JUCE's equals - the HAL gives the same object to every
    ask. The first share registers a Mach service named for this process's id
    with launchd, once for the life of the process, and answers on a dispatch
    queue: never the audio thread.

    THE CHILD. Its message thread asks the parent once a second, beside its
    check that the parent is alive, sending the generation it last acted on;
    the parent answers with the port only when the workgroup has changed since.
    The worker joins what it is handed at the top of its next pass - a join or
    a leave only when it changed, the one kernel call the tick thread also makes
    there - and says in the region's header whether it is in, TRUE ONLY FOR WHAT
    TOOK EFFECT. One offer at a time: the message thread hands the next only
    once the worker has taken the last, and lets go of a workgroup only once
    the worker has left it. Atomics between them and nothing else.

    ELSEWHERE all of this compiles to nothing and reports nobody in.

    NAMES JUCE'S WORKGROUP BY A DECLARATION ONLY.
*/

#include <cstdint>
#include <memory>

namespace juce
{
    class AudioWorkgroup;
}

namespace wfg::plugin
{
    /*  The parent's side, from the device layer: the running device's
        workgroup, or an empty one when it stops. True when a workgroup is now
        shared - false where there is none, it is not a CoreAudio device's,
        or this is not macOS. Not the audio thread. */
    bool shareAudioWorkgroup (const juce::AudioWorkgroup& deviceWorkgroup);

    /*  The same with an os_workgroup_t in hand (null to share nothing): what
        a test shares, having made a work interval of its own. Borrowed - the
        share takes its own reference. Nothing elsewhere than macOS. */
    bool shareWorkgroupHandle (void* osWorkgroup);

    //==============================================================================
    /*  The child's side. Made on the message thread, outliving the worker. */
    class WorkgroupFollower
    {
    public:
        /** Nought or less follows nobody. */
        explicit WorkgroupFollower (std::int64_t parentPid);
        ~WorkgroupFollower();

        WorkgroupFollower (const WorkgroupFollower&) = delete;
        WorkgroupFollower& operator= (const WorkgroupFollower&) = delete;

        /*  The message thread, once a second: asks the parent what it shares
            now, and offers the worker a change. Waits for the parent no more
            than 50 ms. */
        void poll();

        /*  The worker, at the top of every pass: joins or leaves when there is
            something new to, and answers whether it is in. No lock, no
            allocation; a kernel call only on a change. */
        bool keepUp() noexcept;

        /** The worker, on its way out: leaves what it joined. */
        void leave() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
