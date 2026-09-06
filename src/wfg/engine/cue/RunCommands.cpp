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
        /*  A ROUND, MATERIALISED. §3.6 lets a group shuffle its members, play a
            subset of them and loop the result, and every one of those is a
            DECISION the engine takes rather than a fact the document states -
            taken with a random number generator, at a moment nobody typed.

            So it is written down. The record carries the seed the run is
            drawing from and the members in the order they will play, and a
            replay reads the round back rather than drawing one: the generator
            is consulted on the night and never again. That is the same
            guarantee a generated identifier has, applied to an order.

            A ROUND MAY BE EMPTY, which is why the tail is optional: every
            member disabled or pruned away is a group with nothing left to play,
            and §3.6 says it completes rather than spinning. */
        registry.add ({ "run.round",
                        "A group drew a round: the seed it is drawing from, and the members in"
                        " the order they will play.",
                        { { "run", 's', false }, { "seed", 'i', false },
                          { "cue", 's', true, true } },
                        false,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            run->seed = args[1].getInt32();
                            run->round.clear();

                            /*  PRUNED MEMBERS ARE LEFT OUT HERE TOO, and not
                                only where the round was drawn.

                                The two can arrive in either order within one
                                tick. The scheduler draws the round in the hook
                                and the operator's prune was submitted before
                                that hook ran, so the queue holds the prune
                                first - it takes the member out of a round that
                                does not exist yet, and this record then puts it
                                back. An operator who asked at 22:40:07 would
                                have watched the cue they had just dropped play
                                anyway, once, for reasons nothing on their
                                screen could explain.

                                Filtering in both places makes the result the
                                same whichever order they land in, which is what
                                a replay needs as much as the operator does. */
                            for (std::size_t i = 2; i < args.size(); ++i)
                            {
                                const auto& cueId = args[i].getString();

                                if (std::find (run->pruned.begin(), run->pruned.end(), cueId)
                                      == run->pruned.end())
                                    run->round.push_back (cueId);
                            }

                            /*  COUNTED HERE, because this is the one place a
                                round begins - live and on replay both. A
                                counter kept by the scheduler would be right
                                during a show and zero through every replay,
                                since a replay runs no hooks. */
                            ++run->iteration;
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  A TRANSITION, REPORTED WHEN THE BOUNDARY IS PLACED, which is the
            same moment `run.started` is reported for the same reason: the
            scheduler decides on a tick, and the tick it decided on is the one
            a replay has to reproduce. The sound follows a few blocks later,
            live and on replay both - except that on replay there is none.

            WHICH PASS is not here and never will be. §3.15 splits transitions
            from continuous readouts: entering a range is something the machine
            decided, and the third pass of eight is arithmetic on a sample
            counter. A four-hour bed would otherwise log a record every few
            seconds for something nobody chose. */
        registry.add ({ "run.range",
                        "A ranged media cue entered one of its ranges: the index, from nought.",
                        { { "run", 's', false }, { "index", 'i', false } },
                        false,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            /*  Applied and ignored on a run that has finished by
                                another road - killed while its last boundary was
                                in flight. Idempotent where it costs nothing,
                                like every other report here. */
                            if (run->isFinished())
                                return Outcome::ok (args);

                            run->range = args[1].getInt32();

                            /*  THE PASS COUNT GOES BACK TO ONE, and it is reset
                                here rather than by the hook that placed the
                                boundary, so that a replay - which runs no hooks
                                - does not leave the strip reading 7/8 for a
                                range that has just started. */
                            run->rangeIteration = run->range >= 0 ? 1 : 0;

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  AN OPERATOR COMMAND, unlike everything around it. §3.24 gives a
            ranged cue an `advance` - "leave the range you are on at the end of
            the pass you are on" - and it is the only way out of a range that
            loops for ever.

            IT IS A REQUEST AND NOT AN ACT. The scheduler places the boundary at
            the end of the current pass, which may be seconds away; what this
            does is set the flag it reads. That is the whole difference between
            `advance` and a hard stop, and it is why the two are separate verbs
            rather than one with a duration. */
        registry.add ({ "run.advance",
                        "Leaves the range playing now at the end of the pass it is on, and"
                        " continues into the next one. The way out of a loop that never ends.",
                        { { "run", 's', false } },
                        true,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            if (run->isFinished())
                                return Outcome::rejected (reason::typeMismatch);

                            /*  A RUN THAT IS NOT IN A RANGE HAS NOWHERE TO
                                ADVANCE TO, and saying so is better than a
                                silent no-op: an operator who aimed an advance at
                                the wrong cue has been told, and the log carries
                                the refusal. */
                            if (run->range < 0)
                                return Outcome::rejected (reason::typeMismatch);

                            run->advanceRequested = true;
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
        /*  PRUNING IS WHAT AN OPERATOR DOES AT 22:40, and it is deliberately
            not an edit.

            §3.6: a member can be dropped from the round in progress or from
            every round of this run, and either way the SHOW is untouched -
            tomorrow the cue is back. That is the difference between a note
            taken during a performance and a decision (§4.10), and it is why
            this lives on the run rather than on the cue: it evaporates when the
            run does, which is the behaviour somebody wants at the moment they
            reach for it and would have to remember to undo otherwise.

            `round` and `group` differ only in how long they last. A member
            pruned for the round is back in the next one; a member pruned for
            the group is gone for as long as this run is. Neither can reach a
            member already playing - it is a decision about what comes next. */
        registry.add ({ "run.prune",
                        "Drops a member from this run: for the round in progress, or for the"
                        " whole of it. The show is not edited.",
                        { { "run", 's', false }, { "cue", 's', false },
                          { "scope", 's', true } },
                        true,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            const auto& cueId = args[1].getString();
                            const auto scope = args.size() > 2 ? args[2].getString()
                                                               : std::string { "round" };

                            if (scope != "round" && scope != "group")
                                return Outcome::rejected (reason::badValue);

                            /*  Out of the round in progress either way, so that
                                a member not yet reached is not reached. */
                            run->round.erase (std::remove (run->round.begin(), run->round.end(),
                                                           cueId),
                                              run->round.end());

                            if (scope == "group"
                                  && std::find (run->pruned.begin(), run->pruned.end(), cueId)
                                       == run->pruned.end())
                                run->pruned.push_back (cueId);

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.unprune",
                        "Puts a pruned member back, from the next round on. The round in progress"
                        " is not redrawn.",
                        { { "run", 's', false }, { "cue", 's', false } },
                        true,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            const auto& cueId = args[1].getString();

                            /*  FROM THE NEXT ROUND, and the round in progress is
                                left alone. Putting a member back into an order
                                that has already been drawn - and possibly
                                already been passed - would be a cue arriving
                                somewhere nobody chose. The next round is drawn
                                from the members again and has it. */
                            run->pruned.erase (std::remove (run->pruned.begin(),
                                                            run->pruned.end(), cueId),
                                               run->pruned.end());

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  A STOP AIMED AT A RUN, which is §3.8's "may target a specific run
            pointer" - and the only way to say "that one" when a cue has several
            runs live at once, which decision 3 made possible for osc, midi and
            memo cues.

            THREE VERBS AND NOT FOUR. `hard` is the graceful stop §4.4 draws:
            the members come down and the FOOTER RUNS, because that is where a
            scene gives back what it was holding. `afterMember` and
            `afterIteration` are the two boundaries a group was going to reach
            anyway, which is how an infinite loop is left without a cut.

            `fade` is deliberately absent. A fade needs a run of its own to
            report through - it takes time, and something has to say when it
            arrived - and a command has no cue and so no run. A fade-and-stop is
            a STOP CUE, where the duration and the curve are authored values
            somebody decided rather than arguments typed at the moment of
            panic. */
        registry.add ({ "run.stop",
                        "Stops one run: now, at the end of the member playing, or at the end of"
                        " this round. The footer runs either way.",
                        { { "run", 's', false }, { "verb", 's', true } },
                        true,
                        [&runs] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            auto* run = runs.find (args[0].getString());

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            const auto verb = args.size() > 1 ? args[1].getString()
                                                              : std::string { "hard" };

                            if (verb != "hard" && verb != "afterMember"
                                  && verb != "afterIteration")
                                return Outcome::rejected (reason::badValue);

                            if (run->isFinished())
                                return Outcome::ok (args);

                            /*  A BOUNDARY IS ONLY A BOUNDARY IF SOMETHING IS
                                COUNTING. A group has members and rounds to
                                reach the end of; a media cue has neither, so
                                asking it to stop after its member is asking for
                                a boundary that does not exist - and the honest
                                answer is the stop that was asked for, now,
                                rather than a request quietly ignored. */
                            if (verb != "hard" && run->kind == "group")
                            {
                                run->stopAfter = verb == "afterMember" ? "member" : "iteration";
                                return Outcome::ok (args);
                            }

                            run->state = runState::stopping;
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
                            /*  AND IT SKIPS THE FOOTER. §4.4's second level:
                                immediate, drops everything, "runs no footers".
                                A stop cue is the other one - graceful, the same
                                path as normal completion entered early - and it
                                leaves this alone, so a group it stops still
                                releases what it was holding. */
                            run->skipFooter = true;
                            run->state = runState::stopping;
                            return Outcome::ok (args);
                        } });
    }
}
