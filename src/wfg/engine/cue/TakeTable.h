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
    WHAT EACH SAMPLING CHANNEL'S TAKE IS DOING, as the log has it (Phase 9c,
    stage 9c.3, namespace draft §19.3-§19.6).

    The audio side holds the take itself - the samples, in the `Looper` the
    host keeps beside the Edit. This is its account for everybody else: the
    state it is in, the length it was closed at, how many layers lie on it and
    where the loop points are. It moves only with the take verbs, a transport
    cue, a mic cue's GO and its channel let go, and what the audio thread
    reported and the log recorded (`take.closed`) - so a replay with no audio
    reaches the same account, and every refusal is decided from it (namespace
    draft §7's rule).

    ITS RULES ARE THE LOOPER'S, press for press (§19.3's table, and what 9c.1
    decided where the table was silent): the same press moves both, the
    account at once and the take at a sample the hook places. The one thing
    the account cannot know is how long a take closed at, which the audio
    thread says, and `closed` writes.

    AND THE PRESSES, queued for the tick hook: a handler moves the account and
    asks for the press; the hook places it at `now + the launch latency`, as a
    launch is placed, so a press and a cue land on a sample the log can name
    (§19.6). A replay has no hook, and a table told so queues nothing.

    THREADING: none of its own - the tick thread's, as the run table and the
    DCA table are. The playhead alone is written by the hook, read off the
    audio side a tick at a time for the picture; it is not in the log.
*/

#include <map>
#include <string>
#include <vector>

namespace wfg::cue
{
    /** The presses a take answers to - the order the Looper's verbs are in. */
    enum class TakeVerb : int { record, loop, overdub, undo, clear, hold, points };

    /** One press for the hook to place; points in seconds into the take. */
    struct TakePress
    {
        std::string channel;
        TakeVerb verb = TakeVerb::record;
        double in = 0.0;
        double out = 0.0;
    };

    struct Take
    {
        /** empty, recording, looping, overdubbing, held: the row's own words. */
        std::string state = "empty";

        /*  What the audio thread closed it at, in seconds; nought until it
            says. Seconds and not samples, so the account needs no rate: a
            replay has no audio side to ask one of. */
        double length = 0.0;

        /** Layers on the take, the one being laid not counted. */
        int layers = 0;

        /** The loop, in seconds into the take. */
        double loopIn = 0.0;
        double loopOut = 0.0;

        /** The last thing it refused or did by itself, in a sentence. */
        std::string problem;

        /** Where the loop is playing, in seconds - the hook's, for the picture. */
        double playhead = 0.0;
    };

    class TakeTable
    {
    public:
        /** A channel's account; an empty take for one nothing has touched. */
        const Take& of (const std::string& channel) const;

        /*  A PRESS, applied to the account by §19.3's table. Never refused
            here - the command has decided that - and a press that moves
            nothing queues nothing: Loop on a take already looping, Undo on a
            take with no layer. `maxLayers` is the channel's `layers` row. */
        void press (const std::string& channel, TakeVerb verb, int maxLayers);

        /*  Whether a Rec or a layer asked now would find every layer in use:
            the one refusal the account alone decides (`layers-full`). */
        bool wouldStartLayer (const std::string& channel, TakeVerb verb) const;

        /*  WHAT THE AUDIO THREAD REPORTED (`take.closed`): the take closed at
            so many seconds - by a press, by filling its memory (`full`), or held as
            its cue let go - and the loop set to its two ends. A report for a
            take the account has already emptied is too late, and ignored. */
        void closed (const std::string& channel, double seconds, const std::string& how,
                     double longestSeconds);

        /*  ITS CUE LET THE CHANNEL GO: held silent, a take being recorded or a
            layer being laid closed as it is - never lost (§19.3: a take
            survives its cue, Esc and double Esc). */
        void release (const std::string& channel);

        /*  THE TAKE'S DOOR (decision CQ): a loop point moved, in seconds, kept
            inside the take and at least two crossfades before its partner.
            False for a take with no length to keep it in - one recording its
            first pass, or empty - which the door applies and ignores. */
        bool setPoint (const std::string& channel, bool inPoint, double seconds);

        /** The hook's: where the loop is playing, for the picture. */
        void setPlayhead (const std::string& channel, double seconds);

        /** Every channel with an account, emptied ones included - the hook reads them all. */
        std::vector<std::string> channels() const;

        /** The presses asked for since the last call, in order; the hook's. */
        std::vector<TakePress> takePresses();

        /** A replay's table queues nothing: there is no hook to place it. */
        void setQueueing (bool shouldQueue) noexcept    { queueing = shouldQueue; }

        /** Every take back to empty - a show closed, or another opened. */
        void clear();

        /** The two crossfades a loop is at least, in seconds (Looper.h). */
        static constexpr double shortestLoopSeconds = 0.020;

    private:
        void ask (const std::string& channel, TakeVerb verb, double in = 0.0, double out = 0.0);

        std::map<std::string, Take> takes;
        std::vector<TakePress> pending;
        bool queueing = true;
    };
}
