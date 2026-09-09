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
    A network cue in flight: one write to somebody else's node, and the question
    of when it counts as done.

    THE WAIT IS THE WHOLE POINT OF THE CUE KIND (PRD §3.11). Sending a message
    is one line; knowing whether it arrived is what turns a list of OSC cues
    into a chain that can be relied on. A cue whose wait is `none` is what every
    other show program offers - fire and hope - and it is the right answer when
    the target is a lighting desk that will never say anything back. `sent` says
    the datagram left this machine, which is the strongest thing UDP itself can
    ever tell you.

    WHY A JOB AND NOT A FUNCTION CALL. The answer arrives on a later tick than
    the question, always: the send is queued during the tick that fires the cue
    and leaves at the end of it, so the earliest honest report is the next tick.
    A job is what carries the run's identity across that gap.

    AND WHY IT REPORTS FROM THE TICK HOOK. `wfg replay` re-injects every record
    the log holds and re-runs every command handler, so an engine-origin report
    submitted from inside a handler would arrive twice and a deterministic
    session would fail to reproduce itself. The hooks are not run by a replay at
    all. Same rule as a fade, and for the same reason - see Runner::advanceFades.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstdint>
#include <string>

namespace wfg::cue
{
    /** How long a network cue takes to be done. */
    enum class OscWait
    {
        /** Done the moment it is fired. */
        none,

        /** Done when the datagram has left the socket. */
        sent,

        /*  Done when the target has been asked and has answered with the value
            that was written.

            THE ONLY ONE OF THE THREE THAT CAN TELL YOU THE DEVICE DISAGREED,
            which is what makes a sequence of network cues a chain rather than a
            hope: a cue that reports done has been confirmed by the box it was
            aimed at, so the next one can be built on it. It is also the only
            one that can time out, and the only one that needs the target to run
            an OSCQuery server - which is why a mount has to declare that it
            does before a cue may ask for this (question K). */
        verified
    };

    /** From the document's spelling. Anything unknown reads as `none`. */
    OscWait oscWaitFrom (const std::string& text) noexcept;

    /** The reasons a network cue's run can end badly. */
    namespace oscError
    {
        /** Nothing answered in time. */
        inline constexpr const char* timeout = "timeout";

        /*  Something answered, with a different value.

            The one failure that means the device is there, is listening, and is
            not doing what it was told - a clipped range, a mode that ignores
            the parameter, a channel somebody re-patched. It is worth its own
            word because it sends a different person to look. */
        inline constexpr const char* disagreed = "disagreed";
    }

    //==============================================================================
    /*  One network cue in flight. A value the Runner holds and advances; it owns
        nothing and touches nothing.
    */
    struct OscJob
    {
        /** The run of the network cue itself. */
        std::string self;

        OscWait wait = OscWait::none;

        /*  What the sender called this message. Answered one tick later, which
            is why the job exists. Zero when nothing was queued - either there is
            no sender at all, which is a complete configuration, or the write was
            refused before it got that far. */
        std::uint64_t ticket = 0;

        /*  Why it failed, if it did. Set where the failure was noticed and
            reported where reporting is allowed, which is not the same place. */
        std::string failure;

        /*  READ BEFORE WRITE: the state a PREPARED network cue starts in.

            PRD §3.12's anticipation only works if it can be undone, and §13.1
            makes that the condition rather than a nicety: a value pre-sent to a
            target nobody can ask what it held is a value nobody can put back,
            so anticipating it would trade a saved moment for a desk left in a
            state the operator did not choose.

            So a prepared cue asks FIRST. The target is asked what it holds, the
            answer is kept on the run as the restore value, and only then is the
            new value written and verified exactly as an ordinary `verified` cue
            is. The existing path does the opposite - it forgets the remembered
            answer at the moment it writes, and asks afterwards - which is right
            for verification and is precisely wrong for a restore. The ORDER is
            the whole difference, and this flag is what carries it.

            It is not a fourth `OscWait`: the wait is what the DESIGNER asked
            for and this is a fact about when the engine got to the cue. A
            prepared cue with `wait: none` still reads first, because the
            restore value is not optional. */
        bool reading = false;

        /*  What is to be written once the read has come back. Empty otherwise:
            an ordinary cue writes at once and has nothing to remember. */
        osc::Value pending;

        /*  Whether the read has been asked for. ASKED ONCE, which is the
            opposite of what the verify does and is the difference between the
            two questions.

            A verify asks every tick and lets the probe drop the duplicates:
            what it wants is the CURRENT value, so a later answer is a better
            answer. A read wants the value from ONE moment - the one before
            anything was written - and asking again after the answer has been
            submitted but before it has been applied leaves a second answer in
            flight, which lands after the write and satisfies the verify that
            follows it. The cue then reports `disagreed` about a desk that
            agreed perfectly.

            A read that is never answered times out, nothing is pre-sent, and
            the block says `partial` - which is the safe direction: a value that
            could not be read is a value that must not be written early. */
        bool asked = false;

        //  --- verified only ------------------------------------------------
        /** The node being watched, and what it was written with. */
        std::string address;
        std::string mountId;
        std::string typeTag;
        osc::Value expected;

        /** Where to ask, copied so a reload cannot move the question. */
        std::string host;
        int queryPort = 0;

        /*  Ticks spent waiting, against `timeout` seconds turned into ticks.
            Zero total means the first answer or nothing, which is a real thing
            to want from a device on a local switch. */
        int ticksWaited = 0;
        int ticksAllowed = 250;

        bool finished = false;
    };
}
