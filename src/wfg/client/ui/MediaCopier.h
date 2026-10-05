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
    THE BYTES OF AN IMPORT, MOVED OFF THE WINDOW'S THREAD (namespace draft §30,
    S7).

    Eight sounds of 58 to 171 MB are three quarters of a gigabyte, and the
    import used to copy them on the message thread, one after another, inside
    the drop: the window froze, Windows wrote "Not responding" over its title,
    and every pass of the timer - the transport, the running pane, GO's own
    button - waited for the disk. Now one worker, owned by the window and
    joined when it closes, is handed one file at a time by
    `model::MediaImports`, which decides everything, and answers on the
    message thread. Nothing else in the window waits for it.

    WHAT IT DOES WITH ONE FILE is `doMediaJob`, a plain function on purpose:
    the tests call it on real files, with no thread and no message loop.

    A COPY NEVER LANDS HALF WRITTEN. The bytes go into a hidden part-file
    beside the target and are moved over it whole once they are all there, so
    a copy that fails - a full disk - or that is stopped - the window closing -
    leaves the folder as it found it, and a Replace that fails leaves the old
    file playing. Nothing in `media/` is ever deleted by this: what an unsaved
    session leaves there stays until somebody removes it.
*/

#include <wfg/client/model/Media.h>

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace wfg::client::ui
{
    /*  ONE FILE, on whatever thread calls it: looked at, or copied into
        `folder` - the show's `media/`, made when it is not there yet.
        `stopping` is asked between two megabytes; a copy it stops answers
        `failed`, with its part-file gone. */
    model::MediaWork doMediaJob (const model::MediaJob& job, const juce::File& folder,
                                 const std::function<bool()>& stopping);

    class MediaCopier final : private juce::Thread
    {
    public:
        /** Called on the message thread with what one job did, unless the copier has been stopped. */
        using Done = std::function<void (const model::MediaWork&)>;

        explicit MediaCopier (Done whenDone);

        /*  Stops and joins: the copy under way ends at its next megabyte, and
            nothing it would have said arrives afterwards. */
        ~MediaCopier() override;

        /*  Message thread, and only while `busy()` is false - one file at a
            time, which is what keeps the questions and the cues in order. */
        void start (const model::MediaJob& job, const juce::File& folder);

        /** Message thread: whether a job is out, its answer not yet delivered. */
        bool busy() const noexcept { return working; }

        /** For good: nothing more is started, and nothing more is said. */
        void stop();

    private:
        void run() override;

        Done done;
        bool working = false;
        bool stopped = false;

        /*  Read and written on the message thread only: the worker carries a
            copy of the pointer into every answer it posts, and an answer that
            arrives after `stop` finds it false and touches nothing. */
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);

        juce::CriticalSection jobLock;
        std::optional<std::pair<model::MediaJob, juce::File>> queued;   ///< guarded by jobLock
        juce::WaitableEvent wake;
    };
}
