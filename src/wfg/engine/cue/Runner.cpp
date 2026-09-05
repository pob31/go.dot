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

#include <wfg/engine/cue/Runner.h>

#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  Silence, spelled as the parameter table spells it. Written here
            rather than included from CueMatrix because this is the cue layer
            and it names no audio type - one number repeated is cheaper than a
            dependency that would let a Tracktion header in. */
        constexpr double silenceDb = -120.0;

        std::string kindOfCue (const juce::ValueTree& cue)
        {
            const auto element = cue.getType().toString();

            if (element == "Cue")   return "memo";
            if (element == "Group") return "group";
            if (element == "Media") return "media";
            if (element == "Fade")  return "fade";
            if (element == "Stop")  return "stop";
            if (element == "Osc")   return "osc";

            return {};
        }

        std::string mediaFileOf (const juce::ValueTree& cue)
        {
            return cue[juce::Identifier ("file")].toString().toStdString();
        }

        std::vector<osc::Value> one (const std::string& text)
        {
            return { osc::Value::string (text) };
        }

        /** The numbers of a gains list, in the document's own spelling. */
        std::vector<double> gainsOf (const juce::ValueTree& route)
        {
            std::vector<double> out;

            const auto text = route[juce::Identifier ("gains")].toString().toStdString();
            std::size_t i = 0;

            while (i < text.size())
            {
                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                    ++i;

                const auto start = i;

                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                    ++i;

                if (i > start)
                    out.push_back (osc::parseDouble (text.substr (start, i - start)).value_or (0.0));
            }

            return out;
        }
    }

    //==============================================================================
    int launchLatencyTicks (int blockSize, int samplesPerTick) noexcept
    {
        if (blockSize <= 0 || samplesPerTick <= 0)
            return 0;

        /*  ceil ((3 * blockSize - 1) / samplesPerTick), in integers, plus one
            tick of guard. See the derivation in the header. */
        const auto needed = 3 * static_cast<std::int64_t> (blockSize) - 1;
        const auto ticks = (needed + samplesPerTick - 1) / samplesPerTick;

        return 1 + static_cast<int> (ticks);
    }

    //==============================================================================
    Runner::Runner (const doc::ShowDocument& documentToRead, RunTable& runsToDrive,
                    doc::IdRegistry& runIds, Focus& focusToUse)
        : document (documentToRead), runs (runsToDrive), ids (runIds), focus (focusToUse)
    {
    }

    int Runner::latencyTicks() const noexcept
    {
        if (audio == nullptr)
            return 0;

        return launchLatencyTicks (audio->blockSize(), samplesPerTick);
    }

    //==============================================================================
    std::string Runner::arm (Engine& engine, const std::string& cueId, const std::string& runId)
    {
        return armInternal (engine, cueId, runId, false);
    }

    std::string Runner::fire (Engine& engine, const std::string& cueId, const std::string& runId)
    {
        return armInternal (engine, cueId, runId, true);
    }

    std::string Runner::armInternal (Engine& engine, const std::string& cueId,
                                     const std::string& runId, bool fireAtOnce)
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return {};

        const auto kind = kindOfCue (cue);

        /*  A fade and a stop act on a run somebody else started, so they take
            their own path - and they only make sense fired, never armed: there
            is nothing to make ready. */
        if (kind == "fade")
            return fireAtOnce ? fireFade (cue, runId) : std::string {};

        if (kind == "stop")
            return fireAtOnce ? fireStop (cue, runId) : std::string {};

        /*  And a network cue, which is the same shape: it acts on something
            that is not a track, so there is no voice to reserve and nothing to
            make ready. */
        if (kind == "osc")
            return fireAtOnce ? fireOsc (cue, runId) : std::string {};

        /*  A memo is a line in the book and a group has no runner until Phase 3.
            Both are legal things to press GO on; neither makes a run. */
        if (kind != "media")
            return {};

        if (const auto* live = runs.liveRunOf (cueId))
        {
            /*  ARMED IS NOT RUNNING, and the difference is the whole point of
                arming ahead. A cue armed when it reached standby is sitting on
                a reserved voice with its media ready and no sound coming out;
                GO is what turns that into a launch. Treating it as "already
                running" would have made the fast path - the one the design
                exists for - the one where GO does nothing at all.

                DECISION B, 2026-09-05, is about a cue that is actually
                SOUNDING: the GO is applied, standby has advanced, and the
                playing instance carries on untouched. No restart, and no second
                instance stacked on another voice. */
            if (fireAtOnce && live->state == runState::armed)
                if (auto* armed = runs.find (live->id))
                    armed->launchRequested = true;

            return live->id;
        }

        auto id = runId;

        if (id.empty())
            id = ids.generate();

        runs.create (id, cueId, kind);
        auto* run = runs.find (id);

        if (run == nullptr)
            return {};

        run->launchRequested = fireAtOnce;

        /*  NO AUDIO SIDE IS A COMPLETE CONFIGURATION, not a failure. A show
            replayed has no Player and must still create the run, advance
            standby and write the same log - only the sound is missing. */
        if (audio == nullptr)
            return id;

        const auto named = mediaFileOf (cue);

        /*  RESOLVED AGAINST THE BUNDLE, and checked here rather than three
            layers down. A cue naming a file the bundle does not have fails its
            RUN and never the load - a show with one missing sound is still a
            show somebody has to run tonight - and finding out at the arm rather
            than at the launch means the failure is reported while the operator
            is still reading the next line. */
        const auto file = named.empty() || mediaFolder.empty()
                            ? named
                            : juce::File (juce::String (mediaFolder))
                                  .getChildFile (juce::String (named))
                                  .getFullPathName().toStdString();

        if (named.empty()
              || (! mediaFolder.empty() && ! juce::File (juce::String (file)).existsAsFile()))
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (id),
                             osc::Value::string (runError::mediaMissing) });
            return id;
        }

        /*  The lowest free voice, and a playing one is never stolen. Lowest
            rather than round-robin so that a show replayed puts the same cue on
            the same track and two logs of one session compare line for line. */
        const auto track = runs.lowestFreeTrack (audio->trackCount());

        if (track < 0)
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (id),
                             osc::Value::string (runError::noTrack) });
            return id;
        }

        /*  Where it goes, resolved through the buses the show declares, so the
            audio side never has to know what a bus is. */
        std::string problem;
        const auto routing = resolveRouting (cue, audio->channelsPerTrack(), problem);

        if (! problem.empty())
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (id),
                             osc::Value::string (runError::badRoute) });
            return id;
        }

        /*  Reserved from here, so a second arm on the same tick cannot pick the
            same voice. The audio side confirms with audio.armed once the graph
            and the disk are ready; until then the run is armed and silent. */
        run->track = track;

        ArmRequest request;
        request.runId = id;
        request.track = track;
        request.mediaFile = file;
        request.levelDb = static_cast<double> (cue[juce::Identifier ("level")]);
        request.routing = routing;

        run->level = request.levelDb;

        audio->requestArm (request);
        return id;
    }

    //==============================================================================
    std::vector<Coefficient> Runner::resolveRouting (const juce::ValueTree& cue,
                                                     int trackChannels,
                                                     std::string& problem) const
    {
        std::vector<Coefficient> out;
        problem.clear();

        const auto audioNode = document.root().getChildWithName ("Audio");

        for (const auto& route : cue)
        {
            if (route.getType().toString() != "Route")
                continue;

            const auto busId = route[juce::Identifier ("bus")].toString().toStdString();
            const auto bus = document.findById (busId);

            /*  A destination naming a bus the show does not have is a routing
                the rig cannot honour. It fails the run rather than the load: a
                show with one mis-pointed cue is still a show somebody has to
                run tonight. */
            if (! bus.isValid() || bus.getType().toString() != "Bus"
                  || bus.getParent() != audioNode)
            {
                problem = "route names no bus of this show";
                return {};
            }

            const auto firstChannel = static_cast<int> (bus[juce::Identifier ("firstChannel")]);
            const auto width = static_cast<int> (bus[juce::Identifier ("width")]);

            if (width <= 0)
            {
                problem = "a bus of no width";
                return {};
            }

            const auto gains = gainsOf (route);

            /*  A cue routed nowhere yet is an ordinary state for a show being
                written, and it is silent rather than wrong. */
            if (gains.empty())
                continue;

            /*  THE SHAPE IS THE CHECK. A gains list is the cue's channels times
                the bus's width, row by row, so a length that does not divide by
                the width is not a shorter routing - it is a different one, and
                a client that wrote it meant something the show cannot do. */
            if (gains.size() % static_cast<std::size_t> (width) != 0)
            {
                problem = "gains do not divide by the bus width";
                return {};
            }

            const auto inputs = static_cast<int> (gains.size()) / width;

            if (inputs > trackChannels)
            {
                problem = "the cue is wider than a track";
                return {};
            }

            for (int input = 0; input < inputs; ++input)
                for (int channel = 0; channel < width; ++channel)
                {
                    const auto gain = gains[static_cast<std::size_t> (input * width + channel)];

                    /*  Zero coefficients are dropped rather than written. The
                        matrix starts silent, so a zero says nothing new - and a
                        destination list of a hundred mostly-zero numbers would
                        otherwise cost a hundred atomic stores per arm. */
                    if (gain == 0.0)
                        continue;

                    out.push_back ({ input, firstChannel + channel,
                                     static_cast<float> (gain) });
                }
        }

        return out;
    }

    //==============================================================================
    std::string Runner::fireFade (const juce::ValueTree& cue, const std::string& runId)
    {
        return beginFade (cue[idProperty].toString().toStdString(),
                          cue[juce::Identifier ("target")].toString().toStdString(),
                          runId, "fade",
                          static_cast<double> (cue[juce::Identifier ("level")]),
                          static_cast<double> (cue[juce::Identifier ("duration")]),
                          fadeCurveFrom (cue[juce::Identifier ("curve")].toString().toStdString()),
                          false);
    }

    std::string Runner::fireStop (const juce::ValueTree& cue, const std::string& runId)
    {
        const auto verb = cue[juce::Identifier ("verb")].toString();

        /*  A HARD STOP IS A FADE OF NO LENGTH THAT ALSO STOPS. Saying it that
            way rather than writing a second code path means the two verbs
            cannot drift apart: the ordering, the reporting and the
            target-not-running case are written once and behave the same. */
        const auto seconds = verb == "fade"
                               ? static_cast<double> (cue[juce::Identifier ("duration")])
                               : 0.0;

        return beginFade (cue[idProperty].toString().toStdString(),
                          cue[juce::Identifier ("target")].toString().toStdString(),
                          runId, "stop",
                          silenceDb, seconds,
                          fadeCurveFrom (cue[juce::Identifier ("curve")].toString().toStdString()),
                          true);
    }

    std::string Runner::beginFade (const std::string& selfCueId,
                                   const std::string& targetCueId,
                                   const std::string& selfRunId, const std::string& kind,
                                   double toDb, double seconds, FadeCurve curve,
                                   bool stopWhenDone)
    {
        /*  The fade cue gets a run of its own whatever happens next, because a
            fade IS a cue: pressing GO on it is a thing that happened, it needs
            an address while it runs, and a group will need it to finish (§3.6). */
        auto self = selfRunId;

        if (self.empty())
            self = ids.generate();

        /*  THE RUN BELONGS TO THE FADE CUE, not to the cue it is fading. A run
            says which cue it instantiates, and getting that wrong made
            liveRunOf answer with the fade's own run when asked about the media
            it was supposed to be moving - so the fade faded itself. */
        runs.create (self, selfCueId, kind);

        auto* selfRun = runs.find (self);

        if (selfRun == nullptr)
            return self;

        /*  RUNNING FROM THE TICK IT STARTS, because a fade has no arming phase:
            no voice to reserve and no file to make ready, so the `armed` a run
            is born in is a state a fade is never in. Left alone, a client
            watching /godot/run while a fade audibly moved a level would have
            read `armed` for the whole of it. */
        selfRun->state = runState::playing;

        /*  A TARGET THAT IS NOT RUNNING IS A SILENT NO-OP, applied rather than
            refused (§3.8). Fading something that already finished is what an
            operator does when a cue ended earlier than they expected, and it is
            not a mistake - there is simply nothing to fade. The fade's own run
            reports done at once, so a group waiting on it is not held up. */
        const auto* target = runs.liveRunOf (targetCueId);

        if (target == nullptr)
        {
            /*  A JOB WITH NOTHING TO FADE, rather than a report from here. The
                tick hook already knows what to do with a fade whose target has
                gone - it ends the fade's run - so this hands it the same
                situation instead of writing the answer twice. It also keeps the
                rule the hook exists for: only the tick hook reports. */
            FadeJob orphan;
            orphan.self = self;
            running.push_back (orphan);
            return self;
        }

        const auto targetId = target->id;
        const auto fromDb = target->level;

        /*  A FADE TAKES OVER FROM A FADE, from where the level HAS GOT TO and
            not from where the first one started. Anything else is a jump, and a
            jump on a PA is a click nobody can account for afterwards. */
        auto keepStopping = false;
        std::int64_t inheritedStopTick = 0;

        for (const auto& superseded : running)
        {
            if (superseded.target != targetId)
                continue;

            /*  A JOB A REPLAY LEFT BEHIND. `advanceFades` runs from the tick
                hook and a replay has none, so a fade that finished during the
                session is still sitting in this list while the session is being
                replayed. Its run ended when the log said it did, and ending it
                again would be an answer to a question nobody asked. */
            const auto* supersededRun = runs.find (superseded.self);

            if (supersededRun == nullptr || supersededRun->isFinished())
                continue;

            /*  AND THE FADE IT TOOK OVER FROM IS OVER. Its work belongs to
                somebody else now, so the run that reported that work ends -
                which is the rule the Runner already applies to a fade whose
                target has gone, arrived at from the other direction. A run left
                running would be waited on for ever by the group that owns it
                (§3.6), and would sit in /godot/run not moving for the rest of
                the show.

                QUEUED RATHER THAN SUBMITTED, because only the tick hook
                reports. See advanceFades. */
            supersededRuns.push_back (superseded.self);

            /*  BUT A STOP IS NOT A FADE, AND IT STILL HAPPENS (author,
                2026-09-06). A fade takes over the LEVEL; it does not call off
                the stop that was already coming.

                I had it the other way round for one commit, on the reasoning
                that dropping the job dropped the stop with it - which is a
                `remove_if` written for the level deciding a question about
                lifetime, and no way to decide anything. The author's answer is
                the one that survives contact with a show: the operator who
                fired a three-second stop and then rode the level back up did
                not withdraw the stop, and a cue that kept playing because
                somebody touched a fader would be a cue nobody could get rid of.

                So the scheduled arrival survives the takeover, at the tick it
                was always going to land on, and the run goes on reading
                `stopping` because it is. */
            if (superseded.stopWhenDone)
            {
                keepStopping = true;
                inheritedStopTick = superseded.stopsAtTick;
            }
        }

        running.erase (std::remove_if (running.begin(), running.end(),
                                       [&targetId] (const FadeJob& job)
                                       {
                                           return job.target == targetId;
                                       }),
                       running.end());

        FadeJob job;
        job.target = targetId;
        job.self = self;
        job.fromDb = fromDb;
        job.toDb = toDb;
        job.ticksTotal = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        job.curve = curve;
        job.stopWhenDone = stopWhenDone;

        /*  THE STOP THIS FADE INHERITED, if it took over from one.

            `stopsAtTick` is what the superseded job would have arrived at, kept
            as an absolute tick rather than as a remaining count so that it
            cannot drift: the stop lands when it was always going to land, and
            no arithmetic between here and there can move it.

            WHAT THE LEVEL DOES IN BETWEEN IS STILL OPEN. The new fade owns it,
            which is the simplest thing that honours the takeover; the author
            has raised the case where the two ramps should COMPOSE instead -
            a group fade over a member's, and relative fades summed on top of
            each other - and neither exists yet (`fade/@level` is a destination
            in dB, never an offset). When relative fades arrive this is the line
            that changes, and the stop's schedule is not what changes with it. */
        if (keepStopping)
        {
            job.stopWhenDone = true;
            job.stopsAtTick = inheritedStopTick;
        }
        else if (stopWhenDone)
        {
            /*  A stop of its own, landing when its own fade arrives. The two
                are the same number here and diverge only when somebody fades
                over the top of it. */
            job.stopsAtTick = currentTick + job.ticksTotal;
        }

        /*  A cue on its way out says so from the moment it is asked, not when
            the sound goes. `done` here would publish a silence that has not
            happened yet. */
        if (stopWhenDone)
            if (auto* stopping = runs.find (targetId))
                stopping->state = runState::stopping;

        running.push_back (job);
        return self;
    }

    std::string Runner::fireOsc (const juce::ValueTree& cue, const std::string& runId)
    {
        auto self = runId;

        if (self.empty())
            self = ids.generate();

        runs.create (self, cue[idProperty].toString().toStdString(), "osc");

        auto* selfRun = runs.find (self);

        if (selfRun == nullptr)
            return self;

        /*  Running from the tick it fires, like a fade: there is nothing to
            arm, so `armed` would be a state it is never in. */
        selfRun->state = runState::playing;

        OscJob job;
        job.self = self;
        job.wait = oscWaitFrom (cue[juce::Identifier ("wait")].toString().toStdString());

        const auto address = cue[juce::Identifier ("address")].toString().toStdString();
        const auto atom = cue[juce::Identifier ("value")].toString().toStdString();

        /*  THE VALUE IS SPELLED THE WAY THE LOG SPELLS ONE, and reusing that
            grammar is worth more than the four lines it saves. A document, a
            log record and a value on the wire then say the same thing the same
            way - so a cue can be written by copying the atom out of a log of
            the night somebody got it right by hand. */
        const auto value = osc::Value::fromAtom (atom);

        if (! value.has_value())
        {
            job.failure = reason::typeMismatch;
            sending.push_back (job);
            return self;
        }

        /*  NO MOUNT TABLE IS A COMPLETE CONFIGURATION. A replay has none and
            must still create the run and finish it on the tick the log says;
            what it cannot do is write somebody else's node, and there is
            nothing there to write. */
        if (mounts == nullptr)
        {
            sending.push_back (job);
            return self;
        }

        const auto written = mounts->write (address, *value);

        if (! written.ok)
        {
            job.failure = written.reason;
            sending.push_back (job);
            return self;
        }

        /*  IT REACHED THE TREE; NOW IT REACHES THE WIRE. The two are separate
            on purpose: the tree is what a client reads back and what a replay
            reproduces, and the socket is what the other box hears. A cue that
            updated one and not the other would be a lie in whichever direction
            somebody happened to look. */
        if (sender_ != nullptr)
            if (const auto* declaration = mounts->declarationOf (written.mountId))
                job.ticket = sender_->queue (written.mountId,
                                             { declaration->host, declaration->port },
                                             address, written.value);

        sending.push_back (job);
        return self;
    }

    void Runner::advanceSends (Engine& engine)
    {
        for (auto& job : sending)
        {
            if (! job.failure.empty())
            {
                engine.submit (origin::engine, "run.failed",
                               { osc::Value::string (job.self),
                                 osc::Value::string (job.failure) });
                job.finished = true;
                continue;
            }

            /*  `none` FINISHES WITHOUT ASKING ANYTHING, which is what makes it
                the right wait for a target that will never answer - a lighting
                desk, a projector, anything that takes a message and says
                nothing. It still finishes on the tick AFTER the cue fired,
                because that is when a report is allowed to leave, not because
                it waited for anything. */
            if (job.wait == OscWait::none || sender_ == nullptr || job.ticket == 0)
            {
                engine.submit (origin::engine, "run.ended", one (job.self));
                job.finished = true;
                continue;
            }

            /*  `sent` ASKS THE SENDER WHAT HAPPENED. The flush ran at the end of
                the tick that queued this, so the answer is here by now; still
                pending means the flush never ran, which is a wiring fault and
                not something to keep waiting on. Either way the cue reports
                what happened rather than what was asked for, which is the whole
                difference between this wait and the one above. */
            const auto outcome = sender_->outcomeOf (job.ticket);

            if (outcome == tree::MountSender::Outcome::sent)
                engine.submit (origin::engine, "run.ended", one (job.self));
            else
                engine.submit (origin::engine, "run.failed",
                               { osc::Value::string (job.self),
                                 osc::Value::string (runError::sendFailed) });

            job.finished = true;
        }

        sending.erase (std::remove_if (sending.begin(), sending.end(),
                                       [] (const OscJob& job) { return job.finished; }),
                       sending.end());
    }

    void Runner::advanceFades (Engine& engine, std::int64_t tick)
    {
        /*  ONLY THE TICK HOOK REPORTS, and this is where that rule is kept.

            `wfg replay` re-injects every record the session logged AND runs
            every command handler again. So an engine-origin report submitted
            from inside a handler arrives twice on replay - once from the log
            and once from the handler - and a session that was perfectly
            deterministic fails to reproduce itself. The hooks are not run by a
            replay at all: it applies the ticks the log HAS and skips the
            thousands between them, which would advance a fade at some rate
            that is not fifty a second. That is what makes a hook the safe place
            to report from, and a handler the wrong one.

            The rest of the Runner already obeyed this by accident - the media
            path returns before it reports when there is no audio side - and
            fades broke it, because fades run with no audio side by design.
            Fixture #5 is what found it, one commit after the takeover was
            written.

            So: fades taken over since the last tick end their runs here. */
        for (const auto& id : supersededRuns)
            engine.submit (origin::engine, "run.ended", one (id));

        supersededRuns.clear();

        for (auto& job : running)
        {
            /*  The LEVEL stops advancing when it arrives; the JOB may not be
                over, because it can still be holding a stop that is due later.
                Before the author settled that, the two were the same thing and
                one counter did for both. */
            if (! job.isFinished())
                ++job.ticksDone;

            auto* target = runs.find (job.target);

            /*  What was being faded has gone - it ended on its own, or somebody
                killed it. The fade has nothing left to do and says so, rather
                than writing levels into a voice that has moved on to another
                cue. */
            if (target == nullptr || (target->isFinished() && ! job.stopWhenDone))
            {
                job.retired = true;
                engine.submit (origin::engine, "run.ended", one (job.self));
                continue;
            }

            const auto level = job.currentDb();

            /*  THE MODEL AND THE SOUND, both every tick. The run's level is
                what a client watches; the atomic is what the audio interpolates
                between. NEITHER IS LOGGED - §3.15 keeps continuous readouts out
                of the log, and a replay recomputes them from the GO that
                started the fade and the document it read. */
            target->level = level;

            if (audio != nullptr && target->track >= 0)
                audio->setLevelDb (target->track, level);

            /*  WHAT THIS JOB IS WAITING FOR. A plain fade is done when its
                level arrives. A job carrying a stop is done when the STOP is
                due, which for a stop cue fired on its own is the same tick and
                for one a later fade took over from is the tick the original
                stop was always going to land on. */
            if (job.stopWhenDone ? tick < job.stopsAtTick : ! job.isFinished())
                continue;

            if (job.stopWhenDone)
            {
                /*  SILENT FIRST, THEN STOPPED, and the order is the whole point
                    of the fade verb: by the time the clip stops the level is
                    already at silence, so Tracktion's own click suppression has
                    nothing left to suppress. */
                if (audio != nullptr && target->track >= 0)
                    audio->stop (target->track);

                /*  With no audio side the sound cannot report its own end, so
                    the stop says it. A replay has to reach the same state as
                    the session it reproduces. */
                if (audio == nullptr)
                    engine.submit (origin::engine, "run.ended", one (target->id));
            }

            engine.submit (origin::engine, "run.ended", one (job.self));
            job.retired = true;
        }

        running.erase (std::remove_if (running.begin(), running.end(),
                                       [] (const FadeJob& job) { return job.retired; }),
                       running.end());
    }

    //==============================================================================
    void Runner::beforeTick (Engine& engine, std::int64_t tick)
    {
        /*  Fades run whether or not there is an audio side. A replay has none
            and must still move the run's level and finish the fade's run on the
            same ticks, or it would not reproduce the session it is replaying. */
        currentTick = tick;

        advanceFades (engine, tick);
        advanceSends (engine);

        if (audio == nullptr)
            return;

        launchIfDue (engine, tick);
        observeEdges (engine);
    }

    void Runner::launchIfDue (Engine& engine, std::int64_t tick)
    {
        juce::ignoreUnused (tick);

        const auto ticksAhead = latencyTicks();

        if (ticksAhead <= 0 || samplesPerTick <= 0)
            return;

        const auto now = audio->samplesElapsed();
        const auto blockSize = static_cast<std::int64_t> (audio->blockSize());

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || ! run->launchRequested || run->isFinished())
                continue;

            /*  Not until the audio side says the voice is ready. A launch
                placed before the disk has answered plays silence for as long as
                the disk takes, with the run reporting itself as playing
                throughout - the worst shape a failure can have. */
            if (! run->armConfirmed || run->track < 0)
                continue;

            /*  And not until the disk has answered either. The voice can be
                assigned long before the file is mapped, and a launch in that
                window is the worst kind of failure - silence, with the run
                cheerfully reporting itself as playing. */
            if (! audio->isArmReady (run->track))
                continue;

            /*  THE LAUNCH INSTANT. Placed a whole number of ticks ahead of the
                tick being processed, so it is a pure function of the tick index
                and the schedule - which is what makes it reproduce on replay
                rather than depending on when this loop happened to run. */
            const auto target = now + static_cast<std::int64_t> (ticksAhead) * samplesPerTick;

            /*  The guarantee the arithmetic exists to provide, checked rather
                than assumed. A tick thread that overslept would otherwise place
                a launch too close and buy a hole in the cue. */
            if (target - now < 2 * blockSize)
                continue;

            if (audio->launchAtSample (run->track, target))
            {
                run->launchRequested = false;
                run->launchedAtSample = target;

                engine.submit (origin::engine, "run.started", one (run->id));
            }
        }
    }

    void Runner::observeEdges (Engine& engine)
    {
        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->track < 0 || run->isFinished())
                continue;

            const auto playing = audio->isPlaying (run->track);

            /*  A run that was sounding and is not any more has ended. The
                launcher clip stops itself at the end of its length, which is
                the file's, so this is the ordinary way a cue finishes as well
                as how a stop is noticed. */
            if (run->sawPlaying && ! playing)
            {
                engine.submit (origin::engine, "run.ended", one (run->id));
                run->sawPlaying = false;
                continue;
            }

            if (playing)
                run->sawPlaying = true;
        }
    }

    //==============================================================================
    void registerGoCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                             doc::ShowDocument& document, Focus& focus,
                             doc::IdRegistry& runIds)
    {
        juce::ignoreUnused (runIds);

        const auto withRun = [] (std::vector<osc::Value> args, std::size_t index,
                                 const std::string& id)
        {
            if (args.size() > index)
                args[index] = osc::Value::string (id);
            else
                args.push_back (osc::Value::string (id));

            return args;
        };

        //----------------------------------------------------------------------
        /*  ARMING LIVES HERE, WITH THE RUNNER, because it is an ACTION and not
            a report: it reserves a voice and asks the audio side for media.
            RunCommands holds only what the machine says happened, which is what
            keeps that file applicable on a machine with no sound card.

            It is a command in its own right and not an internal step, because
            §4.11 says every gesture-reachable action is one - a surface that
            wants a cue ready before the operator's hand moves has to be able to
            ask. */
        registry.add ({ "audio.arm",
                        "Reserves a voice for a cue and makes its media ready, without playing"
                        " it. What standby does ahead of GO.",
                        { { "cue", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            /*  A cue that plays nothing cannot be made ready to
                                play. Arming a memo would create a run that could
                                never leave `armed`, which looks like progress
                                and is not. */
                            if (cue.getType().toString() != "Media")
                                return Outcome::rejected (reason::typeMismatch);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.arm (engine, cueId, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "go",
                        "Fires the focused list's standby cue and moves standby to the next one.",
                        { { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, &focus, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            const auto listId = list[idProperty].toString().toStdString();
                            const auto standby = list[juce::Identifier ("standby")]
                                                     .toString().toStdString();

                            /*  GO with nothing in standby is applied and does
                                nothing. An operator at the end of a list has
                                not made a mistake, and an R record every time
                                would bury the rejections that matter. */
                            if (standby.empty())
                                return Outcome::ok (args);

                            /*  STANDBY MOVES FIRST, and unconditionally (§3.5).
                                Whether the cue makes a sound, fails to find a
                                voice, or is a memo, the pointer has advanced -
                                which is what lets an operator press GO down a
                                list at speed without waiting to see what each
                                one did. */
                            const auto next = nextOf (list, standby);
                            document.setAttribute (standbyAddressOf (listId), next);

                            const auto id = args.empty() ? std::string {}
                                                         : args[0].getString();

                            return Outcome::ok (withRun (args, 0,
                                                         runner.fire (engine, standby, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "cue.fire",
                        "Fires a named cue without touching standby - what a button on a surface"
                        " does.",
                        { { "cue", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.fire (engine, cueId, id)));
                        } });
    }
}
