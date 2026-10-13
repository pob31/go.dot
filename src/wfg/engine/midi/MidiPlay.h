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
    A RENDERED MIDI FILE PLAYED INTO AN INPUT (namespace draft §60, TG.4;
    AIZ): `wfg serve --midi-play=<port>=<file>`.

    WHY. A CI runner has no MIDI port, so nothing can arrive on one - and a
    time group whose source is MTC cannot be driven from outside without
    something arriving. `--midi-render` (TC.5, AHV) already writes what a port
    would have sent, one line a message; this plays such a file back into a
    port's INPUT, each line at its moment, down the road a message from a
    cable takes (`MidiInputs::inject`): MTC to the inbox, anything else to the
    surfaces and the triggers. So a black-box driver renders MTC from one
    serve, or writes it by hand, and plays it into another (TG.11).

    THE FILE is `--midi-render`'s: `sent_ns due_ns` and the bytes in hex, one
    message a line. A line's moment is its `due_ns`, or its `sent_ns` when the
    due is nought (the queue's messages, sent as soon as they are taken). The
    first line goes at once; each next one the difference of their moments
    after it - never before the one before it, so a file whose moments step
    back plays in its order. Looped (`--midi-play-loop`), the file starts
    again after its last line, at the spacing its last two distinct moments
    had (a second if every line has the same moment).

    THE MOMENTS ARE KEPT, NOT ACCUMULATED: each line is due at the play's
    start plus its offset, so a line that went late does not push the rest
    later. Each is stamped with the moment it was injected - when it really
    "arrived" - not the moment it was due.

    ITS OWN THREAD, raised as the tick and the MTC lane are, sleeping until a
    millisecond and a half before each moment and then waiting out the rest
    on the MTC lane's clock (`MtcLane::Clock::sleepUntil`), so a quarter frame
    played lands about as close to its moment as the lane sends one.
*/

#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/midi/MtcLane.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace wfg::midi
{
    //  One line of a render: its moment on the steady clock, and its bytes.
    struct PlayLine
    {
        std::int64_t moment = 0;
        Bytes bytes;
    };

    /*  A render's text read into lines. Nothing, with `why` saying which line
        and what is wrong with it, for a line that is not two integers and at
        least one hex byte. Blank lines are skipped. Read with `from_chars`,
        which no locale changes (the fr_FR rule, README § Contributing). */
    std::optional<std::vector<PlayLine>> parseRender (const std::string& text, std::string& why);

    /*  The same from a file, a path the command line gave resolved against
        where serve was started - as `--midi-render` resolves its own. */
    std::optional<std::vector<PlayLine>> loadRender (const std::string& path, std::string& why);

    /*  When each line plays, counted from the play's start: the first at
        nought, each next one the difference of their moments later, never
        earlier than the one before. */
    std::vector<std::int64_t> offsetsOf (const std::vector<PlayLine>& lines);

    /*  After the last line, how long before a loop starts again: the last
        positive step between two lines' moments, or a second if there is
        none. */
    std::int64_t loopGapOf (const std::vector<PlayLine>& lines);

    class MidiPlayer
    {
    public:
        //  Where each line goes: its bytes and the moment they were handed over.
        using Inject = std::function<void (const Bytes& bytes, std::int64_t arrivalNanos)>;

        MidiPlayer (std::vector<PlayLine> linesToPlay, bool loopIt, Inject injectInto,
                    std::shared_ptr<const MtcLane::Clock> clockToUse = MtcLane::steadyClock());
        ~MidiPlayer();

        MidiPlayer (const MidiPlayer&) = delete;
        MidiPlayer& operator= (const MidiPlayer&) = delete;

        //  The first line at once. Safe to call twice.
        void start();

        //  Stops between two lines and joins the thread. Safe to call twice, and when never started.
        void stop();

        //  Every line played once, unlooped.
        bool finished() const noexcept { return done.load (std::memory_order_acquire); }

        //  Lines handed over so far, every pass counted.
        std::uint64_t playedCount() const noexcept { return played.load (std::memory_order_relaxed); }

    private:
        void run();

        //  False when stopped before the moment came.
        bool waitUntil (std::int64_t moment);

        std::vector<PlayLine> lines;
        std::vector<std::int64_t> offsets;
        std::int64_t gap = 0;
        bool loop = false;
        Inject inject;
        std::shared_ptr<const MtcLane::Clock> clock;

        std::mutex sleepMutex;
        std::condition_variable wake;
        bool running = false;
        std::thread worker;

        std::atomic<bool> done { false };
        std::atomic<std::uint64_t> played { 0 };
    };
}
