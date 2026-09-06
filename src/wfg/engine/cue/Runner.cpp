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
#include <wfg/engine/tree/MountProbe.h>
#include <wfg/engine/tree/MountSender.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/TickClock.h>
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

    int ticksFor (double seconds) noexcept
    {
        if (! (seconds > 0.0))
            return 0;

        const auto ticks = std::llround (seconds * static_cast<double> (TickClock::rateHz));

        return ticks > 0 ? static_cast<int> (ticks) : 0;
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
    std::string Runner::arm (Engine& engine, std::int64_t tick, const std::string& cueId,
                             const std::string& runId)
    {
        return armInternal (engine, tick, cueId, runId, false);
    }

    std::string Runner::fire (Engine& engine, std::int64_t tick, const std::string& cueId,
                              const std::string& runId)
    {
        return armInternal (engine, tick, cueId, runId, true);
    }

    std::string Runner::armInternal (Engine& engine, std::int64_t tick,
                                     const std::string& cueId,
                                     const std::string& runId, bool fireAtOnce)
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return {};

        const auto kind = kindOfCue (cue);

        /*  A group organises time, order and lifetime, and none of that exists
            until PR 3.3. Pressing GO on one is legal and makes nothing. */
        if (kind.empty() || kind == "group")
            return {};

        /*  ARMING IS A MEDIA IDEA. A fade takes over a level, a network cue
            writes a node, a memo is a line in the book: none of them has
            anything to make ready, so there is nothing for an arm to do and a
            run left sitting in `armed` would look like progress that was not
            happening. `audio.arm` refuses them at the command; this is the same
            answer for the standby path. */
        if (! fireAtOnce && kind != "media")
            return {};

        /*  DECISION N, 2026-09-06: a refire is decided per kind, and this is
            the half of it that says "not again".

            Media and groups are IGNORED - the GO is applied, standby has
            advanced, and the sounding instance carries on untouched. A fade or
            a stop RESTARTS, which is the takeover `beginFade` already
            implements from whatever level the target has reached. An osc, midi
            or memo cue gets a SECOND INSTANCE, because there is nothing to
            collide over: two messages is what firing twice means.

            ARMED IS NOT SOUNDING, and the difference is the whole point of
            arming ahead. A cue armed when it reached standby is sitting on a
            reserved voice with its media ready and no sound coming out; GO is
            what turns that into a launch. Treating it as "already running"
            would have made the fast path - the one the design exists for - the
            one where GO does nothing at all. A cue in its PRE-WAIT is the same
            case one step earlier, and is left alone for the same reason: it is
            already on its way. */
        if (kind == "media")
            if (const auto* live = runs.liveRunOf (cueId))
            {
                if (fireAtOnce && live->state == runState::armed)
                    if (auto* armed = runs.find (live->id))
                    {
                        armed->launchRequested = true;
                        armed->launchRequestedAtTick = tick;
                    }

                return live->id;
            }

        auto id = runId;

        if (id.empty())
            id = ids.generate();

        runs.create (id, cueId, kind);
        auto* run = runs.find (id);

        if (run == nullptr)
            return {};

        /*  THE WAITS, COPIED NOW. §4.10's rule applied to a duration: the run
            instantiates what the cue said when it was fired, so editing the cue
            during the wait changes the next run and not this one. */
        run->preWaitTicks = ticksFor (static_cast<double> (cue[juce::Identifier ("preWait")]));
        run->postWaitTicks = ticksFor (static_cast<double> (cue[juce::Identifier ("postWait")]));

        if (! fireAtOnce)
        {
            armMedia (engine, cue, id);
            return id;
        }

        /*  A PRE-WAIT DELAYS THE FIRING AND NOT THE ARMING, which is the whole
            reason it is worth having on a media cue: the seconds it waits are
            seconds the disk spends getting ready. So the voice is reserved and
            the file is mapped during the wait, and `run.fire` at the far end of
            it has nothing left to do but place the launch.

            Everything else waits with nothing to prepare, which is correct: a
            fade cannot take over a level before it is time to, and a network
            cue must not write early. */
        if (run->preWaitTicks > 0)
        {
            run->state = runState::waiting;
            run->dueTick = tick + run->preWaitTicks;

            if (kind == "media")
                armMedia (engine, cue, id);

            return id;
        }

        fireKind (engine, tick, cue, kind, id);
        return id;
    }

    //==============================================================================
    void Runner::fireNow (Engine& engine, std::int64_t tick, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished())
            return;

        const auto cue = document.findById (run->cue);

        if (! cue.isValid())
            return;

        /*  Out of the wait first, so that `fireKind` sees the state every other
            path sees. A media cue armed during its wait is still `armed` at this
            point and stays there until the launch is placed. */
        run->state = run->track >= 0 ? runState::armed : runState::playing;

        fireKind (engine, tick, cue, run->kind, runId);
    }

    void Runner::fireKind (Engine& engine, std::int64_t tick, const juce::ValueTree& cue,
                           const std::string& kind, const std::string& runId)
    {
        if (kind == "fade")
        {
            fireFade (cue, runId);
            return;
        }

        if (kind == "stop")
        {
            fireStop (cue, runId);
            return;
        }

        if (kind == "osc")
        {
            fireOsc (cue, runId);
            return;
        }

        if (kind == "memo")
        {
            /*  A MEMO IS A LINE IN THE BOOK, and now it is a line with a run.

                Not because a memo does anything, but because §3.6 makes "done"
                the thing a sequence group advances on, and a group whose second
                member is a note to the operator has to know when to move to the
                third. Giving every kind a run is what puts that answer in ONE
                place - the run table - instead of asking each kind's job list
                whether it has heard of an identifier.

                It ends on the NEXT tick, exactly as a network cue with `wait:
                none` does, because that is when a report is allowed to leave.
                Its pre-wait and post-wait work like anything else's, which is
                what makes a memo the cheapest way to put a pause in a sequence. */
            if (auto* run = runs.find (runId))
                run->state = runState::playing;

            finishing.push_back (runId);
            return;
        }

        if (kind != "media")
            return;

        auto* run = runs.find (runId);

        if (run == nullptr)
            return;

        run->launchRequested = true;
        run->launchRequestedAtTick = tick;

        /*  Armed already if it came through a pre-wait or through standby; this
            is the arm for a cue fired from cold, which is the case that pays
            the disk. */
        if (run->track < 0)
            armMedia (engine, cue, runId);
    }

    void Runner::armMedia (Engine& engine, const juce::ValueTree& cue, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->track >= 0)
            return;

        /*  NO AUDIO SIDE IS A COMPLETE CONFIGURATION, not a failure. A show
            replayed has no Player and must still create the run, advance
            standby and write the same log - only the sound is missing.

            IT IS ALSO WHAT KEEPS THE REPORTS BELOW HONEST. They are submitted
            from inside a command handler, which a replay re-runs - so they
            would arrive twice, once from the handler and once from the log. The
            return above is what stops that: a replay has no Player, so it never
            reaches them, and the log's copy is the only one. */
        if (audio == nullptr)
            return;

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
                           { osc::Value::string (runId),
                             osc::Value::string (runError::mediaMissing) });
            return;
        }

        /*  The lowest free voice, and a playing one is never stolen. Lowest
            rather than round-robin so that a show replayed puts the same cue on
            the same track and two logs of one session compare line for line. */
        const auto track = runs.lowestFreeTrack (audio->trackCount());

        if (track < 0)
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (runId),
                             osc::Value::string (runError::noTrack) });
            return;
        }

        /*  Where it goes, resolved through the buses the show declares, so the
            audio side never has to know what a bus is. */
        std::string problem;
        const auto routing = resolveRouting (cue, audio->channelsPerTrack(), problem);

        if (! problem.empty())
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (runId),
                             osc::Value::string (runError::badRoute) });
            return;
        }

        /*  Reserved from here, so a second arm on the same tick cannot pick the
            same voice. The audio side confirms with audio.armed once the graph
            and the disk are ready; until then the run is armed and silent. */
        run->track = track;

        /*  A run holding a voice is `armed`, whatever it was before. A cue in
            its pre-wait keeps `waiting` - the operator's answer to "what is that
            cue doing" is the wait, not the plumbing underneath it. */
        if (! run->isWaiting())
            run->state = runState::armed;

        ArmRequest request;
        request.runId = runId;
        request.track = track;
        request.mediaFile = file;
        request.levelDb = static_cast<double> (cue[juce::Identifier ("level")]);
        request.routing = routing;

        run->level = request.levelDb;

        audio->requestArm (request);
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
                        otherwise cost a hundred atomic stores per arm.

                        `exactlyEqual` rather than `==`, and it is not a
                        formality: the strict preset compiles our code with
                        -Wfloat-equal, and MSVC does not have that warning at
                        all - so this line built cleanly on the machine it was
                        written on and reddened the Linux job for four commits
                        before anybody read the log. The comparison IS exact and
                        is meant to be; saying so is what makes that reviewable
                        rather than suspicious. */
                    if (juce::exactlyEqual (gain, 0.0))
                        continue;

                    out.push_back ({ input, firstChannel + channel,
                                     static_cast<float> (gain) });
                }
        }

        return out;
    }

    //==============================================================================
    void Runner::fireFade (const juce::ValueTree& cue, const std::string& runId)
    {
        beginFade (cue[idProperty].toString().toStdString(),
                          cue[juce::Identifier ("target")].toString().toStdString(),
                          runId, "fade",
                          static_cast<double> (cue[juce::Identifier ("level")]),
                          static_cast<double> (cue[juce::Identifier ("duration")]),
                          fadeCurveFrom (cue[juce::Identifier ("curve")].toString().toStdString()),
                          false);
    }

    void Runner::fireStop (const juce::ValueTree& cue, const std::string& runId)
    {
        const auto verb = cue[juce::Identifier ("verb")].toString();

        /*  A HARD STOP IS A FADE OF NO LENGTH THAT ALSO STOPS. Saying it that
            way rather than writing a second code path means the two verbs
            cannot drift apart: the ordering, the reporting and the
            target-not-running case are written once and behave the same. */
        const auto seconds = verb == "fade"
                               ? static_cast<double> (cue[juce::Identifier ("duration")])
                               : 0.0;

        beginFade (cue[idProperty].toString().toStdString(),
                          cue[juce::Identifier ("target")].toString().toStdString(),
                          runId, "stop",
                          silenceDb, seconds,
                          fadeCurveFrom (cue[juce::Identifier ("curve")].toString().toStdString()),
                          true);
    }

    void Runner::beginFade (const std::string& selfCueId,
                            const std::string& targetCueId,
                            const std::string& selfRunId, const std::string& kind,
                            double toDb, double seconds, FadeCurve curve,
                            bool stopWhenDone)
    {
        juce::ignoreUnused (selfCueId, kind);

        /*  THE RUN ALREADY EXISTS, and did not before PR 3.1. Every kind's run
            is now created in one place - `armInternal` - so that a pre-wait can
            sit between "the cue was fired" and "the fade starts" without each
            kind having to learn about waits. What is left here is the fade.

            THE RUN BELONGS TO THE FADE CUE, not to the cue it is fading. A run
            says which cue it instantiates, and getting that wrong made
            liveRunOf answer with the fade's own run when asked about the media
            it was supposed to be moving - so the fade faded itself. That is
            true of the run `armInternal` made, for the same reason. */
        const auto self = selfRunId;
        auto* selfRun = runs.find (self);

        if (selfRun == nullptr)
            return;

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
            return;
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
    }

    void Runner::fireOsc (const juce::ValueTree& cue, const std::string& runId)
    {
        const auto self = runId;
        auto* selfRun = runs.find (self);

        if (selfRun == nullptr)
            return;

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
            return;
        }

        /*  NO MOUNT TABLE IS A COMPLETE CONFIGURATION. A replay has none and
            must still create the run and finish it on the tick the log says;
            what it cannot do is write somebody else's node, and there is
            nothing there to write. */
        if (mounts == nullptr)
        {
            sending.push_back (job);
            return;
        }

        const auto written = mounts->write (address, *value);

        if (! written.ok)
        {
            job.failure = written.reason;
            sending.push_back (job);
            return;
        }

        /*  IT REACHED THE TREE; NOW IT REACHES THE WIRE. The two are separate
            on purpose: the tree is what a client reads back and what a replay
            reproduces, and the socket is what the other box hears. A cue that
            updated one and not the other would be a lie in whichever direction
            somebody happened to look. */
        const auto* declaration = mounts->declarationOf (written.mountId);

        if (sender_ != nullptr && declaration != nullptr)
            job.ticket = sender_->queue (written.mountId,
                                         { declaration->host, declaration->port },
                                         address, written.value);

        if (job.wait == OscWait::verified)
        {
            job.address = address;
            job.mountId = written.mountId;
            job.expected = written.value;

            /*  FORGOTTEN BEFORE IT IS ASKED FOR, and this line is the whole
                difference between verifying and appearing to. Without it an
                answer the target gave to an earlier cue would still be sitting
                there, would match, and every verified cue on that node would
                report done without anybody being asked anything. */
            mounts->forgetReadback (address);

            if (const auto* node = mounts->nodeAt (address))
                job.typeTag = node->typeTags;

            if (declaration != nullptr)
            {
                job.host = declaration->host;
                job.queryPort = declaration->queryPort;
            }

            const auto seconds = static_cast<double> (cue[juce::Identifier ("timeout")]);
            job.ticksAllowed = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        }

        sending.push_back (job);
        return;
    }

    void Runner::advanceSends (Engine& engine)
    {
        for (auto& job : sending)
        {
            /*  Killed while it waited. A network cue holds no voice either, so
                the same gap as a fade's: `stopping` with nothing to act on it.
                A `verified` cue that somebody gave up on is the case - the
                device is not answering and the operator would like the show to
                stop asking. */
            const auto* selfRun = runs.find (job.self);

            if (selfRun != nullptr && selfRun->state == runState::stopping)
            {
                engine.submit (origin::engine, "run.ended", one (job.self));
                job.finished = true;
                continue;
            }

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

            /*  `verified` GOES AND ASKS, and keeps asking until the answer
                matches or the cue runs out of patience.

                The asking is somebody else's thread - an HTTP exchange with a
                device that has gone away costs its whole timeout to find out,
                and this is the tick thread. What arrives back is a
                `mount.readback` command applied like any other, which is what
                makes a verified cue replayable: the answer is in the log, and a
                replay re-injects it and reaches the same verdict on the same
                tick with no network in the room. */
            if (job.wait == OscWait::verified)
            {
                ++job.ticksWaited;

                if (const auto* answered = mounts != nullptr
                                             ? mounts->readbackOf (job.address) : nullptr)
                {
                    /*  COMPARED AS THE NODE'S OWN TYPE, exactly. osc::Value's
                        equality is identity and not numeric equivalence, so a
                        float32 0.5 and a double 0.5 are different answers - and
                        that is right: the client coerced what the target said
                        to the type the node declared, so anything that still
                        differs is a difference the device made. */
                    const auto matched = *answered == job.expected;

                    engine.submit (origin::engine,
                                   matched ? "run.ended" : "run.failed",
                                   matched ? one (job.self)
                                           : std::vector<osc::Value> {
                                               osc::Value::string (job.self),
                                               osc::Value::string (oscError::disagreed) });

                    job.finished = true;
                    continue;
                }

                if (job.ticksWaited > job.ticksAllowed)
                {
                    engine.submit (origin::engine, "run.failed",
                                   { osc::Value::string (job.self),
                                     osc::Value::string (oscError::timeout) });
                    job.finished = true;
                    continue;
                }

                /*  Asked again every tick, and the probe drops the duplicate
                    unless the last question has come back. "Keep one question
                    outstanding" rather than "ask fifty times a second". */
                if (asker != nullptr)
                    asker->ask ({ job.mountId, job.host, job.queryPort,
                                  job.address, job.typeTag });

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
            /*  KILLED, and this is the half of `run.kill` that did not exist.

                `enforceStops` below stops a voice, and a fade holds no voice -
                it holds a level and a schedule - so killing a fade's own run
                marked it `stopping` and changed nothing: the fade went on
                writing levels into somebody else's cue for the rest of its
                duration, and the run sat in `stopping` for ever because nothing
                was ever going to report it ended.

                THE STOP GOES WITH IT, and that is the difference between a kill
                and a takeover. A fade arriving over a fade-and-stop inherits the
                arrival, because the operator riding a level back up did not
                withdraw the stop (author, 2026-09-06). `run.kill` is the other
                thing entirely: it is the immediate path, the one Esc and
                double-Esc will be built on, and it asks nothing of the cue. Kill
                the run of a stop cue and the stop it was going to perform is
                what you killed.

                WHICH MEANS PUTTING THE TARGET BACK, and that is a decision taken
                here rather than one the sources settle - overrule it early. A
                stop cue marks its target `stopping` the moment it fires, because
                a cue on its way out should say so. If the stop is killed and the
                mark is left, `enforceStops` finds a run that is `stopping` and
                that no job is holding, and stops it - so killing a ten-second
                fade-and-stop would stop the cue INSTANTLY, which is the opposite
                of every reading of what was asked for.

                The other reading is that killing the fade leaves the stop behind
                and it lands at once. It is defensible, and it is not what
                `run.kill` says it does: it asks nothing of the cue, and stopping
                somebody else's sound is a great deal to ask. So the target goes
                back to playing, at whatever level the fade had reached - exactly
                where a killed plain fade leaves it. */
            const auto* selfRun = runs.find (job.self);

            if (selfRun != nullptr && selfRun->state == runState::stopping)
            {
                if (job.stopWhenDone)
                    if (auto* held = runs.find (job.target))
                        if (held->state == runState::stopping && ! held->stopIssued)
                            held->state = runState::playing;

                job.retired = true;
                engine.submit (origin::engine, "run.ended", one (job.self));
                continue;
            }

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
                {
                    /*  MARKED BEFORE IT IS ISSUED, so that enforceStops does
                        not come along on the next tick and issue a second one.
                        Two paths can stop a voice - a stop cue arriving here,
                        and a `run.kill` that nothing else is going to act on -
                        and the flag is what makes them one stop rather than
                        two. */
                    target->stopIssued = true;
                    audio->stop (target->track);
                }

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

    void Runner::advanceWaits (Engine& engine, std::int64_t tick)
    {
        /*  A WAIT COMING DUE IS A DECISION, so it is a record.

            The hook could simply do the firing here and save a command. It must
            not: `wfg replay` runs no hooks at all, so a wait that expired only
            in a hook would never expire on replay and every cue with a pre-wait
            would sit in `waiting` for ever. Submitting is what puts the moment
            in the log, and the log is what a replay has.

            Which is the same shape as everything else the Runner observes -
            `run.started`, `run.ended`, a read-back arriving. A wait elapsing is
            one more thing the machine noticed. */
        /*  A KILLED RUN THAT NOTHING IS GOING TO END.

            `run.kill` writes `stopping` over whatever the run was, which is
            right - it is on its way out and says so - but it means a run killed
            during its pre-wait has lost the only mark that said who was looking
            after it. A voice is stopped by `enforceStops`; a fade and a network
            cue are ended by their own job loops; a run that is waiting, holding
            a post-wait, or a memo between firing and reporting has NO owner at
            all, and before this it stayed `stopping` until the show closed -
            with any group holding on it holding for ever.

            So: anything `stopping` that holds no voice and that no job claims is
            ended here. The ownership test is what keeps this from reporting the
            same run twice, because the job loops run after this one in the same
            tick and will end the ones they own. */
        for (const auto& snapshot : runs.all())
        {
            if (snapshot.state != runState::stopping || snapshot.track >= 0)
                continue;

            const auto owned =
                std::any_of (running.begin(), running.end(),
                             [&snapshot] (const FadeJob& job) { return job.self == snapshot.id; })
                || std::any_of (sending.begin(), sending.end(),
                                [&snapshot] (const OscJob& job) { return job.self == snapshot.id; });

            if (! owned)
                engine.submit (origin::engine, "run.ended", one (snapshot.id));
        }

        for (const auto& snapshot : runs.all())
        {
            if (! snapshot.isWaiting() || tick < snapshot.dueTick)
                continue;

            engine.submit (origin::engine,
                           snapshot.state == runState::waiting ? "run.fire" : "run.done",
                           one (snapshot.id));
        }

        /*  And the runs that had nothing to wait for in the first place. A memo
            ends the tick after it fired; the list is drained whole because
            nothing can be added to it between here and the submit. */
        for (const auto& id : finishing)
            engine.submit (origin::engine, "run.ended", one (id));

        finishing.clear();
    }

    //==============================================================================
    void Runner::beforeTick (Engine& engine, std::int64_t tick)
    {
        /*  Fades run whether or not there is an audio side. A replay has none
            and must still move the run's level and finish the fade's run on the
            same ticks, or it would not reproduce the session it is replaying. */
        currentTick = tick;

        /*  ABOVE THE NULL-PLAYER GATE, all three of them, and that is not an
            ordering detail. `wfg serve` without `--hosted` has no Player at all
            and must still run a show made of memos, network cues and fades -
            which is the configuration a designer works in on a train, and the
            configuration every replay is in. A wait that only elapsed when
            there was a sound card would be a cue list that only worked in a
            theatre. */
        advanceWaits (engine, tick);
        advanceFades (engine, tick);
        advanceSends (engine);

        if (audio == nullptr)
            return;

        launchIfDue (engine, tick);
        enforceStops();
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

                /*  AND HOW LATE IT WAS, which until now nothing measured.

                    `run.late` has been registered, documented and tested since
                    PR 2.3 and had no producer, because the number is not
                    visible from here: this loop places the launch a fixed
                    number of ticks ahead of the tick it happens to run on, so
                    by its own arithmetic it is never late. What it did not
                    know was which tick the launch was ASKED FOR on.

                    The run knows, so the difference is arithmetic: the ticks
                    between the GO and the launch, in samples, in blocks. It is
                    zero for a cue that was armed and waiting, which is the
                    ordinary case and the one the design exists to produce; it
                    is the disk when somebody pressed GO on a cold cue.

                    ONE TICK IS NOT LATE, and subtracting it is the difference
                    between measuring the show and measuring the architecture. A
                    GO is applied in a tick's DRAIN and this hook runs BEFORE the
                    drain, so the earliest tick that can see a launch request is
                    the one after it - always, for every cue, including the ones
                    that were armed and ready. Counting that tick would report
                    seven blocks late for a cue that did exactly what it was
                    asked to.

                    So what is reported is the EXCESS over the best case, which
                    is what the number is for: zero when arming did its job, and
                    the disk when somebody pressed GO on a cold cue.

                    Reported only when there is something to report - a `late 0`
                    record for every cue in a show would be a log of the clock. */
                const auto lateTicks = tick - run->launchRequestedAtTick - 1;

                if (lateTicks > 0 && blockSize > 0)
                {
                    const auto blocks = (lateTicks * samplesPerTick) / blockSize;

                    if (blocks > 0)
                        engine.submit (origin::engine, "run.late",
                                       { osc::Value::string (run->id),
                                         osc::Value::int32 (static_cast<std::int32_t> (blocks)) });
                }
            }
        }
    }

    void Runner::enforceStops()
    {
        /*  A RUN THAT WAS ASKED TO STOP AND THAT NOBODY IS STOPPING.

            `run.kill` marks a run `stopping` and goes no further, deliberately:
            it is a command on the model, registered with the run table and
            nothing else, and it must stay callable from `wfg replay` where
            there is no audio side at all. Its own comment says the sound stops
            and the audio side reports it - which was true of every path except
            the one nobody had written. Nothing told the audio side.

            So a killed cue read `stopping` and went on playing until its file
            ran out. The black-box driver found it, and only because it asserted
            on the SAMPLES: the run reached `done` inside the timeout, because
            the file happened to end first, and every check about the model
            passed while four seconds of audio nobody wanted went to the
            outputs.

            THE STOP GOES HERE rather than in the command, for the reason every
            report does: this is the tick thread, which owns the Player, and a
            command handler is re-run by a replay. It is idempotent - Tracktion
            takes a second stop on a stopped voice quietly - and `observeEdges`
            below is what turns the silence into `run.ended`.

            A FADE-AND-STOP IS NOT THIS. That also marks its target `stopping`,
            and it has a job counting down to a stop of its own; stopping it
            here would land it at the moment the operator asked instead of at
            the end of the fade, which is the whole difference between the two
            verbs. So a run that some fade job is holding is left alone. */
        if (audio == nullptr)
            return;

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.state != runState::stopping
                  || snapshot.track < 0
                  || snapshot.stopIssued)
                continue;

            const auto held = std::any_of (running.begin(), running.end(),
                                           [&snapshot] (const FadeJob& job)
                                           {
                                               return job.stopWhenDone
                                                        && job.target == snapshot.id;
                                           });

            if (held)
                continue;

            if (auto* run = runs.find (snapshot.id))
                run->stopIssued = true;

            audio->stop (snapshot.track);
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
                        (CommandContext& context, const std::vector<osc::Value>& args)
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
                                                         runner.arm (engine, context.tick,
                                                                     cueId, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "go",
                        "Fires the focused list's standby cue and moves standby to the next one.",
                        { { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, &focus, withRun]
                        (CommandContext& context, const std::vector<osc::Value>& args)
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
                                                         runner.fire (engine, context.tick,
                                                                      standby, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "cue.fire",
                        "Fires a named cue without touching standby - what a button on a surface"
                        " does.",
                        { { "cue", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.fire (engine, context.tick,
                                                                      cueId, id)));
                        } });

        //----------------------------------------------------------------------
        /*  AND THE OTHER END OF A PRE-WAIT, which lives here for the reason
            `audio.arm` does: it is an ACTION. RunCommands holds what the machine
            says HAPPENED, and this makes something happen - a voice is launched,
            a level starts moving, a datagram goes out.

            It is a command rather than something the hook simply does, because
            `wfg replay` runs no hooks: a wait that expired only inside one would
            never expire on replay, and every cue with a pre-wait would sit in
            `waiting` for the length of the session. The record is what a replay
            has, so the record is what the moment has to be.

            ANYONE MAY SEND IT, deliberately, as with every engine-origin
            command: one a replay could not send would be one a replay could not
            reproduce. Sent early it does what the wait would have done, which is
            "fire this now" - a legitimate thing to want and the same thing
            `cue.fire` means. */
        registry.add ({ "run.fire",
                        "A run's pre-wait elapsed: fire it now. What the engine sends itself at the"
                        " far end of a wait.",
                        { { "run", 's', false } },
                        true,
                        [&engine, &runner] (CommandContext& context,
                                            const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            runner.fireNow (engine, context.tick, runId);
                            return Outcome::ok (args);
                        } });
    }
}
