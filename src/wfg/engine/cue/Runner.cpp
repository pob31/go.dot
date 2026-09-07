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

#include <wfg/engine/midi/MidiMessages.h>

#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountProbe.h>
#include <wfg/engine/tree/MountSender.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

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
            if (element == "Midi")  return "midi";

            return {};
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

    std::string Runner::textOf (const juce::ValueTree& cue, const char* name) const
    {
        const auto id = cue[idProperty].toString().toStdString();

        if (id.empty())
            return {};

        return document.getAttribute ("/godot/cue/" + id + "/" + name).value_or (std::string {});
    }

    double Runner::numberOf (const juce::ValueTree& cue, const char* name) const
    {
        return osc::parseDouble (textOf (cue, name)).value_or (0.0);
    }

    namespace
    {
        /*  How many samples one pass of a range is.

            ROUNDED TO NEAREST rather than truncated, because the boundary is
            the sum of these and a truncation would accumulate: eight passes of
            a range whose length lands half a sample short would end four
            samples early, which over an eight-hour bed is a drift nobody could
            explain from the document. */
        std::int64_t samplesForRange (const RangeSpec& range, std::int64_t rate) noexcept
        {
            const auto seconds = range.out - range.in;

            if (! (seconds > 0.0) || rate <= 0)
                return 0;

            return static_cast<std::int64_t> (
                std::llround (seconds * static_cast<double> (rate)));
        }
    }

    std::vector<RangeSpec> Runner::rangesOf (const juce::ValueTree& cue) const
    {
        std::vector<RangeSpec> out;

        for (const auto& child : cue)
        {
            if (child.getType().toString() != "Range")
                continue;

            const auto id = child[idProperty].toString().toStdString();

            if (id.empty())
                continue;

            const auto value = [this, &id] (const char* name)
            {
                return document.getAttribute ("/godot/range/" + id + "/" + name)
                         .value_or (std::string {});
            };

            RangeSpec range;
            range.in = osc::parseDouble (value ("in")).value_or (0.0);
            range.out = osc::parseDouble (value ("out")).value_or (0.0);

            /*  One rather than nought when the row is absent, because the row's
                default is one pass and nought means FOR EVER. Reading a missing
                attribute as "loop this range for ever" would be the worst
                possible way to be wrong about it. */
            const auto loops = value ("loops");
            range.loops = loops.empty() ? 1 : std::atoi (loops.c_str());

            out.push_back (range);
        }

        /*  DOCUMENT ORDER IS PLAYLIST ORDER, which is why nothing sorts here:
            `range/index` is derived from exactly this walk, so the strip's
            numbering and the order the graph plays them in are one fact. */
        return out;
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

        if (kind.empty())
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
                        /*  AND IT STILL WAITS. A cue armed at standby has had
                            its voice and its file made ready; what it has not
                            had is its pre-wait, which starts when the cue is
                            FIRED and not when it was got ready. Firing it
                            straight away here would have made a pre-wait
                            something that only applied to cues nobody had
                            prepared - which is every cue, until the standby
                            started arming them, and then none of them.

                            Found by the arming: the moment standby armed a cue
                            ahead, the pre-wait test stopped seeing a wait. */
                        if (armed->preWaitTicks > 0)
                        {
                            armed->state = runState::waiting;
                            armed->dueTick = tick + armed->preWaitTicks;
                        }
                        else
                        {
                            armed->launchRequested = true;
                            armed->launchRequestedAtTick = tick;
                        }
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
        run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
        run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

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

    bool Runner::isManualGroup (const juce::ValueTree& cue) const
    {
        return cue.isValid()
                 && cue.getType().toString() == "Group"
                 && textOf (cue, "mode") != "timeline"
                 && textOf (cue, "advance") != "auto";
    }

    std::vector<std::string> Runner::fireStandby (Engine& engine, std::int64_t tick,
                                                  const juce::ValueTree& list,
                                                  const std::string& cueId,
                                                  const std::vector<std::string>& supplied)
    {
        std::vector<std::string> used;

        /*  The identifiers, supplied by a replay or drawn fresh. Kept in one
            place so that "the next one" means the same thing on both roads. */
        std::size_t taken = 0;

        const auto nextId = [&]
        {
            auto id = taken < supplied.size() ? supplied[taken] : std::string {};
            ++taken;

            if (id.empty())
                id = ids.generate();

            used.push_back (id);
            return id;
        };

        /*  THE GROUPS BETWEEN THE CUE AND THE LIST, outermost first. A member of
            a manual sequence plays as part of its group - §3.6 makes the group
            the thing that organises its members' time, order and lifetime - so
            every one of them has to be live before the member can be its child.

            Walking up and then reversing, because the document knows parents
            and not paths. */
        std::vector<juce::ValueTree> ancestors;

        for (auto node = document.findById (cueId).getParent();
             node.isValid() && node != list;
             node = node.getParent())
        {
            if (node.getType().toString() == "Group")
                ancestors.push_back (node);
        }

        std::reverse (ancestors.begin(), ancestors.end());

        std::string parentRun;

        /*  Whether this GO ENTERED a group rather than joining one already
            running. If it did, the group's job fires the member after the
            header and this must not fire it as well. */
        auto createdGroup = false;

        for (std::size_t level = 0; level < ancestors.size(); ++level)
        {
            const auto& group = ancestors[level];
            const auto groupId = group[idProperty].toString().toStdString();

            /*  ALREADY RUNNING IS THE ORDINARY CASE: the operator pressed GO on
                member one a moment ago, and members two onwards join the run
                that started then. */
            if (const auto* live = runs.liveRunOf (groupId))
            {
                parentRun = live->id;
                continue;
            }

            const auto id = nextId();

            runs.create (id, groupId, "group", parentRun);

            if (auto* run = runs.find (id))
            {
                run->preWaitTicks = ticksFor (numberOf (group, "preWait"));
                run->postWaitTicks = ticksFor (numberOf (group, "postWait"));

                /*  WHERE THIS LEVEL ENTERS: the member of THIS group that the
                    pointer is inside of, which is the next group down the path,
                    or the cue itself at the bottom. The pointer's own
                    identifier is a member of the innermost group and of nothing
                    above it, so handing it to every level gave each of those a
                    member it could not find and a fall back to member one - a
                    scene starting somewhere nobody asked for. */
                run->enterAt = level + 1 < ancestors.size()
                                 ? ancestors[level + 1][idProperty].toString().toStdString()
                                 : cueId;
            }

            /*  ONLY THE OUTERMOST IS FIRED HERE, and the rest are left standing.

                All of them are CREATED now, because the record has to carry
                every identifier this press produced and a replay never draws
                one of its own. Firing them too would start a scene from the
                inside out: the innermost group would spawn its member on the
                next tick while its parent was still running the header that is
                supposed to come first, and the parent's job - finding a child
                it had not started - would start a second one beside it.

                So each of them waits to be launched by its parent's job, at the
                moment §3.6 puts it; only the one with no group above it has
                nobody to do that. */
            if (parentRun.empty())
                fireKind (engine, tick, group, "group", id);

            parentRun = id;
            createdGroup = true;
        }

        /*  And the cue itself, as a child of the innermost group - or of
            nothing, when the pointer was at the top level all along. */
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return used;

        if (parentRun.empty())
        {
            const auto id = fire (engine, tick, cueId, nextId());

            /*  THE RUN IT ACTED ON, which is not always the one it drew.

                Standby arms ahead, so the ordinary GO launches a run that
                already existed - and `fire` answers with THAT identifier while
                the one drawn here goes unused. Recording the drawn one would
                put a number in the log that names nothing; recording what
                `fire` returned keeps the record's promise, which is that a
                replay never has to draw a number of its own.

                Empty means the cue makes no run at all, and then the record
                carries nothing rather than an identifier for a run that does
                not exist. */
            if (id.empty())
                used.pop_back();
            else
                used.back() = id;

            return used;
        }

        /*  Entering the group is the whole of this GO: the header runs and the
            job fires the member at the far end of it. */
        if (createdGroup)
            return used;

        /*  Decision N, 2026-09-06: a media cue that is already sounding is
            ignored - the GO is applied and logged, the pointer has advanced, and
            the playing instance carries on. */
        if (runs.liveRunOf (cueId) != nullptr && kindOfCue (cue) == "media")
            return used;

        /*  A member of a manual group is spawned INTO it, so the group waits for
            it, its footer runs after it, and killing the group takes it with it. */
        /*  SPAWNED AND NOT LAUNCHED. The group's job starts it on the next
            tick, because one launcher is better than two: `run.launch` begins a
            pre-wait, and a member started from both here and there would begin
            its wait twice. */
        const auto id = spawnChild (engine, parentRun, cueId, nextId());

        if (id.empty())
            used.pop_back();

        return used;
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

        if (kind == "midi")
        {
            fireMidi (cue, runId);
            return;
        }

        if (kind == "group")
        {
            /*  A GROUP RUN IS A SCHEDULER AND NOT A SOUND. It holds no voice
                and no level (§4.12: containers describe behaviour, content
                describes output), so what firing it creates is a job that will
                spawn its members and wait for them.

                `playing` from the moment it starts, because a group with a
                header running, or a member playing, or a footer to come, is
                doing something - and there is no other word for it that a
                client watching /godot/run would read correctly. */
            auto* run = runs.find (runId);

            if (run == nullptr)
                return;

            run->state = runState::playing;

            /*  HOW MANY ROUNDS, copied now and not read at each boundary.

                §3.6 says a mid-run toggle of `mode` or `advance` takes effect
                at the next member boundary, which is why those two are read
                from the document every tick - but a loop COUNT is not a
                behaviour, it is how long this run is going to be. Changing it
                under a running group would move a finish line the operator has
                already been told about, so it goes with the waits: copied at
                the start, and the edit reaches the next run. */
            run->iterations = static_cast<int> (numberOf (cue, "loops"));

            /*  ONE JOB PER RUN, and the guard is not defensive tidiness.

                A group run reaches here by two roads - `fireStandby`, for the
                one the pointer entered, and `run.launch` from its parent's job -
                and a run that took both would be scheduled twice: two jobs
                spawning the same members, launching them twice and ending them
                twice, all under one identifier. That is what a GO into a nested
                manual group produced, and it is cheap enough to make impossible
                rather than merely unlikely. */
            const auto already = std::any_of (scheduled.begin(), scheduled.end(),
                                              [&runId] (const GroupJob& other)
                                              {
                                                  return other.run == runId && ! other.retired;
                                              });

            if (already)
                return;

            GroupJob job;
            job.run = runId;
            job.enterAt = run->enterAt;
            scheduled.push_back (job);
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

        const auto named = textOf (cue, "file");

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
        request.levelDb = numberOf (cue, "level");
        request.routing = routing;
        request.ranges = rangesOf (cue);

        /*  NO SLOT, and it is a refusal rather than a truncation. The graph is
            built with as many launcher slots as the show's widest cue has
            ranges, once, when the show loads (§3.25) - so a range added during
            the show has nowhere to be armed. Arming the first S of them would
            be a cue that plays most of what it says, which is worse than one
            that says it cannot. */
        if (static_cast<int> (request.ranges.size()) > audio->slotCount())
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (runId),
                             osc::Value::string (runError::noSlot) });
            return;
        }

        /*  THE CUE'S AUTHORED LEVEL IS THE RUN'S OWN, which is what a fade
            aimed at this cue moves and what a trim from a group above it is
            added TO. `level` itself is left for applyLevels to compute on the
            next tick, so there is one place that decides what a run is heard
            at rather than two that could disagree. */
        run->ownLevel = request.levelDb;
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
                          textOf (cue, "target"),
                          runId, "fade",
                          numberOf (cue, "level"),
                          numberOf (cue, "duration"),
                          fadeCurveFrom (textOf (cue, "curve")),
                          false);
    }

    void Runner::fireStop (const juce::ValueTree& cue, const std::string& runId)
    {
        const auto verb = textOf (cue, "verb");

        /*  ADVANCE: THE THIRD GRACEFUL VERB, and the one that belongs to a
            ranged media cue rather than to a group.

            §3.24's range list may loop for ever, and `advance` is how it is got
            out of: the range playing now finishes the pass it is on and then
            leaves, into the next range or into silence. Which is the same shape
            as `afterMember` on a group - reach a boundary the scene was going
            to reach anyway - aimed at a different kind of boundary.

            IT IS NOT A STOP. A cue with three ranges advanced out of its first
            plays its second, and the run goes on. The stop cue's own run is
            over either way, because asking is all it does. */
        if (verb == "advance")
        {
            const auto* target = runs.liveRunOf (textOf (cue, "target"));

            if (target != nullptr)
                if (auto* run = runs.find (target->id))
                {
                    if (run->range >= 0)
                    {
                        run->advanceRequested = true;
                        finishing.push_back (runId);
                        return;
                    }

                    /*  Nothing to advance out of. A hard stop, for the reason
                        `afterMember` is one against a cue that is not a group:
                        there is no boundary to wait for, and a request quietly
                        ignored is worse than one honoured plainly. */
                    run->state = runState::stopping;
                }

            finishing.push_back (runId);
            return;
        }

        /*  TWO OF THE VERBS ASK FOR A BOUNDARY RATHER THAN FOR SILENCE.

            §3.6's infinite loop has to be leavable, and cutting it off mid-cue
            is exactly what an ambience bed exists not to do - so `afterMember`
            and `afterIteration` let the scene reach the end of the member
            playing now, or the end of this round, and stop there. The footer
            still runs, because leaving is leaving.

            Only a group has boundaries. Against anything else these are a hard
            stop, because there is nothing to wait for and a request quietly
            ignored is worse than one honoured plainly. */
        if (verb == "afterMember" || verb == "afterIteration")
        {
            const auto* target = runs.liveRunOf (textOf (cue, "target"));

            if (target != nullptr)
                if (auto* run = runs.find (target->id))
                {
                    if (run->kind == "group")
                    {
                        run->stopAfter = verb == "afterMember" ? "member" : "iteration";
                        finishing.push_back (runId);
                        return;
                    }

                    run->state = runState::stopping;
                }

            /*  The stop cue's own run is over either way: it asked, and the
                asking is all it does. Ended on the next tick like a memo,
                because that is when a report is allowed to leave. */
            finishing.push_back (runId);
            return;
        }

        /*  A HARD STOP IS A FADE OF NO LENGTH THAT ALSO STOPS. Saying it that
            way rather than writing a second code path means the two verbs
            cannot drift apart: the ordering, the reporting and the
            target-not-running case are written once and behave the same. */
        const auto seconds = verb == "fade"
                               ? numberOf (cue, "duration")
                               : 0.0;

        beginFade (cue[idProperty].toString().toStdString(),
                          textOf (cue, "target"),
                          runId, "stop",
                          silenceDb, seconds,
                          fadeCurveFrom (textOf (cue, "curve")),
                          true);
    }

    Runner::Takeover Runner::resolveTakeover (const std::string& targetId)
    {
        /*  WHAT THE JOBS ALREADY ON THIS TARGET MEANT, and what of it survives.

            Two separate things come out of this, and keeping them apart is why
            it is a function: which RUNS are over (their work belongs to somebody
            else now) and which SCHEDULE is inherited (a stop that was already
            coming). The first is about lifetime; the second is about time. */
        Takeover out;

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
                `stopping` because it is.

                A `run.kill` on the stop's own run is the other case entirely and
                is handled in advanceFades: that one abandons the stop, because
                kill asks nothing of the cue. */
            if (superseded.stopWhenDone)
            {
                out.keepStopping = true;
                out.stopsAtTick = superseded.stopsAtTick;
            }
        }

        running.erase (std::remove_if (running.begin(), running.end(),
                                       [&targetId] (const FadeJob& job)
                                       {
                                           return job.target == targetId;
                                       }),
                       running.end());

        return out;
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

        /*  FROM ITS OWN LEVEL, not from its effective one. A member inside a
            group trimmed to -6 dB is HEARD at -9 while its own level says -3,
            and a fade that took over from -9 would fold the trim into the base:
            the trim would then be counted twice while it lasted, and would be
            left behind in the member when the group released it. */
        const auto fromDb = target->ownLevel;

        /*  A FADE TAKES OVER FROM A FADE, from where the level HAS GOT TO and
            not from where the first one started. Anything else is a jump, and a
            jump on a PA is a click nobody can account for afterwards.

            RESOLVED BEFORE THE NEW JOB IS BUILT, in a step of its own, and the
            separation is worth the function it costs. Deciding what the old
            jobs meant and deciding what the new one is are two questions with
            one thing in common - a level - and PR 3.12 changes the answer to
            the second (a run's level becomes `base + Σ trims`, so a group fade
            composes with a member's rather than replacing it) without touching
            the first. Written as one block, that change would have had to be
            made in the middle of a loop that is also deciding lifetimes. */
        const auto takeover = resolveTakeover (targetId);

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
        if (takeover.keepStopping)
        {
            job.stopWhenDone = true;
            job.stopsAtTick = takeover.stopsAtTick;
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
        job.wait = oscWaitFrom (textOf (cue, "wait"));

        const auto address = textOf (cue, "address");
        const auto atom = textOf (cue, "value");

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

            const auto seconds = numberOf (cue, "timeout");
            job.ticksAllowed = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        }

        sending.push_back (job);
        return;
    }

    void Runner::fireMidi (const juce::ValueTree& cue, const std::string& runId)
    {
        const auto self = runId;
        auto* selfRun = runs.find (self);

        if (selfRun == nullptr)
            return;

        /*  Running from the tick it fires, like a fade and a network cue: there
            is nothing to arm, so `armed` is a state it is never in. */
        selfRun->state = runState::playing;

        /*  IT REUSES THE NETWORK CUE'S JOB, and the reason is that it is the
            same job. Both are "a message left this machine, and here is when
            this cue counts as done"; the only difference is which wire, and
            that is decided before the job exists. A second job type would be a
            second copy of the kill path, the report path and the wait, and the
            three would drift.

            `verified` is the one thing an OscJob can do that this cannot, and
            it is refused when the show LOADS rather than here - a wait for an
            answer that can never come would otherwise be a cue that hangs at
            half past seven. */
        OscJob job;
        job.self = self;
        job.wait = oscWaitFrom (textOf (cue, "wait"));

        if (job.wait == OscWait::verified)
            job.wait = OscWait::sent;

        midi::MessageSpec spec;
        spec.type = textOf (cue, "type");
        spec.channel = static_cast<int> (numberOf (cue, "channel"));
        spec.number = static_cast<int> (numberOf (cue, "number"));
        spec.data = static_cast<int> (numberOf (cue, "data"));
        spec.sysex = textOf (cue, "sysex");

        const auto built = midi::messageFor (spec);

        if (! built.ok())
        {
            job.failure = runError::badMessage;
            sending.push_back (job);
            return;
        }

        /*  NO SINK IS A COMPLETE CONFIGURATION, for the reason no mount table
            is: a replay has none and must still create the run and finish it on
            the tick the log says. What it cannot do is put bytes on a cable,
            and there is no cable. */
        if (midiOut == nullptr)
        {
            sending.push_back (job);
            return;
        }

        const auto problem = midiOut->send (textOf (cue, "port"), built.bytes);

        if (! problem.empty())
            job.failure = problem;

        sending.push_back (job);
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

            /*  THE RUN'S OWN LEVEL, and only that.

                What reaches the voice is `applyLevels` below, because what this
                fade wrote is not what the cue is heard at: a group above it may
                be trimming, and a group run has no voice of its own at all.
                Writing the atomic from here worked while a run's level was a
                value rather than a sum, and would now write the base where the
                effective level belongs.

                NEITHER IS LOGGED - §3.15 keeps continuous readouts out of the
                log, and a replay recomputes them from the GO that started the
                fade and the document it read. */
            target->ownLevel = level;

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
                                [&snapshot] (const OscJob& job) { return job.self == snapshot.id; })
                /*  A GROUP OWNS ITS OWN ENDING, and forgetting that here would
                    have been the sharp bug: a killed group holds no voice and no
                    job of the other two kinds, so this sweep would have ended it
                    on the spot - before `advanceGroups`, which runs after this
                    one, had killed a single member. The group would have read
                    `done` with its whole scene still playing underneath it. */
                || std::any_of (scheduled.begin(), scheduled.end(),
                                [&snapshot] (const GroupJob& job) { return job.run == snapshot.id; });

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

    std::vector<std::string> Runner::membersOf (const juce::ValueTree& group) const
    {
        std::vector<std::string> out;

        for (const auto& child : group)
        {
            if (! child.hasProperty (idProperty))
                continue;

            if (kindOfCue (child).empty())
                continue;

            /*  A DISABLED MEMBER IS SKIPPED, which Phase 1 deliberately did not
                do and said so where it asserted the opposite: "skipping is a
                running-behaviour decision that Phase 1 has no runner to
                justify". Phase 3 has the runner. A disabled cue is still a row
                in the list - it is not deleted, and the pointer can still be
                parked on it - but a group does not spawn it, because a member
                that plays nothing and is waited on for ever is the failure the
                whole completion table exists to avoid. */
            const auto id = child[idProperty].toString().toStdString();

            /*  THROUGH THE DOCUMENT AND NOT OFF THE VALUETREE, because the
                canonical writer OMITS an attribute holding its default and the
                reader leaves it absent - so a cue that has never had `enabled`
                written to it has no such property at all, and asking the tree
                directly answers `false` for every cue in the show. Which it
                did: the first version of this skipped every member of every
                group and the groups all completed instantly.

                `getAttribute` resolves the row and supplies the default, which
                is the whole reason the document has one door. */
            if (textOf (child, "enabled") == "false")
                continue;

            out.push_back (id);
        }

        return out;
    }

    std::string Runner::spawnChild (Engine& engine, const std::string& parentRun,
                                    const std::string& cueId, const std::string& runId)
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return {};

        const auto kind = kindOfCue (cue);

        if (kind.empty())
            return {};

        auto id = runId;

        if (id.empty())
            id = ids.generate();

        runs.create (id, cueId, kind, parentRun);

        auto* run = runs.find (id);

        if (run == nullptr)
            return {};

        run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
        run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

        /*  ARMED AND NOT LAUNCHED. A media member reserves its voice and asks
            for its file here, which is the whole reason spawning is a separate
            moment from launching: an auto sequence spawns the next member while
            the current one is still playing, so the disk is paid for before the
            chain arrives rather than after. Every other kind has nothing to make
            ready and simply waits in the state it was born in. */
        if (kind == "media")
            armMedia (engine, cue, id);

        return id;
    }

    void Runner::launchRun (Engine& engine, std::int64_t tick, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished())
            return;

        /*  The same fork the top-level path takes, and it has to be the same
            one: a member with a pre-wait waits exactly as a cue fired from
            standby does, and §2.4's rule that waits COMPOSE is what falls out
            of the group having its own on top. */
        if (run->preWaitTicks > 0)
        {
            run->state = runState::waiting;
            run->dueTick = tick + run->preWaitTicks;
            return;
        }

        fireNow (engine, tick, runId);
    }

    namespace
    {
        constexpr std::uint64_t goldenGamma = 0x9e3779b97f4a7c15ull;

        /*  SplitMix64, which is four lines and is what a shuffle seed wants: it
            turns a small integer into a well-spread stream, so a seed of 1 and
            a seed of 2 give unrelated orders.

            WRITTEN OUT RATHER THAN REACHED FOR. <random>'s engines are
            specified down to the bit and its DISTRIBUTIONS are not, and neither
            is `std::shuffle` - the same seed gives different orders on
            different standard libraries. A fixture drawn on this machine has to
            reproduce on the CI runners, so the order has to be a property of
            this file. */
        std::uint64_t splitMix (std::uint64_t& state)
        {
            state += goldenGamma;
            auto z = state;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            return z ^ (z >> 31);
        }

        /** Fisher-Yates, written out for the reason above. */
        void shuffleInPlace (std::vector<std::string>& items, std::uint64_t& state)
        {
            for (auto i = items.size(); i > 1; --i)
                std::swap (items[i - 1],
                           items[static_cast<std::size_t> (splitMix (state) % i)]);
        }
    }

    std::vector<std::string> Runner::drawRound (Engine& engine, const juce::ValueTree& group,
                                                const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr)
            return {};

        auto members = membersOf (group);

        /*  PRUNING IS RUN-LOCAL and is applied here rather than in `membersOf`:
            that one answers what the DOCUMENT says, and this one answers what
            this run is going to play. §3.6 - a pruned member is out for this
            round or for this run, and the show is untouched either way. */
        members.erase (std::remove_if (members.begin(), members.end(),
                                       [run] (const std::string& cueId)
                                       {
                                           return std::find (run->pruned.begin(),
                                                             run->pruned.end(), cueId)
                                                    != run->pruned.end();
                                       }),
                       members.end());

        if (members.empty())
            return {};

        /*  THE SEED THIS RUN IS DRAWING FROM, and it belongs to the RUN rather
            than to the round: the group's own when it has one, which is how a
            shuffled scene gets rehearsed - the same order every night - and a
            fresh one otherwise, so the show is different every night. Written
            into the log either way, so every night reproduces exactly.

            Every round of a run comes from that one seed mixed with the round
            number, which is what makes round three a pure function of the seed
            and the number three. A running state carried between rounds would
            do as well and would be one more thing that has to survive a replay
            for no reason. */
        /*  A MANUAL GROUP PLAYS ITS MEMBERS AS THEY ARE WRITTEN, and neither
            `selection` nor `play` reaches it.

            Both of those are the MACHINE choosing - which member comes next,
            and how many of them - and in a manual sequence the operator is the
            one choosing (§3.6: "a member starts on GO ... the operator is the
            parent"). The pointer walks the list in document order, so a
            shuffled round would have the group finishing at whichever member
            the draw happened to put last, at a moment the operator has no way
            to see coming; and "play two of five" would leave three rows the
            pointer walks through and nothing happens on.

            Ignored rather than refused at load, because the pair is meaningful
            the moment somebody makes the group automatic - which is a toggle
            §3.6 expects during tech. */
        const auto manual = textOf (group, "mode") != "timeline"
                              && textOf (group, "advance") != "auto";

        const auto shuffles = ! manual && textOf (group, "selection") == "shuffle";
        const auto authored = static_cast<std::int32_t> (numberOf (group, "seed"));

        /*  A GROUP THAT DOES NOT SHUFFLE DRAWS NOTHING, and reads zero. The
            seed would be unused, and an unused random number in every group's
            log is a number somebody will one day try to interpret - as well as
            the one thing in an otherwise identical pair of sessions that
            differs. */
        const auto seed = run->iteration > 0
                            ? run->seed
                            : (authored != 0 ? authored : (shuffles ? ids.drawSeed() : 0));

        auto state = static_cast<std::uint64_t> (seed)
                       ^ (static_cast<std::uint64_t> (run->iteration + 1) * goldenGamma);

        if (shuffles && members.size() > 1)
        {
            shuffleInPlace (members, state);

            /*  THE BOUNDARY CONSTRAINT (§3.6): a member is never heard twice
                running across a round boundary, so the first of the new round
                may not be the last of the one before. Redrawn until it is not -
                which with two members decides the order completely, and that is
                right rather than degenerate: alternating is what somebody
                asking for two shuffled ambiences means.

                Bounded, because a loop whose exit depends on a generator being
                fair is a loop that can hang a show. With more than one member a
                redraw succeeds with probability at least 1/n, so sixteen tries
                is a certainty that does not rely on the generator at all. */
            const auto last = run->round.empty() ? std::string {} : run->round.back();

            for (int tries = 0; tries < 16 && ! last.empty() && members.front() == last; ++tries)
                shuffleInPlace (members, state);
        }

        /*  PLAY N OF M (§3.6). Nought is all of them, and more than there are
            is all of them too rather than a refusal: deleting a member should
            not stop a show loading. */
        const auto play = manual ? std::size_t { 0 }
                                 : static_cast<std::size_t> (std::max (0.0,
                                                                       numberOf (group, "play")));

        if (play > 0 && play < members.size())
            members.resize (play);

        std::vector<osc::Value> args { osc::Value::string (runId),
                                       osc::Value::int32 (seed) };

        for (const auto& cueId : members)
            args.push_back (osc::Value::string (cueId));

        engine.submit (origin::engine, "run.round", std::move (args));
        return members;
    }

    void Runner::advanceGroups (Engine& engine)
    {
        /*  THE HOOK DECIDES AND THE HANDLER APPLIES, which is why nothing here
            changes a run: every decision leaves as a command. `wfg replay`
            re-injects every record AND re-runs every handler, so a scheduler
            that acted directly would act twice on replay - and one that acted
            only in the hook would not act at all, because a replay runs no
            hooks. Submitting is the only shape that is right in both.

            It costs a tick at every boundary. A member's `run.ended` is applied
            in tick n's drain, this sees it at n+1 and submits, and the launch
            goes in at n+2 - which is `/godot/engine/sequenceGapTicks`, published
            rather than left for somebody to discover with a stopwatch. §3.6's
            sequence group is discrete children relaunched; the sample-accurate
            join is §3.24's range, which is a different mechanism on purpose.

            THREE PHASES AND ONE PIECE OF MACHINERY. A header, the members and a
            footer are all "a list of cues, spawned in order, waited on"; what
            differs is which list, and the one case that launches everything at
            once instead of one at a time (a timeline group's members). So the
            phases share `phaseCues` and the spawn/await loop rather than having
            three copies that could come to disagree about what "done" means. */
        for (auto& job : scheduled)
        {
            auto* run = runs.find (job.run);

            if (run == nullptr || run->isFinished())
            {
                job.retired = true;
                continue;
            }

            const auto group = document.findById (run->cue);

            if (! group.isValid())
            {
                engine.submit (origin::engine, "run.ended", one (job.run));
                job.retired = true;
                continue;
            }

            /*  ASKED TO STOP, and which way decides whether the footer runs.

                A STOP CUE IS GRACEFUL AND RUNS THE FOOTER: it is the same path
                as normal completion entered early, which is exactly what §4.4
                promises of Esc - "a group aborted at 04:12 releases its channels
                and kills its LFOs exactly as it would have at 06:00". The
                footer is where that releasing lives, and skipping it would leave
                the channels held by a scene that has gone.

                `run.kill` SKIPS IT: the immediate path, which "runs no footers
                and asks nothing of the cue", and which Phase 10's double-Esc
                will be built on. The two are told apart by the flag `run.kill`
                sets, because both write the same `stopping` state and the state
                alone cannot say which was meant. */
            if (run->state == runState::stopping && job.phase != groupPhase::footer)
            {
                for (const auto* child : runs.childrenOf (job.run))
                    if (! child->isFinished())
                        engine.submit (origin::engine, "run.kill", one (child->id));

                if (! runs.allChildrenFinished (job.run))
                    continue;

                if (run->skipFooter || ! beginPhase (engine, job, group, groupPhase::footer))
                {
                    engine.submit (origin::engine, "run.ended", one (job.run));
                    job.retired = true;
                }

                continue;
            }

            if (job.phase == groupPhase::entering)
            {
                if (beginPhase (engine, job, group, groupPhase::header))
                    continue;

                if (beginPhase (engine, job, group, groupPhase::members))
                    continue;

                /*  Nothing to run at all - no header, no members. Complete
                    rather than stuck: §3.6 says an emptied round completes the
                    group rather than spinning, and this is the same answer one
                    level up. The footer still runs, because a group that
                    reserved nothing may still have a footer that says so. */
                if (! beginPhase (engine, job, group, groupPhase::footer))
                {
                    engine.submit (origin::engine, "run.ended", one (job.run));
                    job.retired = true;
                }

                continue;
            }

            /*  THE ROUND IS THE RUN'S, and it is re-read rather than copied
                once. `run.prune` takes a member out of the round in progress -
                which is the whole of what an operator wants at 22:40, and is
                useless if the scheduler is working from a list it took a copy
                of before they asked.

                Which also settles where the round LIVES: on the run, written by
                the command that drew it, read here. The copy the phase starts
                with is the same list one tick earlier, because the record has
                not been applied yet when `beginPhase` returns. */
            if (job.phase == groupPhase::members && run->iteration > 0)
                job.phaseCues = run->round;

            /*  AN EMPTIED ROUND COMPLETES THE GROUP (§3.6) rather than spinning
                on nothing - which is what pruning the last member of an
                infinite loop leaves behind. */
            if (job.phase == groupPhase::members && job.phaseCues.empty())
            {
                endOfRound (engine, job, group);
                continue;
            }

            const auto timeline = job.phase == groupPhase::members
                                    && textOf (group, "mode") == "timeline";

            /*  THE CHILDREN OF THIS PHASE, and not every child of the run.

                A group run collects its header's runs, its members' and its
                footer's under one parent, which is what makes killing it take
                the whole scene - and it means "the children" is the wrong set
                for any single phase to reason about. A header phase that
                launched the first armed child would launch the member a
                descending GO had already created, in place of the header cue it
                was there to run; a timeline phase counting children against its
                own member list would think it had spawned them all one short.

                So each phase asks about the runs of its own cues. The rest are
                still the run's children, and still die with it. */
            const auto inPhase = [&job] (const std::string& cueId)
            {
                return std::find (job.phaseCues.begin(), job.phaseCues.end(), cueId)
                         != job.phaseCues.end();
            };

            /*  A run is CLAIMED ONCE, by whichever phase was running the cue it
                belongs to when it appeared - which is what keeps round two from
                inheriting round one's finished runs, since both rounds play the
                same cues. */
            for (const auto* child : runs.childrenOf (job.run))
                if (inPhase (child->cue) && ! job.hasClaimed (child->id))
                {
                    job.claimed.push_back (child->id);
                    job.phaseRuns.push_back (child->id);
                }

            std::vector<const Run*> children;

            for (const auto& id : job.phaseRuns)
                if (const auto* child = runs.find (id))
                    children.push_back (child);

            const auto allFinished = [&children]
            {
                return std::all_of (children.begin(), children.end(),
                                    [] (const Run* child) { return child->isFinished(); });
            };

            /*  ASKED TO STOP AT A BOUNDARY, and this is the near one: the end
                of whatever is playing now. `afterIteration` is the far one and
                is read by `endOfRound`, which simply does not start another.

                Guarded on the phase having started something, because "nothing
                of this phase is running" is also true of a phase that has not
                begun - and a group told to stop after its member should not
                vanish before the member exists. */
            if (job.phase == groupPhase::members
                  && run->stopAfter == "member"
                  && ! children.empty()
                  && allFinished())
            {
                finishPhase (engine, job, group);
                continue;
            }

            /*  A TIMELINE SCHEDULES EVERYTHING AT ENTRY and each member's
                pre-wait is its OFFSET from that moment (§3.6) - which is why
                raising the group's own pre-wait defers a whole scene without
                disturbing the relative timing somebody spent an afternoon
                getting right. The launches are a tick after the spawns because
                the identifiers do not exist until the spawns have applied. */
            if (timeline)
            {
                for (auto i = job.launched; i < children.size(); ++i)
                    engine.submit (origin::engine, "run.launch", one (children[i]->id));

                job.launched = children.size();

                if (children.size() >= job.phaseCues.size() && allFinished())
                    endOfRound (engine, job, group);

                continue;
            }

            /*  A MANUAL SEQUENCE DOES NOT ADVANCE ITSELF. §3.6: "manual - a
                member starts on GO ... the operator is the parent." So once its
                header is done the job spawns nothing and waits; each GO on the
                member the pointer has reached creates that member's run as a
                child, and the job simply notices when the last one is finished.

                A header and a footer are always sequences and always automatic,
                whatever the group says: they are the group's own preparation and
                release, and an operator does not step through them. */
            const auto manual = job.phase == groupPhase::members
                                  && textOf (group, "advance") != "auto";

            if (manual)
            {
                /*  THE JOB IS THE ONLY THING THAT LAUNCHES, whoever asked for
                    the member. GO creates the child - so that the record
                    carries its identifier and a replay re-supplies it - and the
                    job starts it on the next tick, which is the same tick
                    budget every other member boundary costs.

                    One launcher rather than two because `run.launch` begins a
                    pre-wait, and a member launched twice would begin its wait
                    twice. */
                for (auto i = job.launched; i < children.size(); ++i)
                    engine.submit (origin::engine, "run.launch", one (children[i]->id));

                job.launched = children.size();

                /*  DONE WHEN THE LAST MEMBER HAS BEEN FIRED AND HAS FINISHED,
                    and both halves are needed. A manual group between GOs looks
                    exactly like one that is over - no child is running either
                    way - so "nothing is running" cannot be the test. What tells
                    them apart is whether the last member was ever started. */
                const auto& last = job.phaseCues.back();

                const auto lastFired =
                    std::any_of (children.begin(), children.end(),
                                 [&last] (const Run* child) { return child->cue == last; });

                if (lastFired && allFinished())
                    endOfRound (engine, job, group);

                continue;
            }

            /*  A sequence, which a header and a footer always are: launch what
                was spawned, wait for it, then spawn the next. */
            if (job.awaiting.empty())
            {
                for (const auto* child : children)
                    if (child->state == runState::armed)
                    {
                        job.awaiting = child->id;
                        engine.submit (origin::engine, "run.launch", one (child->id));
                        break;
                    }

                continue;
            }

            const auto* awaited = runs.find (job.awaiting);

            if (awaited == nullptr || ! awaited->isFinished())
                continue;

            job.awaiting.clear();

            if (job.nextMember < job.phaseCues.size())
            {
                engine.submit (origin::engine, "run.spawn",
                               { osc::Value::string (job.run),
                                 osc::Value::string (job.phaseCues[job.nextMember]) });
                ++job.nextMember;
                continue;
            }

            endOfRound (engine, job, group);
        }

        scheduled.erase (std::remove_if (scheduled.begin(), scheduled.end(),
                                         [] (const GroupJob& job) { return job.retired; }),
                         scheduled.end());
    }

    bool Runner::beginPhase (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                             const char* phase)
    {
        /*  THE MEMBERS PHASE PLAYS A ROUND, which is not the same list as the
            group's members: it may be shuffled, it may be a subset (§3.6's
            "play N of M"), and it may have had a member pruned out of it for
            tonight. A header and a footer are always themselves, in order. */
        const auto cues = phase == groupPhase::members
                            ? drawRound (engine, group, job.run)
                            : membersOf (group.getChildWithName (phase == groupPhase::header
                                                                   ? "Header" : "Footer"));

        /*  AN ABSENT OR EMPTY PHASE IS SKIPPED RATHER THAN ENTERED, and saying
            so with a `false` is what lets the caller fall through to the next
            one. A group with no header should not spend a tick in a header. */
        if (cues.empty())
            return false;

        job.phase = phase;
        job.phaseCues = cues;
        job.nextMember = 0;
        job.launched = 0;
        job.awaiting.clear();
        job.phaseRuns.clear();

        const auto timeline = phase == groupPhase::members
                                && textOf (group, "mode") == "timeline";

        if (timeline)
        {
            for (const auto& cue : cues)
                engine.submit (origin::engine, "run.spawn",
                               { osc::Value::string (job.run), osc::Value::string (cue) });

            job.nextMember = cues.size();
            return true;
        }

        const auto childRuns = runs.childrenOf (job.run);

        /*  A MANUAL SEQUENCE STARTS WHERE THE OPERATOR WAS, which is member one
            in every ordinary case - the pointer descends to it and GO there is
            what created the group - and is not member one when `standby.set`
            put the pointer somewhere else. Firing member one then would start a
            scene at a place nobody asked for.

            After this the group spawns nothing on its own: each GO creates the
            member the pointer has reached. */
        auto first = std::size_t { 0 };

        /*  KEPT UNTIL THE MEMBERS BEGIN. A header is the group's own
            preparation and runs before the members whatever the pointer was on,
            so what the operator asked for has to still be here when its turn
            comes - which it was not, because entering the header cleared it. */
        if (phase == groupPhase::members && ! job.enterAt.empty())
        {
            const auto at = std::find (cues.begin(), cues.end(), job.enterAt);

            if (at != cues.end())
                first = static_cast<std::size_t> (at - cues.begin());

            job.enterAt.clear();
        }

        /*  ALREADY THERE, which happens for one member and only when a GO
            descended into this group: the run for the member the pointer is
            inside of was created by that press, so the record could carry its
            identifier. Spawning a second would leave two runs of one cue under
            one parent - a scene playing twice, out of step with itself. It is
            adopted instead: not spawned, and started by the launch above like
            any other member.

            UNCLAIMED, which is the word that makes this survive a loop. A
            group's second round plays the same cues as its first, so "is there
            a child for this cue" answers yes on every round after the first -
            and the round would adopt a run that finished a minute ago and then
            wait for it to finish again, for ever. What is being asked is
            whether something has appeared that no phase has taken charge of
            yet, and the job records exactly that. */
        const auto unclaimed =
            std::any_of (childRuns.begin(), childRuns.end(),
                         [&job, &cues, first] (const Run* child)
                         {
                             return child->cue == cues[first] && ! job.hasClaimed (child->id);
                         });

        if (unclaimed)
        {
            job.nextMember = first + 1;
            return true;
        }

        engine.submit (origin::engine, "run.spawn",
                       { osc::Value::string (job.run), osc::Value::string (cues[first]) });
        job.nextMember = first + 1;
        return true;
    }

    void Runner::endOfRound (Engine& engine, GroupJob& job, const juce::ValueTree& group)
    {
        auto* run = runs.find (job.run);

        /*  A ROUND ENDING IS NOT THE GROUP ENDING, which is the whole of what
            `loops` buys. Another round begins unless one of three things says
            otherwise: this was the last one, somebody asked the group to stop
            at a boundary, or there is nothing left to play.

            THE COUNT IS OF ROUNDS AND NOT OF PLAYBACKS (§3.6). With `play` set,
            a round is a subset of the members, so three loops of two-of-five is
            six cues rather than three - which is what a designer asking for
            "two of these, three times" means.

            Only the MEMBERS loop. A header and a footer are the group's own
            preparation and release; running them twice would release something
            twice and prepare something that was already prepared. */
        const auto again = job.phase == groupPhase::members
                             && run != nullptr
                             && run->stopAfter.empty()
                             && (run->iterations == 0 || run->iteration < run->iterations);

        if (again && beginPhase (engine, job, group, groupPhase::members))
            return;

        finishPhase (engine, job, group);
    }

    void Runner::finishPhase (Engine& engine, GroupJob& job, const juce::ValueTree& group)
    {
        /*  Header, then members, then footer, and the group is done when the
            last of them is. The footer BLOCKS (§3.6), which is not a special
            case here: it is a phase like the other two, and the group's
            `run.ended` comes after it because it comes after all of them. */
        if (job.phase == groupPhase::header)
        {
            if (beginPhase (engine, job, group, groupPhase::members))
                return;
        }

        if (job.phase != groupPhase::footer)
            if (beginPhase (engine, job, group, groupPhase::footer))
                return;

        engine.submit (origin::engine, "run.ended", one (job.run));
        job.retired = true;
    }

    void Runner::armStandby (Engine& engine)
    {
        /*  THE STANDBY ARMS WHAT IT LANDS ON, and until now nothing did.

            §11.4 of the namespace draft said "standby arms implicitly" and no
            code ever did it: `audio.arm` has been a command with no submitter
            anywhere in the engine since PR 2.3, so a GO on a cue nobody had
            armed by hand did the arming AND the launching in one, and paid the
            disk while the operator's hand was already down. §11.8 measured that
            disk at about 0.4 s for a local file.

            Which is the whole point of arming ahead: the work happens while the
            operator reads the next line, not after they press GO.

            IT IS A COMMAND AND NOT SOMETHING THIS HOOK DOES, for the reason
            every decision here is: a hook does not run during a replay, and a
            run created outside the log is a run the replay would not have. So
            the hook notices and submits, and `audio.arm` carries the run
            identifier it drew - which is what a replay re-supplies.

            ASKED ONCE PER CUE. `armedStandby` is what stops this being a
            submission every tick for as long as the pointer sits there; a cue
            that failed to arm is not retried, because a voice that was busy a
            tick ago is busy now and fifty rejections a second is not a report,
            it is a fault of its own. */
        if (audio == nullptr)
            return;

        const auto list = focus.list (document);
        const auto standby = list.isValid()
                               ? list[juce::Identifier ("standby")].toString().toStdString()
                               : std::string {};

        if (standby == armedStandby)
            return;

        armedStandby = standby;

        if (standby.empty())
            return;

        const auto cue = document.findById (standby);

        /*  ONLY A MEDIA CUE HAS ANYTHING TO MAKE READY. Asking to arm a memo
            would be a rejection every time the pointer passed over one, which
            would fill the log with a refusal about something nobody did wrong.
            (A group at standby arms what it would launch first - PR 3.4, with
            the cursor that knows how to look inside one.) */
        if (! cue.isValid() || cue.getType().toString() != "Media")
            return;

        /*  Already running or already armed: nothing to do. `audio.arm` would
            answer with the live run and change nothing, but not asking is
            cheaper and keeps the log about things that happened. */
        if (runs.liveRunOf (standby) != nullptr)
            return;

        engine.submit (origin::engine, "audio.arm", one (standby));
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
        advanceGroups (engine);
        armStandby (engine);
        advanceFades (engine, tick);
        applyLevels();
        advanceSends (engine);

        if (audio == nullptr)
            return;

        launchIfDue (engine, tick);
        advanceRanges (engine);
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

            /*  SLOT NOUGHT UNTIL PR 3.9 KNOWS BETTER. A cue with no ranges
                plays out of the first slot, which is what Phase 2 did without
                having to say so; a ranged cue enters its first range, which is
                the same slot. Which slot a LATER range sounds out of is the
                range job's to say, and it does not exist yet. */
            if (audio->launchAtSample (run->track, 0, target))
            {
                run->launchRequested = false;
                run->launchedAtSample = target;

                engine.submit (origin::engine, "run.started", one (run->id));

                /*  AND WHICH RANGE IT IS IN, when it has any. A cue with no
                    ranges plays its file out of slot nought and never enters
                    one, which is what `range = -1` says and is every cue Phase
                    2 knew about.

                    The bookkeeping is set here and the published value by the
                    record, which is the same split `run.started` uses: a replay
                    is told which range and cannot be told which SAMPLE, having
                    no counter to have counted it. */
                if (const auto ranges = rangesOf (document.findById (run->cue));
                    ! ranges.empty())
                {
                    run->rangeStartedAtSample = target;
                    run->passesWanted = ranges.front().loops;
                    run->passSamples = samplesForRange (ranges.front(),
                                                        audio->sampleRate());
                    run->boundaryPlacedAt = -1;
                    run->rangesFinished = false;

                    engine.submit (origin::engine, "run.range",
                                   { osc::Value::string (run->id), osc::Value::int32 (0) });
                }

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

    void Runner::advanceRanges (Engine& engine)
    {
        /*  WHAT A LOOPING RANGE COSTS PER PASS: nothing. M12 measured three
            ways of carrying a loop from one pass to the next and the clip's own
            wrap won every one of ten configurations, by between five and
            twenty-three thousand times in damage energy - so Go.dot places
            nothing INSIDE a range and only at the boundary OUT of it.

            Which makes this loop's job small: count the pass for the strip to
            read, and place one stop-and-play pair when the range is ending. */
        const auto rate = static_cast<std::int64_t> (audio->sampleRate());
        const auto ticksAhead = latencyTicks();

        if (rate <= 0 || ticksAhead <= 0 || samplesPerTick <= 0)
            return;

        const auto now = audio->samplesElapsed();
        const auto lead = static_cast<std::int64_t> (ticksAhead) * samplesPerTick;

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->range < 0 || run->track < 0 || run->isFinished())
                continue;

            /*  Nothing to count until the launch has been placed and the first
                range has actually been entered. */
            if (run->rangeStartedAtSample <= 0 || run->passSamples <= 0)
                continue;

            /*  THE PASS, from the sample counter. Not a counter the scheduler
                increments, because a counter would be right during a show and
                nought through every replay - and because at 50 Hz a pass
                shorter than 20 ms would be missed entirely by anything that
                counted edges. */
            const auto elapsed = std::max<std::int64_t> (0, now - run->rangeStartedAtSample);
            run->rangeIteration = static_cast<int> (elapsed / run->passSamples) + 1;

            if (run->rangesFinished)
                continue;

            /*  RE-READ AT EVERY BOUNDARY, which is decision L: a `loops` an
                operator changed while the range played is honoured from here,
                and a range deleted while it played is not entered again. What
                is NOT re-read is the pass length of the range playing now - it
                is what the clip was armed with, and changing it would need the
                message thread to re-arm the slot, which is PR 3.10's. */
            const auto cue = document.findById (run->cue);
            const auto ranges = rangesOf (cue);
            const auto count = static_cast<int> (ranges.size());

            /*  The cue lost every range while it played. There is nothing left
                to advance into, so the range playing now is the last one. */
            const auto stillThere = run->range < count;

            const auto wanted = stillThere
                                  ? ranges[static_cast<std::size_t> (run->range)].loops
                                  : run->passesWanted;

            /*  WHERE THIS RANGE ENDS.

                An advance ends it at the next pass boundary that is still far
                enough ahead to be placed; a loop count ends it after that many
                passes; and nought passes with no advance never ends at all,
                which is what an ambience bed is. */
            std::int64_t endsAt = 0;

            if (run->advanceRequested)
            {
                /*  THE END OF THE PASS IT IS ON, measured from NOW and not from
                    the placement horizon. Adding the horizon first is the shape
                    this had when it was written and it never fired: the answer
                    was always more than a horizon away by construction, so the
                    check below deferred it for ever and an advance did nothing
                    at all.

                    The horizon belongs to the PLACEMENT and not to the choice
                    of instant. If the pass ends too soon to place cleanly, that
                    is what `run.late` is for. */
                const auto passesGone = (now - run->rangeStartedAtSample) / run->passSamples;

                endsAt = run->rangeStartedAtSample + (passesGone + 1) * run->passSamples;
            }
            else if (wanted > 0)
            {
                endsAt = run->rangeStartedAtSample
                           + static_cast<std::int64_t> (wanted) * run->passSamples;
            }
            else
            {
                continue;                       // loops for ever, and nobody has asked it not to
            }

            /*  NOT YET. The boundary is placed when it comes inside the
                placement horizon and not before, so that an edit made while the
                range plays is still read in time to change it. */
            if (endsAt - now > lead)
                continue;

            /*  ALREADY PLACED. LaunchHandle keeps ONE queued state, so a second
                stop queued at the same instant would replace the play that was
                queued with the first - the outgoing range would end and the
                incoming one would never start. */
            if (run->boundaryPlacedAt == endsAt)
                continue;

            /*  TOO LATE TO PLACE CLEANLY, which is what `run.late` is for. The
                tick thread overslept, or an edit moved the boundary closer than
                the graph can honour; the transition still happens, at the first
                instant that can be honoured, and the log says by how much. */
            const auto blockSize = static_cast<std::int64_t> (audio->blockSize());
            auto placeAt = endsAt;

            if (placeAt - now < 2 * blockSize)
            {
                placeAt = now + 2 * blockSize;

                if (blockSize > 0)
                {
                    const auto blocks = (placeAt - endsAt) / blockSize;

                    if (blocks > 0)
                        engine.submit (origin::engine, "run.late",
                                       { osc::Value::string (run->id),
                                         osc::Value::int32 (static_cast<std::int32_t> (blocks)) });
                }
            }

            const auto next = run->range + 1;
            const auto hasNext = next < count;

            /*  THE PAIR, both at the same instant. M12 priced it: the outgoing
                range is taken down with SlotControlNode's own 40-sample decay,
                which is 25 to 33 samples of damage and does not grow with the
                block size, and the incoming range starts on exactly its sample. */
            audio->stopAtSample (run->track, run->range, placeAt);

            if (hasNext)
                audio->launchAtSample (run->track, next, placeAt);

            run->boundaryPlacedAt = placeAt;
            run->advanceRequested = false;

            if (! hasNext)
            {
                /*  THE PLAYLIST IS OVER, and saying so here is what stops
                    `observeEdges` ending the run at every boundary before this
                    one - see `rangesFinished`. */
                run->rangesFinished = true;
                continue;
            }

            /*  The next range's clock starts at the boundary, so its first pass
                is measured from where it will actually begin rather than from
                the tick that decided it. */
            run->rangeStartedAtSample = placeAt;
            run->passesWanted = ranges[static_cast<std::size_t> (next)].loops;
            run->passSamples = samplesForRange (ranges[static_cast<std::size_t> (next)], rate);

            engine.submit (origin::engine, "run.range",
                           { osc::Value::string (run->id),
                             osc::Value::int32 (static_cast<std::int32_t> (next)) });
        }
    }

    void Runner::applyLevels()
    {
        /*  EFFECTIVE = OWN + EVERY ANCESTOR'S OWN, walked rather than cached.

            The chain is at most as deep as the show's nesting and a show is a
            handful of levels, so the walk is cheaper than any bookkeeping that
            would have to be invalidated - and bookkeeping is where a trim gets
            left behind after the group that owned it has gone. */
        const auto effectiveOf = [this] (const Run& run)
        {
            auto total = run.ownLevel;
            auto parent = run.parent;

            /*  BOUNDED BY THE TABLE, not by the tree, because a `parent` that
                pointed at itself would otherwise be a show that hangs on its
                first tick. The table cannot be longer than it is. */
            for (std::size_t guard = 0; guard <= runs.all().size() && ! parent.empty(); ++guard)
            {
                const auto* above = runs.find (parent);

                if (above == nullptr)
                    break;

                total += above->ownLevel;
                parent = above->parent;
            }

            return total;
        };

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->isFinished())
                continue;

            const auto effective = effectiveOf (*run);

            if (juce::approximatelyEqual (effective, run->level))
                continue;

            run->level = effective;

            /*  A GROUP RUN HAS NO VOICE, which is what makes its level a trim
                rather than a level: the number reaches the outputs through its
                members, each of which has just had it added to its own. */
            if (audio != nullptr && run->track >= 0)
                audio->setLevelDb (run->track, effective);
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
                /*  A BOUNDARY IS NOT AN ENDING, and without this every ranged
                    cue would end at its first one.

                    At a boundary the outgoing slot stops in the same block the
                    incoming one starts - but this poll is 20 ms wide and a
                    block is a fraction of that, so a poll can fall between them
                    and see neither playing. The run would report itself done
                    with two ranges still to play, and the sound would go on
                    without it.

                    `rangesFinished` is set when the LAST range's end has been
                    placed, so the silence after that one is the cue finishing. */
                if (run->range >= 0 && ! run->rangesFinished)
                    continue;

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
                        /*  AS MANY IDENTIFIERS AS THE PRESS CREATED. A member
                            three manual groups deep needs each of those groups
                            live before it can be their child, so one GO makes
                            four runs - and the record carries all of them, in
                            the order they were made, because a replay never
                            draws one of its own. */
                        { { "run", 's', true, true } },
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
                            /*  The run table, so that a manual group with
                                rounds left keeps the pointer instead of letting
                                it out on the last member of round one. */
                            const auto next = nextStandby (list, standby, &runner.runTable());
                            document.setAttribute (standbyAddressOf (listId), next);

                            /*  EVERY IDENTIFIER THIS GO CREATED, not just one.

                                A member three levels inside manual groups needs
                                each of those groups live before it can be their
                                child, so one GO can create four runs. The record
                                carries all of them, in the order they were made,
                                and a replay hands them back in that order - which
                                is the same guarantee the single identifier gave,
                                widened to a number that depends on where the
                                pointer was. */
                            std::vector<std::string> supplied;

                            for (const auto& value : args)
                                supplied.push_back (value.getString());

                            const auto made = runner.fireStandby (engine, context.tick,
                                                                  list, standby, supplied);

                            std::vector<osc::Value> applied;

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
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
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            /*  A MANUAL SEQUENCE GROUP HAS NOBODY TO BE ITS
                                PARENT when it is fired by name. §3.6 makes the
                                operator the parent: its members start on GO, one
                                press at a time, and the pointer is what says
                                which. Fired from a surface it would run its
                                header, start its first member and then wait for
                                a GO that is never coming - a scene stuck halfway
                                with its voices held.

                                Refused rather than quietly run as an automatic
                                one, because "run this group without me" is a
                                reasonable thing to want and is a different group
                                from the one somebody wrote. */
                            if (cue.getType().toString() == "Group"
                                  && runner.isManualGroup (cue))
                                return Outcome::rejected (reason::needsGo);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.fire (engine, context.tick,
                                                                      cueId, id)));
                        } });

        //----------------------------------------------------------------------
        /*  WHAT FIRED IS AN ARGUMENT, NOT AN ORIGIN.

            §4.11 wants every gesture-reachable action to exist as a named
            command, and a trigger firing is a gesture somebody made months ago
            in a document. The origin says where the message came from -
            `udp:10.0.0.5:9000`, `midi:BCF2000`, `clock` - and nothing anywhere
            enforces an origin, by design (RunCommands.h): anyone may send an
            engine-origin command, because one only the inside of the process
            could send would be one a replay could not send. So the trigger's
            identity travels as the argument, where it can be checked.

            IT FIRES AND MOVES NOTHING. §3.5 and §3.7 are both explicit: only GO
            moves the standby. That is the whole reason a background list can be
            driven by something other than a person without the person losing
            their place, and it is why this is not `go` with a different name. */
        registry.add ({ "trigger.fire",
                        "A trigger fired its cue. The standby does not move, whatever it was.",
                        { { "trigger", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto trigger = document.findById (args[0].getString());

                            if (! trigger.isValid()
                                  || trigger.getType().toString() != "Trigger")
                                return Outcome::rejected (reason::unknownId);

                            const auto cue = trigger.getParent();

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto cueId = cue[idProperty].toString().toStdString();

                            /*  A MANUAL SEQUENCE GROUP HAS NOBODY TO BE ITS
                                PARENT, the same refusal `cue.fire` gives and for
                                the same reason: its members start on GO, one
                                press at a time, and fired from here it would run
                                its header, start its first member and wait for a
                                press that is never coming. */
                            if (cue.getType().toString() == "Group"
                                  && runner.isManualGroup (cue))
                                return Outcome::rejected (reason::needsGo);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.fire (engine, context.tick,
                                                                      cueId, id)));
                        } });

        //----------------------------------------------------------------------
        /*  THE TWO A GROUP SENDS ITSELF, and they are two rather than one
            because spawning and launching are separate moments. An auto
            sequence spawns the next member while the current one is still
            playing - so the disk is paid for before the chain reaches it - and
            launches it when the current one reports done. A timeline group does
            both at entry for every member at once.

            Like every engine-origin command they may be sent by anyone: one
            only the inside of the process could send would be one a replay
            could not send. */
        registry.add ({ "run.spawn",
                        "A group created one of its members' runs: reserved, made ready, and not"
                        " yet started.",
                        { { "parent", 's', false }, { "cue", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto parentRun = args[0].getString();
                            const auto cueId = args[1].getString();

                            if (! runner.knowsRun (parentRun))
                                return Outcome::rejected (reason::unknownId);

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto id = args.size() > 2 ? args[2].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 2,
                                                         runner.spawnChild (engine, parentRun,
                                                                            cueId, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.launch",
                        "A spawned run begins: its pre-wait starts, or it fires at once when it"
                        " has none.",
                        { { "run", 's', false } },
                        true,
                        [&engine, &runner] (CommandContext& context,
                                            const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            runner.launchRun (engine, context.tick, runId);
                            return Outcome::ok (args);
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
