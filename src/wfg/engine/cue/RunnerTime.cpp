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

/*
    A TIME GROUP FIRING ON ITS CODE (namespace draft §60, TG.7; the Runner's,
    in a file of its own beside Runner.cpp, as RunnerSeat.cpp is). TG.2 armed
    a time group and fired nothing; TG.5 and TG.6 gave the Runner a map of
    each incoming code against Go.dot's own clock - which frame fell on which
    sample, at what speed. This is where a member is started from that map.

    THE RULES, AS BUILT. Every tick of the group's members phase:

      - Its source's map is read (`ltc:<sync/ltcIn>`, `mtc:<sync/mtcIn>`,
        `clock`). While the code RUNS FORWARDS, each member whose time falls
        after the last frame the group crossed and no later than where the code
        will be in two seconds (AIQ) is LOADED: one run of it, spawned under
        the group's run and named from it (RD.2), so a backup applying the same
        maps loads it under the same name - a sound's voice and file armed, a
        picture read ahead, a scene entered so its own members load.

      - A loaded member is ASKED FOR a launch horizon and two ticks before the
        sample the code reaches its time on: fired, with its launch placed on
        exactly that sample (`Run::launchNotBefore`, RD.8's field, which
        `launchIfDue` places on). Two ticks and not one, because the ask is a
        record applied in this tick's drain and the launch is placed by the
        next tick's hook, which must still find the sample at least a horizon
        away. A picture's appearance is placed there the same way. Its pre-wait
        counts from its time, in samples, never on the tick counter.

      - The crossed frame moves on to where the code is, while it runs
        forwards; a `jumped` turn puts it where the jump landed (AIH), so a jump
        past a member never starts it, and running into the time again after a
        jump back starts it a second time (AIR) - the run still playing from the
        first crossing stopped as the new one starts: a restart, not a second
        copy.

      - Backwards or stopped, nothing new is loaded or asked for: what was asked
        for keeps its sample, and what was loaded keeps its arm (TG.8 decides
        what a locate does to a chaser). A member whose time was crossed before
        it could be asked - loaded in the very tick, its pre-wait still to run -
        is still asked for, on the sample it was given while the code ran.

      - A member loaded too close to its time - the group armed, or the code
        jumped, just before it - starts as soon as it can, from its top, and says
        by how much (`run.late`, the words a cold GO's launch uses).

      - Armed while the code is already past a member's time, the group does
        not start it: a free runner starts only when the code runs into its
        time. A chaser that should be playing by then is TG.8's to seat
        (`seatChasersAt`), called here and left empty.

    A SCENE AS A MEMBER (AIV) - a timeline or an automatic sequence - whose
    every member's start the walk knows (`ShowWalk.h`: one round, every member,
    in order, no header, and an automatic sequence's lengths known) is PLACED:
    entered as soon as it is loaded, two seconds ahead, so that its members can
    be loaded two seconds before their own starts; and COMMITTED once its own
    start comes within a launch horizon while its code still wants it - from
    then on each member is asked for on its own sample, its offset from the
    scene's start in samples of Go.dot's clock. This is `placedTick`, in place
    of the timeline's spawn-everything and the sequence's one-after-another
    (which waits for a member to END before it even loads the next): a
    descendant is never launched by a counter and never twice, since the
    placed job alone launches it, once. A scene the walk cannot place - a
    header, a loop, a shuffle, a length unknown - is started as written when
    its time comes within a launch horizon, and its members follow their own
    counters from there, late by its entry: said here, and by the walk's own
    rules, not guessed.

    EVERYTHING IS A RECORD (`run.spawn`, `run.fire`, `run.kill`), decided here
    and applied by the handlers, so `wfg replay` - which runs no hooks and so
    reads no map here - reaches the same runs from the log, and a backup's
    hooks, applying the main's `code.heard` mapped onto its own clock, decide
    the same things on the same ticks under the same names.
*/

