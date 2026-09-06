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

#include <wfg/engine/cue/RunCommands.h>

#include <algorithm>

namespace wfg::cue
{
    //==============================================================================
    void registerRunCommands (CommandRegistry& registry, RunTable& runs)
    {
        //----------------------------------------------------------------------
        registry.add ({ "audio.armed",
                        "The audio side reports which track a run got. Sent by the engine once the"
                        " graph is ready to play it.",
                        { { "run", 's', false }, { "track", 'i', false } },
                        false,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            const auto track = args[1].getInt32();

                            if (track < 0)
                                return Outcome::rejected (reason::typeMismatch);

                            /*  A report that arrives after the run has finished
                                is dropped rather than resurrecting it. The
                                message thread and the tick thread run at their
                                own speeds, so a stop that overtook an arm is an
                                ordinary race and not a defect. */
                            if (run->isFinished())
                                return Outcome::ok (args);

                            run->track = track;

                            /*  The voice is real and the media is ready, which
                                is what a queued GO was waiting for. */
                            run->armConfirmed = true;
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.started",
                        "The sound started. Reported by the audio side on the tick it was observed.",
                        { { "run", 's', false } },
                        false,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            /*  Applied and ignored once it has finished, so a
                                report in flight when a stop landed cannot undo
                                the stop. */
                            if (! run->isFinished())
                                run->state = runState::playing;

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.ended",
                        "The run finished, or was stopped. Its track is free from this tick.",
                        { { "run", 's', false } },
                        false,
                        [&runs] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            /*  A failed run stays failed. Both are finished, but
                                one of them says why, and overwriting that with
                                "done" would throw away the only account of what
                                went wrong. */
                            if (run->state == runState::failed)
                                return Outcome::ok (args);

                            /*  ENDED IS NOT DONE WHEN THERE IS A POST-WAIT.

                                §3.6: a post-wait is "how long after completion
                                this cue reports done to its parent". The sound
                                has stopped, the message has gone - and the cue
                                is not finished, because something is holding on
                                it. Publishing `done` here and keeping a private
                                timer would tell every client the opposite of
                                what the sequence above it was doing.

                                Applied a second time it is idempotent the way
                                the rest of this file is: a run already holding
                                its post-wait keeps the deadline it has rather
                                than restarting it. */
                            if (run->postWaitTicks > 0 && ! run->isWaiting())
                            {
                                run->state = runState::postWait;
                                run->dueTick = context.tick + run->postWaitTicks;
                                return Outcome::ok (args);
                            }

                            if (run->state != runState::postWait)
                            {
                                run->state = runState::done;
                                run->endedAtTick = context.tick;
                            }

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.done",
                        "A post-wait elapsed, so the run now reports done to whatever was waiting"
                        " on it.",
                        { { "run", 's', false } },
                        false,
                        [&runs] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            /*  Applied and ignored on a run that has finished by
                                another road - killed during its post-wait, say.
                                Idempotent where it costs nothing, like every
                                other report here. */
                            if (run->isFinished())
                                return Outcome::ok (args);

                            run->state = runState::done;
                            run->endedAtTick = context.tick;
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.failed",
                        "The run never played, and this is why: no-track, media-missing or"
                        " bad-route.",
                        { { "run", 's', false }, { "reason", 's', false } },
                        false,
                        [&runs] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            const auto why = args[1].getString();

                            if (why.empty())
                                return Outcome::rejected (reason::typeMismatch);

                            /*  A run that already played and finished is not
                                retroactively a failure. Applied, so the report
                                is in the log where somebody can see that it
                                arrived too late to mean anything. */
                            if (run->state == runState::done)
                                return Outcome::ok (args);

                            run->state = runState::failed;
                            run->error = why;
                            run->track = -1;
                            run->endedAtTick = context.tick;

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.late",
                        "How many blocks the launch was late by. Zero is the ordinary case; the"
                        " number is what makes GO is instant a measurement rather than a claim.",
                        { { "run", 's', false }, { "blocks", 'i', false } },
                        false,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            const auto blocks = args[1].getInt32();

                            if (blocks < 0)
                                return Outcome::rejected (reason::typeMismatch);

                            /*  The WORST it was, not the last report. A run that
                                was 3 blocks late and then reported 0 was still
                                3 blocks late, and the number is only worth
                                publishing if it cannot be talked down. */
                            run->late = std::max (run->late, blocks);
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.kill",
                        "Stops a run now. The primitive Esc and double-Esc will use; it runs no"
                        " footers and asks nothing of the cue.",
                        { { "run", 's', false } },
                        true,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            /*  Killing something already finished is applied and
                                does nothing. An operator hitting the button
                                twice is not making a mistake worth a rejection,
                                and a client reconnecting has no way to know
                                what is still running. */
                            if (run->isFinished())
                                return Outcome::ok (args);

                            /*  `stopping` rather than `done`, because the sound
                                has not stopped yet: the audio side reports
                                `run.ended` when it actually has. Saying done
                                here would publish a silence that had not
                                happened. */
                            run->state = runState::stopping;
                            return Outcome::ok (args);
                        } });
    }
}