#include <wfg/engine/cue/Runner.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/cue/Override.h>
#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/cue/TriggerIndex.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        /*  HOW FAR AHEAD A MEMBER IS LOADED (AIQ): two seconds, far past what a
            sound's file takes to answer from a disk (§11.8 measured some 0.4 s)
            and short enough that a show of a few hundred timed cues holds a few
            voices at a time, not all of them. */
        constexpr double preloadSeconds = 2.0;

        /*  HOW MANY TICKS BEYOND ITS LAUNCH HORIZON A RUN IS ASKED FOR: one for
            the drain the ask is applied in, one for the hook that places it on
            the next tick to still find its sample a whole horizon away. */
        constexpr int askBeyondHorizon = 2;

        std::vector<osc::Value> one (const std::string& text)
        {
            return { osc::Value::string (text) };
        }

        /*  TWO RATES THAT NUMBER THEIR LABELS ALIKE: the same frames a second
            and the same frames dropped. 23.976 counts as 24 does and 29.97 as
            30, so their labels name the same frame numbers whatever the speed
            the frames come at - the map's rate may be told from the show's by
            its period alone, and a label read at either is the same frame. */
        bool countsAlike (timecode::Rate a, timecode::Rate b) noexcept
        {
            return timecode::nominalOf (a) == timecode::nominalOf (b) && timecode::dropsFrames (a) == timecode::dropsFrames (b);
        }

        /*  THE FRAME OF THE CODE A MEMBER'S `at` NAMES, in the map's own unit.

            The wall clock's map counts milliseconds of the day, and a clock
            member's time is `HH:MM:SS` as a clock trigger's is (AIL, read by the
            same `secondOfDayFor`). A timecode's map counts frames at the rate
            the code's labels count by, and a member's label is read at the
            show's rate (AIL): the same frame when the two count alike, which is
            every show whose code agrees with it; otherwise - a warned show, the
            code at 30 and the show at 25 - the label's moment at the show's
            rate, and the first frame of the code at or after it, so the member
            starts on a frame the code has, never before its moment. Nothing for
            a time that does not read under the map's unit: a member written for
            another source, which TG.2 warns of, never fires. */
        std::optional<std::int64_t> frameOnCode (const timecode::CodeMap& map, const std::string& at,
                                                 timecode::Rate showRate)
        {
            if (at.empty())
                return std::nullopt;

            if (! map.unit.rate.has_value())
            {
                const auto second = secondOfDayFor (at);

                if (second < 0)
                    return std::nullopt;

                return static_cast<std::int64_t> (second) * 1000;
            }

            const auto label = timecode::parseLabel (at);

            if (! label.has_value())
                return std::nullopt;

            const auto codeRate = *map.unit.rate;

            if (countsAlike (showRate, codeRate))
                return timecode::frameOf (*label, codeRate);

            /*  frame * den/num seconds at the show's rate, in the code's frames,
                rounded up: whole numbers throughout, a day of frames times a
                thousand and one times thirty thousand well inside 64 bits. */
            const auto shown = timecode::frameOf (*label, showRate);
            const auto numerator = shown * timecode::denOf (showRate) * timecode::numOf (codeRate);
            const auto denominator = timecode::numOf (showRate) * timecode::denOf (codeRate);

            if (denominator <= 0)
                return std::nullopt;

            const auto frames = (numerator + denominator - 1) / denominator;
            return timecode::codemap::wrapped (frames, map.unit.perDay);
        }

        /*  WHETHER A FRAME IS STILL AHEAD OF ONE CROSSED, the near way round the
            day as CodeMap reads a frame: a code running across midnight has its
            members just after midnight ahead, not twenty-three hours behind. */
        bool ahead (std::int64_t frame, std::int64_t of, std::int64_t perDay) noexcept
        {
            return timecode::codemap::nearestOffset (frame - of, perDay) > 0;
        }

        /*  WHETHER A MEMBER'S TIME IS IN THE LOADING WINDOW: after the frame the
            group last crossed, and no further than where the code will be two
            seconds from now. */
        bool inWindow (std::int64_t frame, std::int64_t crossed, std::int64_t upTo, std::int64_t perDay) noexcept
        {
            const auto into = timecode::codemap::nearestOffset (frame - crossed, perDay);
            const auto reach = timecode::codemap::nearestOffset (upTo - crossed, perDay);
            return into > 0 && into <= reach;
        }

        //  Seconds on Go.dot's clock in its samples.
        std::int64_t samplesOf (double seconds, int sampleRate) noexcept
        {
            return std::llround (seconds * static_cast<double> (sampleRate));
        }

        /*  The kinds whose start lands on a sample: a sound, a live input, a
            code (`launchIfDue`, `launchTimecode`) and a picture (`advanceVideo`).
            Every other kind does what it does on the tick it is fired on. */
        bool startsOnASample (const std::string& kind) noexcept
        {
            return kind == "media" || kind == "mic" || kind == "timecode" || kind == "video";
        }
    }

    //==========================================================================
    juce::ValueTree Runner::timeGroupAbove (const juce::ValueTree& cue) const
    {
        for (auto node = cue.getParent(); node.isValid(); node = node.getParent())
        {
            if (! node.hasType ("Group"))
            {
                //  A header or a footer is a section of its group, not a stop.
                if (node.hasType ("Header") || node.hasType ("Footer"))
                    continue;

                break;
            }

            if (isTimeGroup (groupMode (node)))
                return node;
        }

        return {};
    }

    const Runner::TimePlan& Runner::timePlanOf (const juce::ValueTree& timeGroup)
    {
        const auto id = timeGroup[idProperty].toString().toStdString();
        auto& plan = timePlans[id];

        /*  ONCE A REVISION OF THE SHOW, and again when the lengths the walk
            reads are swapped for newer ones: the walk is a pass over the group,
            and the hook asks every tick. `revision` is never nought, so a plan
            never built never matches. */
        if (plan.revision == document.revision() && plan.durations == durations)
            return plan;

        plan = TimePlan {};
        plan.revision = document.revision();
        plan.durations = durations;

        //  THE SOURCE, as serve names the watchers' (Console.cpp): the source word and the input or port.
        const auto source = textOf (timeGroup, "timeSource");

        if (source == "clock")
        {
            plan.source = "clock";
        }
        else if (source == "mtc")
        {
            if (const auto port = document.getAttribute ("/godot/sync/mtcIn").value_or (std::string {}); ! port.empty())
                plan.source = "mtc:" + port;
        }
        else if (const auto input = document.getAttribute ("/godot/sync/ltcIn").value_or (std::string {}); ! input.empty())
        {
            plan.source = "ltc:" + input;
        }

        plan.showRate = timecode::rateFromWord (document.getAttribute ("/godot/sync/frameRate").value_or (std::string {}))
                          .value_or (timecode::Rate::fps25);

        for (const auto& memberId : membersOf (timeGroup))
        {
            const auto member = document.findById (memberId);

            if (! member.isValid())
                continue;

            TimeMember entry;
            entry.cue = memberId;
            entry.at = textOf (member, "at");
            entry.preWait = std::max (0.0, numberOf (member, "preWait"));
            entry.kind = member.hasType ("Group") ? "group" : member.hasType ("Video") ? "video" : "media";
            plan.members.push_back (std::move (entry));
        }

        /*  EACH CUE'S START UNDER ITS MEMBER, by the walk the solver and the
            slot analysis read (ShowWalk.h): the group's members are walked as a
            list's top rows, so each member is the origin of its own chain, and
            a cue inside it is placed at the second it starts, counted from the
            moment its chain began - the member's time, before the member's own
            pre-wait (the walk counts a group's members after its pre-wait, as
            the Runner runs them). A scene inside that the walk could not time
            whole - a length unknown - is the origin of a chain of its own, its
            members counted from its own beginning, which is its start less its
            pre-wait; a cue in no chain at all has no start, and the scene
            holding it is not placed. */
        static const Reader schema;
        Walk walk (schema, durations);
        walk.visitList (timeGroup);

        std::map<std::string, double> preWaits;

        for (const auto& entry : walk.placed)
        {
            preWaits[entry.id] = std::max (0.0, schema.number (entry.node, "cue", "preWait"));

            if (entry.ancestors.empty())
            {
                plan.starts[entry.id] = preWaits[entry.id];
                plan.topOf[entry.id] = entry.id;
                continue;
            }

            plan.topOf[entry.id] = entry.ancestors.front();

            if (entry.startChain.empty())
                continue;

            auto began = 0.0;

            if (entry.startChain != entry.ancestors.front())
            {
                const auto chain = plan.starts.find (entry.startChain);

                if (chain == plan.starts.end())
                    continue;

                began = chain->second - preWaits[entry.startChain];
            }

            plan.starts[entry.id] = began + entry.from;
        }

        /*  A SCENE IS PLACED when it is a timeline or an automatic sequence and
            every member it would run has a start - which the walk gives only
            when nothing of its pace is left to the night: no header, one round,
            all of them, in order. Its own start is known too, or it would not
            be reached here at all. */
        for (const auto& entry : walk.placed)
        {
            if (entry.element != "Group" || plan.starts.count (entry.id) == 0
                  || ! pacedByMachine (groupMode (entry.node)))
                continue;

            auto all = true;

            for (const auto& memberId : membersOf (entry.node))
                if (plan.starts.count (memberId) == 0)
                    all = false;

            if (all)
                plan.placeable.insert (entry.id);
        }

        return plan;
    }

    //==========================================================================
    GroupJob* Runner::jobOf (const std::string& runId)
    {
        for (auto& job : scheduled)
            if (! job.retired && job.run == runId)
                return &job;

        return nullptr;
    }

    bool Runner::awaitsTheCode (const Run& run) const
    {
        if (run.isFinished())
            return false;

        //  Loaded and never fired: `fireKind` stamps every kind's ask.
        if (run.state == runState::armed && run.launchRequestedAtTick == 0)
            return true;

        if (! run.isGroup())
            return false;

        for (const auto& job : scheduled)
            if (! job.retired && job.run == run.id)
                return job.codeSample > 0 && ! job.codeCommitted;

        return false;
    }

    bool Runner::pictureLoadedForTheCode (const Run& run)
    {
        if (run.kind != "video" || run.sampler || run.isFinished() || run.state != runState::armed
              || run.launchRequestedAtTick != 0 || run.parent.empty())
            return false;

        const auto* parent = runs.find (run.parent);

        if (parent == nullptr || parent->isFinished())
            return false;

        if (const auto group = document.findById (parent->cue); group.isValid() && isTimeGroup (groupMode (group)))
            return true;

        const auto* placed = jobOf (parent->id);
        return placed != nullptr && placed->codeSample > 0;
    }

    void Runner::seatChasersAt (GroupJob& job, std::int64_t frame)
    {
        /*  TG.8 SEATS HERE a member that chases and whose time the code has
            already passed - started at its offset, as load to time seats a
            scene (`seatPlan`), when it should be playing at `frame` - on the
            group armed while its code runs, and on every jump. Free runners are
            never seated (AIH): they start only when the code runs into their
            time. Nothing yet: TG.7 fires chasers and free runners alike. */
        juce::ignoreUnused (job, frame);
    }

    //==========================================================================
    bool Runner::askOnCode (Engine& engine, Run& pending, std::int64_t start, std::int64_t now, bool mayCommit,
                            bool placeable)
    {
        const auto horizon = [this] (int ticks)
        {
            return static_cast<std::int64_t> (ticks + askBeyondHorizon) * samplesPerTick;
        };

        if (pending.isGroup())
        {
            if (pending.state == runState::armed && pending.launchRequestedAtTick == 0)
            {
                /*  A SCENE THE CODE PLACES IS ENTERED AS SOON AS IT IS LOADED,
                    uncommitted: its members load two seconds before their own
                    starts, under it, so a member starting with the scene is
                    never cold. Its start goes to its job through the run, read
                    by the handler that fires it. */
                if (placeable)
                {
                    pending.codeSample = start;
                    engine.submit (origin::engine, "run.fire", one (pending.id));
                    return false;
                }

                /*  ONE IT CANNOT PLACE is started as written when its start is
                    within a launch horizon - its header, its rounds, its
                    members on their own counters from there - and so goes with
                    no start of the code's on it, or its job would read it as
                    placed. */
                if (! mayCommit || start - now > horizon (latencyTicks()))
                    return false;

                pending.codeSample = 0;
                engine.submit (origin::engine, "run.fire", one (pending.id));
                return true;
            }

            auto* placed = jobOf (pending.id);

            if (placed == nullptr || placed->codeSample <= 0 || placed->codeCommitted)
                return false;

            /*  UNTIL IT IS COMMITTED its start is its parent's to move: a code
                re-anchored, a parent scene's own start moved. */
            placed->codeSample = start;
            pending.codeSample = start;

            if (! mayCommit || start - now > horizon (latencyTicks()))
                return false;

            placed->codeCommitted = true;
            return true;
        }

        if (pending.state != runState::armed || pending.launchRequestedAtTick != 0 || ! mayCommit)
            return false;

        /*  A SOUND, A LIVE INPUT, A CODE OR A PICTURE: asked for far enough
            ahead to be placed on its sample - a picture by the video's own lead,
            which is two ticks with no player. Anything else fires on the tick its
            moment falls in, which is when what it does leaves. */
        if (startsOnASample (pending.kind))
        {
            const auto lead = pending.kind == "video" ? videoLeadTicks() : latencyTicks();

            if (start - now > horizon (lead))
                return false;

            pending.launchNotBefore = start;
        }
        else if (start - now >= static_cast<std::int64_t> (samplesPerTick))
        {
            return false;
        }

        pending.codeSample = start;
        engine.submit (origin::engine, "run.fire", one (pending.id));
        return true;
    }

    //==========================================================================
    void Runner::timeTick (Engine& engine, GroupJob& job, const juce::ValueTree& group, const Run& groupRun)
    {
        const auto& plan = timePlanOf (group);
        const auto* map = plan.source.empty() ? nullptr : codeMapOf (plan.source);
        const auto now = videoClockNow();
        const auto rate = videoSampleRate();

        if (map == nullptr || map->state == timecode::CodeState::absent || now < 0 || rate <= 0
              || samplesPerTick <= 0 || map->unit.perDay <= 0)
            return;

        const auto perDay = map->unit.perDay;
        const auto moving = map->state == timecode::CodeState::running;
        const auto forwards = moving && std::isfinite (map->speed) && map->speed > 0.0;
        const auto turns = codeJumpsOf (plan.source);
        const auto sameUnit = job.codeUnitNum == map->unit.num && job.codeUnitDen == map->unit.den;

        //  The frames each member's time names, on this map.
        std::map<std::string, std::int64_t> frames;

        for (const auto& member : plan.members)
            if (const auto frame = frameOnCode (*map, member.at, plan.showRate))
                frames[member.cue] = *frame;

        const auto memberNamed = [&plan] (const std::string& cueId) -> const TimeMember*
        {
            for (const auto& member : plan.members)
                if (member.cue == cueId)
                    return &member;

            return nullptr;
        };

        /*  EACH MEMBER'S RUN STILL WAITING ON THE CODE, the newest, read once a
            tick rather than once a question: what this hook submits is applied
            after it, so the table does not move while it decides. */
        std::map<std::string, Run*> waitingRuns;

        for (const auto* child : runs.childrenOf (groupRun.id))
            if (awaitsTheCode (*child))
                waitingRuns[child->cue] = runs.find (child->id);

        const auto awaiting = [&waitingRuns] (const std::string& cueId) -> Run*
        {
            const auto found = waitingRuns.find (cueId);
            return found != waitingRuns.end() ? found->second : nullptr;
        };

        const auto upTo = forwards ? map->positionAt (now + samplesOf (preloadSeconds, rate)).frame : 0;

        //----------------------------------------------------------------------
        /*  THE FIRST RUNNING CODE THE GROUP SEES: what it crossed is where the
            code is, so a member whose time has passed is not started (AIH) -
            the group armed while the code was already past it. */
        if (job.codeCrossed < 0)
        {
            if (! moving)
                return;

            job.codeCrossed = map->positionAt (now).frame;
            job.codeTurns = turns;
            job.codeUnitNum = map->unit.num;
            job.codeUnitDen = map->unit.den;
            seatChasersAt (job, job.codeCrossed);
        }

        /*  A JOB A SEAT HANDED OVER (RD.8) holds no count of this machine's:
            it takes the count as it stands, with no jump read into it. */
        else if (job.codeTurns < 0)
        {
            job.codeTurns = turns;
            job.codeUnitNum = map->unit.num;
            job.codeUnitDen = map->unit.den;
        }

        /*  A JUMP (AIH): the code appeared again, came back or located. What the
            group crossed is where it landed, so a member it jumped past is never
            started; what was loaded for a time still in the window is kept, and
            what was loaded for a time the code no longer comes to is let go,
            idle, its voice with it. A free runner already playing plays on. */
        else if (turns != job.codeTurns)
        {
            job.codeTurns = turns;
            job.codeUnitNum = map->unit.num;
            job.codeUnitDen = map->unit.den;
            job.codeCrossed = moving ? map->positionAt (now).frame : timecode::codemap::wrapped (map->frame, perDay);
            job.codeAhead.clear();

            for (const auto& member : plan.members)
            {
                const auto* waiting = awaiting (member.cue);

                if (waiting == nullptr)
                    continue;

                const auto frame = frames.find (member.cue);

                if (forwards && frame != frames.end() && inWindow (frame->second, job.codeCrossed, upTo, perDay))
                {
                    job.codeAhead.push_back (member.cue);
                    continue;
                }

                for (const auto* child : runs.childrenOf (groupRun.id))
                    if (child->cue == member.cue && awaitsTheCode (*child))
                        engine.submit (origin::engine, "run.kill", one (child->id));
            }

            seatChasersAt (job, job.codeCrossed);
        }

        /*  A CODE WHOSE RATE BECAME CLEARER (TG.5's `running` with another
            unit) numbers its frames again: what was crossed is counted again
            from where the code is, and nothing loaded is let go. */
        else if (! sameUnit)
        {
            job.codeUnitNum = map->unit.num;
            job.codeUnitDen = map->unit.den;

            if (moving)
                job.codeCrossed = map->positionAt (now).frame;
        }

        //----------------------------------------------------------------------
        /*  LOADED: each member whose time is in the next two seconds, once for
            this pass of the code. A run already loaded and waiting - a seat's,
            or one kept through a jump - is taken as this pass's. */
        std::set<std::string> loadedNow;

        if (forwards)
            for (const auto& member : plan.members)
            {
                const auto frame = frames.find (member.cue);

                if (frame == frames.end() || ! inWindow (frame->second, job.codeCrossed, upTo, perDay)
                      || std::find (job.codeAhead.begin(), job.codeAhead.end(), member.cue) != job.codeAhead.end())
                    continue;

                job.codeAhead.push_back (member.cue);

                if (awaiting (member.cue) != nullptr)
                    continue;

                engine.submit (origin::engine, "run.spawn",
                               { osc::Value::string (groupRun.id), osc::Value::string (member.cue) });
                loadedNow.insert (member.cue);
            }

        //----------------------------------------------------------------------
        /*  ASKED FOR: each loaded member, on the sample the code reaches its
            time on, its pre-wait after it - worked out again every tick while
            the code runs forwards, so a re-anchored code moves it; kept as it
            was once the code stopped or turned after crossing its time. */
        for (const auto& cueId : job.codeAhead)
        {
            auto* waiting = awaiting (cueId);
            const auto* member = memberNamed (cueId);
            const auto frame = frames.find (cueId);

            if (waiting == nullptr || member == nullptr || frame == frames.end())
                continue;

            const auto crossedIt = ! ahead (frame->second, job.codeCrossed, perDay);

            if (! forwards && ! crossedIt)
                continue;

            /*  Kept on the run while it waits - on a scene's job once it is
                entered - so a code that stops after crossing the time, the
                member's pre-wait still to run, still has it to ask on. */
            auto start = waiting->codeSample;
            const auto* placed = waiting->isGroup() ? jobOf (waiting->id) : nullptr;

            if (placed != nullptr)
                start = placed->codeSample;

            if (forwards)
                if (const auto at = map->sampleOf (frame->second))
                {
                    start = *at + samplesOf (member->preWait, rate);

                    if (placed == nullptr)
                        waiting->codeSample = start;
                }

            if (start <= 0)
                continue;

            const auto placeable = plan.placeable.count (cueId) > 0;
            const auto waitingId = waiting->id;

            if (! askOnCode (engine, *waiting, start, now, true, placeable))
                continue;

            /*  A RESTART (AIR): the run of this member still playing from an
                earlier crossing stops as this one starts - one member, heard
                once - by the stop a graceful end uses. */
            for (const auto* child : runs.childrenOf (groupRun.id))
                if (child->cue == cueId && child->id != waitingId && ! child->isFinished()
                      && child->state != runState::stopping && ! awaitsTheCode (*child))
                    engine.submit (origin::engine, "run.stop", one (child->id));
        }

        //----------------------------------------------------------------------
        //  CROSSED, while the code runs forwards: never back.
        if (forwards)
            if (const auto at = map->positionAt (now).frame; ahead (at, job.codeCrossed, perDay))
                job.codeCrossed = at;

        /*  And a member crossed and asked for leaves this pass - unless its run
            is still waiting (its pre-wait to run, a scene not yet committed) or
            was loaded only in this tick, its run not made yet. */
        std::erase_if (job.codeAhead, [&] (const std::string& cueId)
                       {
                           const auto frame = frames.find (cueId);

                           if (frame == frames.end() || memberNamed (cueId) == nullptr)
                               return true;

                           return ! ahead (frame->second, job.codeCrossed, perDay) && loadedNow.count (cueId) == 0
                                    && awaiting (cueId) == nullptr;
                       });
    }

    //==========================================================================
    void Runner::placedTick (Engine& engine, GroupJob& job, const juce::ValueTree& group, const Run& groupRun)
    {
        const auto timeGroup = timeGroupAbove (group);
        const TimePlan* plan = timeGroup.isValid() ? &timePlanOf (timeGroup) : nullptr;
        const auto now = videoClockNow();
        const auto rate = job.codeRate > 0 ? job.codeRate : videoSampleRate();

        const auto startOf = [plan] (const std::string& cueId) -> std::optional<double>
        {
            if (plan == nullptr)
                return std::nullopt;

            const auto found = plan->starts.find (cueId);
            return found != plan->starts.end() ? std::optional<double> (found->second) : std::nullopt;
        };

        const auto self = startOf (groupRun.cue);

        //  Taken on sight, as a timeline takes what it spawned.
        for (const auto* child : runs.childrenOf (job.run))
            if (std::find (job.phaseCues.begin(), job.phaseCues.end(), child->cue) != job.phaseCues.end()
                  && ! job.hasTaken (child->id) && child->prepare.empty())
            {
                job.taken.push_back (child->id);
                job.phaseRuns.push_back (child->id);
            }

        const auto runOf = [this, &job] (const std::string& cueId) -> Run*
        {
            Run* found = nullptr;

            for (const auto& id : job.phaseRuns)
                if (auto* child = runs.find (id); child != nullptr && child->cue == cueId)
                    found = child;

            return found;
        };

        /*  A MEMBER WHOSE START IS NO LONGER KNOWN - the show edited under the
            scene, the scene moved out from under its code - and not yet loaded
            leaves the phase: nothing could place it, and a phase waiting on it
            would never end. What was started finishes. */
        std::erase_if (job.phaseCues, [&] (const std::string& cueId)
                       {
                           return (! self.has_value() || ! startOf (cueId).has_value() || now < 0 || rate <= 0)
                                    && runOf (cueId) == nullptr;
                       });

        auto allOver = true;

        for (const auto& cueId : job.phaseCues)
        {
            auto* child = runOf (cueId);
            const auto start = startOf (cueId);

            if (child == nullptr)
            {
                allOver = false;

                //  Loaded two seconds before its own start, under the scene.
                if (start.has_value() && self.has_value()
                      && job.codeSample + samplesOf (*start - *self, rate) - samplesOf (preloadSeconds, rate) <= now)
                    engine.submit (origin::engine, "run.spawn",
                                   { osc::Value::string (job.run), osc::Value::string (cueId) });

                continue;
            }

            if (! child->isFinished())
                allOver = false;

            if (! awaitsTheCode (*child) || ! start.has_value() || ! self.has_value())
                continue;

            const auto placeable = plan != nullptr && plan->placeable.count (cueId) > 0;
            askOnCode (engine, *child, job.codeSample + samplesOf (*start - *self, rate), now, job.codeCommitted, placeable);
        }

        /*  OVER WHEN EVERY MEMBER HAS PLAYED - no round to draw again (a placed
            scene plays once, or the walk would not have placed it) - and on to
            its footer. */
        if (allOver)
            finishPhase (engine, job, group);
    }

    //==========================================================================
    std::string Runner::fireTimeMember (Engine& engine, std::int64_t tick, const std::string& cueId,
                                        const std::string& runId)
    {
        const auto cue = document.findById (cueId);
        const auto group = cue.isValid() ? cue.getParent() : juce::ValueTree {};

        if (! group.isValid() || ! group.hasType ("Group") || ! isTimeGroup (groupMode (group)))
            return {};

        /*  ARMED: the group's live run playing - a run state, which a handler
            may read and a replay holds, never the job's phase, which the hook
            moves and a replay never does. Not one on its way out, killed or
            taken back. */
        const auto* armed = runs.liveRunOf (group[idProperty].toString().toStdString());

        if (armed == nullptr || armed->state != runState::playing || armed->takenBack || beingKilled (*armed))
            return {};

        const auto groupRun = armed->id;

        /*  NAMED FROM THE GROUP'S RUN, as the code's are (RD.2) - or the
            record's name, a replay's or a follower's. Made, never adopted: a
            member the code loaded for its time stays the code's. */
        const auto id = runId.empty() ? namer.next (RunNames::causeOf (groupRun, cueId), cueId) : runId;

        if (id.empty())
            return {};

        const auto made = spawnChild (engine, groupRun, cueId, id, tick);

        if (made.empty())
            return {};

        launchRun (engine, tick, made);
        return made;
    }
}
