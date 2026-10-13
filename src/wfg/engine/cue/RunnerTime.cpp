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
        time. A chaser that should be playing by then is seated (TG.8,
        `seatChasersAt`, below).

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

    CHASING (TG.8 and TG.9; AIF, AIG, AIH, AIN, AIS, AIT, AIU, AIV). A member
    whose `chase` is not `free` is held to its code once it plays: it should be
    as far into its material as the code is past its time
    (`chase::chasePlaceOf`). It is brought back by a jump, re-seated at a
    locate, or caught up gradually:

      - THE JUMP. Every tick after the speeds are placed (`chaseTick`), a
        chaser by `jump` is measured a launch horizon ahead - where its file
        is by its own clock against where the code says - and, more than
        `sync/chaseTolerance` out while the code runs forwards, jumps there
        over `sync/chaseCrossfade`: inside the slice it plays, or in a cue with
        no slices, a move on its slot's reader with a crossfade, the one a
        slice moved under itself makes (§33, CD), and the run's clock told
        where the file is from there. Across slices, a seek, its gap said
        (`sought`). A movie cuts, a step on its time ring, and its locked
        sounds jump with it. One move at a time on a voice: two segments only
        live in one, so a jump waits until the last move is reached.

      - THE LOCATE. A `jumped` turn - the code appeared, came back after a
        stop, or located - re-seats the chasers inside its own `code.heard`
        record, which hands back the runs it drew (`seatChasersOnCode`): one
        that should be playing at the new place and is, is brought into step
        where it is; one that should and is not is seated at its offset, as
        load to time seats a cue; one that should not and is ends, as a jump
        ends what it abandons. Free runners playing are left alone (AIH); a
        member of any kind asked for its sample on the old line and not yet
        playing is cut. A code that stops leaves every chaser playing (AIG),
        and its return is a locate.

      - A GROUP ARMED WHILE ITS CODE RUNS seats its chasers already past their
        time by `code.seat`, which the hook submits with the sample it read
        the code at - the one place a hook asks for a seat.

      - THE CATCH-UP (TG.9). A chaser by `varispeed` or `timestretch` is
        never moved while it is near: measured as a jump chaser is, it is run
        at the code's speed times a correction - timecode/Chase.h's law, at
        most `sync/chaseRate`, begun past `sync/chaseTolerance` and ended back
        within a millisecond (a stretched voice's chunk, AJC) - and jumps only
        past `sync/chaseJump`. The factor is the run's (`Run::chaseFactor`),
        multiplied into its own speed where `applyRates` and the launch place
        a speed on the voice, and into a movie's rate where `advanceMovie`
        places its time ring: a speed decided this tick is placed a launch
        horizon ahead, so the error the hook measures there already holds
        every speed placed before it. A movie's locked sounds run at the
        movie's factor; a scene member's sounding media each at its own, by
        the member's method (AIV). The method also chose the voice's mode at
        its arm (`chaseMethodOf`, AIU). The code stopped or turned back, every
        factor is one: a chaser plays on at its own speed (AIG), and the
        locate that brings the code back jumps it, or steers it from there
        when it is less than `sync/chaseJump` out. The factor is worked out
        from the code's logged line and the run's own clock, never logged, so
        a backup applying the main's lines steers its chasers alike.
*/

#include <wfg/engine/cue/Runner.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/cue/Override.h>
#include <wfg/engine/cue/PlayedMedia.h>
#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/cue/Solver.h>
#include <wfg/engine/cue/TriggerIndex.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/Chase.h>
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/Timecode.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

        /*  WHETHER A MEMBER'S `chase` HOLDS IT TO ITS CODE (AIF): every word
            but `free`, and an empty one is the row's default, `free`. */
        bool chasesTheCode (const std::string& chase) noexcept
        {
            return ! chase.empty() && chase != "free";
        }

        //  THE WORDS `run/chaseState` TAKES (TG.2's row).
        namespace chaseWords
        {
            inline constexpr const char* freeRunner = "free";
            inline constexpr const char* inStep = "inStep";
            inline constexpr const char* catching = "catching";
            inline constexpr const char* jumped = "jumped";
        }

        /*  HOW FAR APART A SOUND AND ITS CODE MAY BE IN TWO DIFFERENT SLICES
            before it is sought rather than left to cross into the code's: a
            second, far past a chase's drift at a boundary - a member a few
            milliseconds from the code crosses into its slice within those
            milliseconds - and well short of a locate's distance. */
        constexpr double acrossSlicesSeconds = 1.0;
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
            entry.chase = textOf (member, "chase");
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

    void Runner::seatChasersAt (Engine& engine, const Run& groupRun, std::int64_t now)
    {
        /*  NOT TWICE: a `jumped` turn applied since the group was armed has
            seated its chasers already, in its own record, and the code has run
            on from there - the members it reaches from here are the hook's. */
        if (seatedOnCode (groupRun.id))
            return;

        engine.submit (origin::engine, "code.seat", { osc::Value::string (groupRun.id), osc::Value::int64 (now) });
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

            /*  AND A CHASER ALREADY PAST ITS TIME is seated where the code is
                (TG.8), by a record a replay applies. */
            if (forwards)
                for (const auto& member : plan.members)
                    if (const auto frame = frames.find (member.cue);
                        frame != frames.end() && chasesTheCode (member.chase)
                          && ! ahead (frame->second, job.codeCrossed, perDay))
                    {
                        seatChasersAt (engine, groupRun, now);
                        break;
                    }
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
            idle, its voice with it. A free runner already playing plays on. The
            chasers were re-seated by the `code.heard` record that said the jump
            (TG.8, `seatChasersOnCode`), in the drain before this tick, and what
            it seated is asked for already, not waiting on the code. */
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

    //==========================================================================
    /*  WHERE A CHASER SHOULD BE (TG.8), pure: see Runner.h. */
    namespace chase
    {
        double codeSecondsSince (const timecode::CodeMap& map, std::int64_t frame, std::int64_t sample) noexcept
        {
            if (map.unit.num <= 0 || map.unit.den <= 0)
                return 0.0;

            const auto at = map.positionAt (sample);
            const auto frames = static_cast<double> (timecode::codemap::nearestOffset (at.frame - frame, map.unit.perDay))
                                  + at.fraction;

            return frames * static_cast<double> (map.unit.den) / static_cast<double> (map.unit.num);
        }

        Place chasePlaceOf (const std::vector<RangeSpec>& ranges, double startOffset,
                            std::optional<double> fileLength, double rate, double seconds)
        {
            Place out;

            /*  BEFORE ITS TIME: where it would begin - its first slice's in
                point, or its start offset. */
            if (seconds < 0.0)
            {
                out.before = true;
                out.file = startOffset;

                for (std::size_t index = 0; index < ranges.size(); ++index)
                    if (ranges[index].out > ranges[index].in)
                    {
                        out.range = static_cast<int> (index);
                        out.file = ranges[index].in;
                        break;
                    }

                return out;
            }

            const auto progress = seconds * std::max (0.0, rate);
            out.progress = progress;

            //  NO SLICES: the file from its start offset, over at its end when the show knows it.
            if (ranges.empty())
            {
                out.file = startOffset + progress;
                out.over = fileLength.has_value() && out.file >= *fileLength;
                return out;
            }

            /*  A PLAYLIST OVER ONE FILE (§3.24), pass by pass in order. A slice
                that loops for ever is followed pass by pass too: the code says
                how long it has played, where a load to time has nobody to ask
                and lands on its first pass. */
            auto remaining = progress;
            auto last = -1;

            for (std::size_t index = 0; index < ranges.size(); ++index)
            {
                const auto& slice = ranges[index];
                const auto span = slice.out - slice.in;

                if (! (span > 0.0))
                    continue;

                last = static_cast<int> (index);
                const auto whole = span * static_cast<double> (slice.loops);

                if (slice.loops <= 0 || remaining < whole)
                {
                    const auto passes = std::floor (remaining / span);

                    out.range = static_cast<int> (index);
                    out.pass = static_cast<int> (passes) + 1;
                    out.file = slice.in + (remaining - passes * span);
                    return out;
                }

                remaining -= whole;
            }

            out.over = true;
            out.range = last;
            out.file = last >= 0 ? ranges[static_cast<std::size_t> (last)].out : startOffset;
            return out;
        }
    }

    //==========================================================================
    Runner::ChaseSettings Runner::chaseSettings() const
    {
        const auto read = [this] (const char* name, double fallback)
        {
            const auto value = osc::parseDouble (document.getAttribute (std::string ("/godot/sync/") + name)
                                                   .value_or (std::string {}));

            return value.has_value() && std::isfinite (*value) ? std::max (0.0, *value) : fallback;
        };

        /*  A TOLERANCE OF NOUGHT is as tight as a jump can be held, not a jump
            every tick: what is left after a jump is a sample's rounding, and a
            tenth of a millisecond is twice that at 48 kHz. */
        ChaseSettings out;
        out.tolerance = std::max (0.0001, read ("chaseTolerance", 10.0) / 1000.0);
        out.catchUp = std::max (out.tolerance, read ("chaseJump", 0.25));
        out.crossfade = read ("chaseCrossfade", 10.0) / 1000.0;

        /*  THE CATCH-UP'S CLAMP (TG.9): the row is a percentage, its range the
            schema's ten; held there, so a value the door let through by another
            road never asks a voice for more. And where a stretched voice's
            catch-up ends: one of the stretcher's chunks at the interface's rate
            (AJC, timecode/Chase.h). */
        out.rate = std::clamp (read ("chaseRate", 1.0) / 100.0, 0.0, 0.10);
        out.stretchSettle = timecode::chaseSettleFor (true, audio != nullptr ? static_cast<double> (audio->sampleRate()) : 0.0);
        return out;
    }

    std::string Runner::chaseMethodOf (const Run& run) const
    {
        /*  UP THE RUNS, not the document: a locked sound may sit anywhere in the
            show and is a child of its movie's run, and a seated sound of the
            group's run it was seated under. The first run whose parent is a
            time group's is the member; its cue's word is the chase. Bounded, as
            a parent chain a bad table made circular must not hang the arm. */
        const Run* at = &run;

        for (int depth = 0; depth < 64 && at != nullptr && ! at->parent.empty(); ++depth)
        {
            const auto* up = runs.find (at->parent);

            if (up == nullptr)
                break;

            if (up->isGroup())
                if (const auto node = document.findById (up->cue);
                    node.isValid() && node.hasType ("Group") && isTimeGroup (groupMode (node)))
                {
                    const auto member = document.findById (at->cue);
                    return member.isValid() ? textOf (member, "chase") : std::string {};
                }

            at = up;
        }

        return {};
    }

    std::optional<chase::Place> Runner::chaseTargetOf (const juce::ValueTree& cue, const TimeMember& member,
                                                       const TimePlan& plan, const timecode::CodeMap& map,
                                                       std::int64_t sample, double start,
                                                       const std::map<std::string, double>* lengths) const
    {
        if (! cue.isValid())
            return std::nullopt;

        const auto frame = frameOnCode (map, member.at, plan.showRate);
        const auto rate = numberOf (cue, "rate");

        /*  BACKWARDS OR STANDING, nothing to hold it to: a cue played the other
            way runs from its end (§41), and one at nought never moves. */
        if (! frame.has_value() || ! std::isfinite (rate) || ! (rate > 0.0))
            return std::nullopt;

        return chase::chasePlaceOf (rangesOf (cue), std::max (0.0, numberOf (cue, "startOffset")),
                                    playedLengthOf (cue, lengths), rate,
                                    chase::codeSecondsSince (map, *frame, sample) - start);
    }

    std::optional<double> Runner::chaseErrorOf (const Run& run, std::int64_t sample, const chase::Place& target,
                                                double rate, const std::vector<RangeSpec>& ranges) const
    {
        if (audio == nullptr || run.launchedAtSample <= 0 || sample < run.launchedAtSample || ! (rate > 0.0))
            return std::nullopt;

        //  Where its own clock has its file: what `run/position` reads.
        const auto actual = fileSecondAt (run, static_cast<double> (sample));

        //  NO SLICES, as the code has none either: one file against the other.
        if (run.range < 0 || target.range < 0)
        {
            if ((run.range < 0) != (target.range < 0))
                return std::nullopt;

            return (actual - target.file) / rate;
        }

        const auto slot = static_cast<std::size_t> (run.range);

        if (slot >= run.playingSlices.size())
            return std::nullopt;

        const auto& playing = run.playingSlices[slot];
        const auto span = playing.out - playing.in;

        /*  IN THE SLICE THE CODE IS IN: the near way round its loop, since the
            same second comes round at every pass - and a member a whole pass
            out reads in step, its count of passes its own. */
        if (run.range == target.range)
        {
            auto difference = actual - target.file;
            const auto loops = slot < ranges.size() ? ranges[slot].loops : 1;

            if (span > 0.0 && loops != 1)
            {
                difference = std::fmod (difference, span);

                if (difference >= span / 2.0)
                    difference -= span;
                else if (difference < -span / 2.0)
                    difference += span;
            }

            return difference / rate;
        }

        /*  IN ANOTHER: what each has played since the cue began - the slices
            ahead of the run's whole, then its own passes as its clock last
            counted them. */
        auto before = 0.0;

        for (std::size_t index = 0; index < slot && index < ranges.size(); ++index)
            if (const auto length = ranges[index].out - ranges[index].in; length > 0.0)
                before += length * static_cast<double> (std::max (1, ranges[index].loops));

        const auto played = before + static_cast<double> (std::max (0, run.rangeIteration - 1)) * std::max (0.0, span)
                              + (actual - playing.in);

        return (played - target.progress) / rate;
    }

    //==========================================================================
    bool Runner::chaseJump (Run& run, std::int64_t sample, const chase::Place& target, double crossfade)
    {
        if (audio == nullptr || run.kind != "media" || run.track < 0 || run.launchedAtSample <= 0
              || sample <= run.launchedAtSample || run.stopIssued || run.rangesFinished || run.direction < 0)
            return false;

        const auto now = audio->samplesElapsed();
        const auto rate = static_cast<double> (audio->sampleRate());

        /*  ONE MOVE AT A TIME ON A VOICE: it keeps the latest two segments, the
            one a jump fades out of and the jump, so a move is placed only once
            the last one - a jump's, or a slice moved under itself (§33) - has
            been reached. And never closer than two blocks, as a launch. */
        if (! (rate > 0.0) || now < run.loopMovePlacedAt
              || sample - now < 2 * static_cast<std::int64_t> (audio->blockSize()))
            return false;

        //----------------------------------------------------------------------
        /*  A CUE WITH NO SLICES: the move on slot nought, from where its reader
            will be, and the run's file read from there as a turn's is (§41) -
            bent, its file from `turnFile` at `turnSource`. When the show knows
            the file's end, Go.dot places that end itself, as it does for a cue
            that has played backwards: the clip's own end is counted in reads,
            and a jump moved the file under them. One whose end is not known
            keeps the clip's, out by what its jumps added up to. */
        if (run.range < 0)
        {
            if (target.range >= 0)
                return false;

            const auto readerAt = readerSecondAt (run, static_cast<double> (sample));
            const auto move = audio->placeLoop (run.track, 0, { readerAt, target.file, 0.0, 0.0, crossfade, 1, false });

            if (move == 0)
                return false;

            run.turnSource = run.rateClock.sourceAt (static_cast<double> (sample));
            run.turnFile = target.file;
            run.bent = true;

            if (run.pieceEnd > run.pieceStart)
            {
                run.turnEndPlaced = 0;

                if (! run.turned)
                {
                    run.turned = true;
                    audio->endsOutside (run.track, true);
                }
            }

            run.loopMove = move;
            run.loopMoveFrom = readerAt;
            run.loopMovePlacedAt = sample;
            return true;
        }

        //----------------------------------------------------------------------
        /*  INSIDE THE SLICE IT PLAYS: the slice moved under itself to where the
            code is, its points unchanged - `moveSoundingSlice`'s clock, from the
            sample the move lands on - and the passes the code says it has
            played already counted, so the slice ends where the code's would.
            Not a slice on its way in (a boundary placed, a move not reached),
            nor one that bounces or plays backwards. */
        const auto slot = static_cast<std::size_t> (run.range);

        if (target.range != run.range || slot >= run.playingSlices.size() || run.bent || run.passSamples <= 0
              || run.rangeStartedAtSample <= 0 || now < run.rangeStartedAtSample)
            return false;

        const auto playing = run.playingSlices[slot];

        if (playing.pingPong || ! (target.file >= playing.in && target.file < playing.out))
            return false;

        const auto atSpeed = ! run.rateClock.isIdentityFrom (static_cast<double> (run.launchedAtSample));
        const auto played = atSpeed ? run.rateClock.sourceAt (static_cast<double> (sample)) - run.rangeSource
                                    : static_cast<double> (sample - run.rangeStartedAtSample);
        const auto readerAt = run.readerAtSliceStart + played / rate;

        const auto move = audio->placeLoop (run.track, run.range,
                                            { readerAt, target.file, playing.in, playing.out, crossfade, 1, false });

        if (move == 0)
            return false;

        //  The outgoing clock, kept for the lane until the move is reached (§20.4).
        run.laneOutgoingOrigin = run.positionOrigin;
        run.laneOutgoingAt = run.rangeStartedAtSample;
        run.laneOutgoingPass = run.passSamples;
        run.laneOutgoingSource = run.rangeSource;
        run.laneOutgoingFirst = run.firstPassSamples;
        run.laneOutgoingFirstFrom = run.firstPassFrom;

        run.rangeStartedAtSample = sample;
        run.rangeSource = run.rateClock.sourceAt (static_cast<double> (sample));
        run.positionOrigin = playing.in;
        run.firstPassFrom = target.file;
        run.firstPassSamples = std::max<std::int64_t> (1, std::llround ((playing.out - target.file) * rate));
        run.passesBefore = std::max (0, target.pass - 1);
        run.readerAtSliceStart = readerAt;
        run.boundaryPlacedAt = -1;

        run.loopMove = move;
        run.loopMoveFrom = readerAt;
        run.loopMovePlacedAt = sample;
        return true;
    }

    void Runner::cutMovie (VideoJob& job, std::int64_t sample, const chase::Place& target,
                           const std::vector<RangeSpec>& ranges)
    {
        /*  A STEP on the playhead's ring, as a seek places one: where it is
            there, then where it goes - a picture cuts rather than crossfades
            (AIT). Its range and pass follow the code's. */
        placeVideoPoint (job, video::Property::time, { sample, videoValueOf (job, video::Property::time, sample) });
        placeVideoPoint (job, video::Property::time, { sample, target.file });

        job.moviePosition = target.file;
        job.movieAt = sample;
        job.rangeId = target.range >= 0 && static_cast<std::size_t> (target.range) < ranges.size()
                        ? ranges[static_cast<std::size_t> (target.range)].id : std::string {};
        job.pass = std::max (0, target.pass - 1);
        job.bounce = 1;
    }

    //==========================================================================
    bool Runner::steerChaser (Engine& engine, Run& run, const juce::ValueTree& targetCue, const TimeMember& member,
                              const TimePlan& plan, const timecode::CodeMap& map, double start,
                              const ChaseSettings& settings, bool byJump, bool inHandler, const Run* leader)
    {
        if (run.isFinished() || run.state == runState::stopping || run.takenBack || ! targetCue.isValid())
            return false;

        const auto rate = numberOf (targetCue, "rate");
        const auto* lengths = handlerDurations();

        /*  THE CODE'S SPEED, code seconds per Go.dot second: what a catch-up
            chaser in step runs at (AIS). Only read while the code runs forwards
            - the hook does not steer otherwise - so anything else reads as one. */
        const auto codeSpeed = std::isfinite (map.speed) && map.speed > 0.0 ? map.speed : 1.0;

        /*  A JUMP CHASER plays at its own speed between its jumps, as TG.8 has
            it: its factor is one, whatever a chase it had before an edit left. */
        if (byJump)
        {
            run.chaseFactor = 1.0;
            run.chaseCorrecting = false;
        }

        /*  A CATCH-UP CHASER NOT MEASURED THIS TICK - waiting for its launch or
            its time, a movie waiting for its sounds after a seek - or just
            jumped, which lands it in step: at the code's speed, nothing being
            corrected, so it starts and carries on with the code rather than at
            one. A locked sound at its movie's, which the movie's run carries. */
        const auto holdCodeSpeed = [&run, byJump, leader, codeSpeed]
        {
            if (byJump)
                return;

            run.chaseFactor = leader != nullptr ? leader->chaseFactor : codeSpeed;
            run.chaseCorrecting = false;
        };

        /*  ONE NOT HELD TO ITS CODE AT ALL - played backwards, bouncing between
            a slice's points, a movie bouncing or standing, a time that does not
            read - plays at its own speed, as a free runner does (TG.8 holds
            none of them): a locked sound at its movie's. */
        const auto notHeld = [&run, byJump, leader]
        {
            if (byJump)
                return;

            run.chaseFactor = leader != nullptr ? leader->chaseFactor : 1.0;
            run.chaseCorrecting = false;
        };

        /*  THE LAW'S NUMBERS (timecode/Chase.h): the show's rows, and where the
            catch-up ends - a millisecond, or a stretched voice's chunk (AJC). */
        timecode::ChaseLaw law;
        law.start = settings.tolerance;
        law.jump = settings.catchUp;
        law.most = settings.rate;
        law.settle = run.stretch ? settings.stretchSettle : timecode::chaseSettleSeconds;

        /*  THE ERROR SAID, and what the method does with it. A jump chaser
            acts past the tolerance. A catch-up chaser is steered below
            `sync/chaseJump` - its factor the law's, or a locked sound's its
            movie's, so the two keep together however each was measured - and
            acts only past it. True when it is to be brought back by a jump or
            a cut. */
        const auto outOfStep = [&] (double error)
        {
            run.chaseError = error * 1000.0;

            if (byJump)
            {
                if (std::abs (error) <= settings.tolerance)
                {
                    run.chaseState = chaseWords::inStep;
                    return false;
                }

                return true;
            }

            const auto step = timecode::chaseStep (law, error, codeSpeed, run.chaseCorrecting);

            if (step.move == timecode::ChaseMove::jump)
                return true;

            run.chaseFactor = leader != nullptr ? leader->chaseFactor : step.factor;
            run.chaseCorrecting = leader != nullptr ? leader->chaseCorrecting : step.correcting;
            run.chaseState = run.chaseCorrecting ? chaseWords::catching : chaseWords::inStep;
            return false;
        };

        const auto jumpedHere = [&run, &holdCodeSpeed]
        {
            run.chaseState = chaseWords::jumped;
            holdCodeSpeed();
        };

        //----------------------------------------------------------------------
        /*  A MOVIE, measured where its next point is placed: its playhead there
            against the code's second, at its rate times its factor, as
            `advanceMovie` moves it. Not while it waits for its sounds after a
            seek, nor bouncing, nor backwards. */
        if (run.kind == "video")
        {
            VideoJob* job = nullptr;

            for (auto& candidate : showing)
                if (candidate.self == run.id && candidate.movie)
                    job = &candidate;

            const auto at = videoSampleAhead();
            const auto sampleRate = videoSampleRate();

            if (job == nullptr || ! job->placed || job->removed || job->movieEnded || ! job->soundsSought.empty()
                  || at < 0 || at < job->movieAt || sampleRate <= 0)
            {
                holdCodeSpeed();
                return false;
            }

            if (job->bounce != 1 || ! (job->rate > 0.0) || ! (rate > 0.0))
            {
                notHeld();
                return false;
            }

            const auto target = chaseTargetOf (targetCue, member, plan, map, at, start, lengths);

            if (! target.has_value())
            {
                notHeld();
                return false;
            }

            if (! target->sounding())
            {
                holdCodeSpeed();
                return false;
            }

            const auto ranges = rangesOf (targetCue);
            const auto actual = job->moviePosition
                                  + job->rate * run.chaseFactor * static_cast<double> (at - job->movieAt)
                                      / static_cast<double> (sampleRate);
            auto error = actual - target->file;

            //  The near way round a loop, in the slice the code is in.
            if (target->range >= 0 && static_cast<std::size_t> (target->range) < ranges.size())
            {
                const auto& slice = ranges[static_cast<std::size_t> (target->range)];
                const auto span = slice.out - slice.in;

                if (slice.id == job->rangeId && slice.loops != 1 && span > 0.0)
                {
                    error = std::fmod (error, span);

                    if (error >= span / 2.0)
                        error -= span;
                    else if (error < -span / 2.0)
                        error += span;
                }
            }

            if (! outOfStep (error / rate))
                return false;

            cutMovie (*job, at, *target, ranges);
            jumpedHere();
            return true;
        }

        //----------------------------------------------------------------------
        /*  A SOUND, measured a launch horizon ahead, where a jump would land -
            and where a speed decided now is first placed. Not one played the
            other way or bouncing between a slice's points: its file runs against
            the clock, and the code's line names none. */
        if (run.kind != "media" || audio == nullptr || samplesPerTick <= 0)
        {
            holdCodeSpeed();
            return false;
        }

        if (run.direction < 0 || (run.range >= 0 && run.bent))
        {
            notHeld();
            return false;
        }

        const auto now = audio->samplesElapsed();
        const auto at = now + static_cast<std::int64_t> (latencyTicks()) * samplesPerTick;

        if (run.launchedAtSample <= 0 || at <= run.launchedAtSample)
        {
            holdCodeSpeed();
            return false;
        }

        /*  A JUMP PLACED BEYOND WHERE IT IS MEASURED, and not reached there yet -
            a stretched voice's, below - is left to land, as just jumped: from the
            sample it lands on the run's clock reads the file where the jump puts
            it, and before it reads it standing there, which measured against the
            code would be a jump's worth of error that is not there. */
        if (at < run.loopMovePlacedAt)
        {
            holdCodeSpeed();
            return false;
        }

        const auto target = chaseTargetOf (targetCue, member, plan, map, at, start, lengths);

        if (! target.has_value())
        {
            notHeld();
            return false;
        }

        if (! target->sounding())
        {
            holdCodeSpeed();
            return false;
        }

        const auto ranges = rangesOf (targetCue);
        const auto error = chaseErrorOf (run, at, *target, rate, ranges);

        if (! error.has_value())
        {
            holdCodeSpeed();
            return false;
        }

        if (! outOfStep (*error))
            return false;

        //  IN THE SLICE IT PLAYS, or a cue with none: the crossfaded jump.
        if (target->range == run.range)
        {
            /*  A STRETCHED VOICE'S JUMP LANDS BEYOND ITS STRETCHER. Its reader
                runs ahead of what is heard by the stretcher's latency and a chunk
                - some 155 ms at 48 kHz, four launch horizons - so a jump placed a
                horizon ahead was met only after the reader had read past it,
                applied from where the reader was, and heard that much behind the
                code: 106 ms in blackbox.timegroup (TG.11), then seven seconds
                catching up. So it is placed where the reader has not been yet, its
                lead beyond the horizon, at the code's second there; the clock is
                not measured again until it lands (above). A resampled voice reads
                a frame or two ahead and jumps a horizon ahead, as before. */
            auto land = at;
            auto landing = *target;

            if (run.stretch)
            {
                land = at + static_cast<std::int64_t> (std::ceil (audio->stretchLeadSamples (run.ratePlaced)));
                const auto there = chaseTargetOf (targetCue, member, plan, map, land, start, lengths);

                if (! there.has_value() || ! there->sounding() || there->range != run.range)
                    return false;

                landing = *there;
            }

            if (! chaseJump (run, land, landing, settings.crossfade))
                return false;

            jumpedHere();
            return true;
        }

        /*  IN ANOTHER SLICE. A second or less apart, it is one about to cross
            its boundary into the code's, or the code about to cross into its
            own, and it is left to: in the same slice, a jump. Further, it is
            sought there - stopped and started again where the code will be when
            it can launch, the seat's lead on, and said: a seek has a gap. A
            handler submits nothing; a locate sought it already if its slice
            had moved. */
        if (inHandler || std::abs (*error) < acrossSlicesSeconds)
            return false;

        const auto launchAt = now + static_cast<std::int64_t> (latencyTicks() + seatLeadTicks) * samplesPerTick;
        const auto there = chaseTargetOf (targetCue, member, plan, map, launchAt, start, lengths);

        if (! there.has_value() || ! there->sounding())
            return false;

        run.launchNotBefore = launchAt;
        run.warning = runWarning::sought;
        jumpedHere();
        engine.submit (origin::engine, "run.seek", { osc::Value::string (run.id), osc::Value::float64 (there->file) });
        return true;
    }

    //==========================================================================
    void Runner::chaseTick (Engine& engine)
    {
        if (samplesPerTick <= 0)
            return;

        std::optional<ChaseSettings> settings;

        /*  BY INDEX: nothing here makes a job, but the jobs are the handlers'
            and a reference held across a submit is a habit not to start. */
        for (std::size_t index = 0; index < scheduled.size(); ++index)
        {
            if (scheduled[index].retired || scheduled[index].phase != groupPhase::members)
                continue;

            const auto groupId = scheduled[index].run;
            const auto* groupRun = runs.find (groupId);

            if (groupRun == nullptr || groupRun->isFinished())
                continue;

            const auto group = document.findById (groupRun->cue);

            if (! group.isValid() || ! group.hasType ("Group") || ! isTimeGroup (groupMode (group)))
                continue;

            const auto& plan = timePlanOf (group);
            const auto* map = plan.source.empty() ? nullptr : codeMapOf (plan.source);

            /*  THE CODE STOPPED (AIG), or running backwards: every chaser plays
                on at its own speed - its factor one again, so a catch-up chaser
                does not carry the code's last speed or a correction into the
                silence - and catches up when it comes back, which is a locate,
                its record's to seat, and then steering from there. */
            const auto forwards = map != nullptr && map->state == timecode::CodeState::running
                                    && std::isfinite (map->speed) && map->speed > 0.0;

            if (! settings.has_value())
                settings = chaseSettings();

            //  The group's live runs by their cue, read once a tick rather than once a member.
            std::map<std::string, std::vector<std::string>> byCue;

            for (const auto* child : runs.childrenOf (groupId))
                if (! child->isFinished())
                    byCue[child->cue].push_back (child->id);

            for (const auto& member : plan.members)
            {
                const auto mine = byCue.find (member.cue);

                if (mine == byCue.end())
                    continue;

                const auto chases = chasesTheCode (member.chase);
                const auto byJump = member.chase == "jump";

                for (const auto& id : mine->second)
                {
                    auto* run = runs.find (id);

                    if (run == nullptr)
                        continue;

                    /*  AT ITS OWN SPEED, it and everything under it: a factor of
                        one and no correction under way (TG.9). */
                    const auto ownSpeed = [this, &id, run]
                    {
                        run->chaseFactor = 1.0;
                        run->chaseCorrecting = false;

                        for (const auto* below : runs.descendantsOf (id))
                            if (auto* under = runs.find (below->id); under != nullptr && ! under->isFinished())
                            {
                                under->chaseFactor = 1.0;
                                under->chaseCorrecting = false;
                            }
                    };

                    /*  A FREE RUNNER says so, and everything under it: nothing
                        holds it - nor any chase an edit of its `chase` took away. */
                    if (! chases)
                    {
                        run->chaseState = chaseWords::freeRunner;
                        run->chaseError = 0.0;
                        ownSpeed();

                        for (const auto* below : runs.descendantsOf (id))
                            if (auto* under = runs.find (below->id); under != nullptr && ! under->isFinished())
                            {
                                under->chaseState = chaseWords::freeRunner;
                                under->chaseError = 0.0;
                            }

                        continue;
                    }

                    if (! forwards)
                    {
                        ownSpeed();
                        continue;
                    }

                    /*  A SOUND OR A MOVIE, and a movie's locked sounds against
                        the movie's second - at the movie's own factor, steered
                        first, so a catch-up moves the picture and its sound
                        together; a scene's runs each against its own start under
                        the member (AIV), as the walk placed it, each steered by
                        the member's method from its own error. */
                    const auto steerWithSounds = [&] (const std::string& runId, double start)
                    {
                        auto* steered = runs.find (runId);

                        if (steered == nullptr)
                            return;

                        const auto cue = document.findById (steered->cue);
                        steerChaser (engine, *steered, cue, member, plan, *map, start, *settings, byJump, false);

                        if (steered->kind != "video")
                            return;

                        std::vector<std::string> sounds;

                        for (const auto* child : runs.childrenOf (runId))
                            if (child->kind == "media" && ! child->isFinished() && followsAMovie (document.findById (child->cue)))
                                sounds.push_back (child->id);

                        for (const auto& sound : sounds)
                            if (auto* locked = runs.find (sound))
                                steerChaser (engine, *locked, cue, member, plan, *map, start, *settings, byJump, false,
                                             steered);
                    };

                    if (run->isGroup())
                    {
                        std::vector<std::pair<std::string, double>> under;

                        for (const auto* below : runs.descendantsOf (id))
                            if (! below->isFinished() && (below->kind == "media" || below->kind == "video"))
                                if (const auto found = plan.starts.find (below->cue); found != plan.starts.end())
                                    under.emplace_back (below->id, found->second);

                        for (const auto& [belowId, start] : under)
                            steerWithSounds (belowId, start);

                        continue;
                    }

                    steerWithSounds (id, member.preWait);
                }
            }
        }
    }

    //==========================================================================
    void Runner::endChaser (const std::string& runId, std::int64_t tick)
    {
        /*  EVERYTHING UNDER IT FIRST, then it: a scene's members and a movie's
            locked sounds end with it, and nothing watching sees a finished run
            with live ones below. */
        std::vector<std::string> ending;

        for (const auto* below : runs.descendantsOf (runId))
            ending.insert (ending.begin(), below->id);

        ending.push_back (runId);

        const auto at = videoSampleAhead();

        for (const auto& id : ending)
        {
            endHere (id, tick);

            /*  A PICTURE TAKEN DOWN a horizon ahead - a cut, as a chasing
                picture moves (AIT) - its job gone once its run is over. */
            for (auto& job : showing)
                if (job.self == id && ! job.removed)
                {
                    if (job.placed && videoSink != nullptr)
                        videoSink->remove (job.self, at >= 0 ? at : -1);

                    job.removed = true;
                }
        }
    }

    //==========================================================================
    std::vector<std::string> Runner::seatChasersOnCode (Engine& engine, std::int64_t tick, const std::string& source,
                                                        const std::string& groupRun, std::int64_t sample, bool locate,
                                                        const std::vector<std::string>& supplied)
    {
        std::vector<std::string> used;
        std::size_t taken = 0;

        const NameDraw nextId = [&] (const std::string& under, const std::string& ofCue)
        {
            auto id = taken < supplied.size() ? supplied[taken] : std::string {};
            ++taken;

            if (id.empty())
                id = namer.next (RunNames::causeOf (under, ofCue), ofCue);

            used.push_back (id);
            return id;
        };

        //  The marks of groups that are over, forgotten.
        std::erase_if (codeSeated, [this] (const std::string& id)
                       {
                           const auto* run = runs.find (id);
                           return run == nullptr || run->isFinished();
                       });

        /*  THE GROUPS: every time group armed on the source, or the one named
            - armed being its run playing, as a member fired by hand reads it
            (AJB), never its job's phase, which a replay does not move. */
        std::vector<std::string> groups;

        for (const auto& candidate : runs.all())
        {
            if (! candidate.isGroup() || candidate.state != runState::playing || candidate.takenBack
                  || beingKilled (candidate) || (! groupRun.empty() && candidate.id != groupRun))
                continue;

            if (const auto node = document.findById (candidate.cue);
                node.isValid() && node.hasType ("Group") && isTimeGroup (groupMode (node)))
                groups.push_back (candidate.id);
        }

        /*  WHERE THE CODE WILL BE WHEN A SEATED SOUND CAN START: a seat's lead
            after the record's own sample, so live and in a replay the same
            members are judged playing or not, and a sound seated now is armed
            at the second the code will be at when it launches there. */
        const auto launchAt = sample + static_cast<std::int64_t> (seatLeadTicks) * std::max (0, samplesPerTick);
        const auto* lengths = handlerDurations();
        const auto settings = chaseSettings();

        //  A sound or a scene the seat made, launched on the sample its offset is for, as a follower's is (RD.8).
        const auto placeSeated = [this, launchAt, tick] (const std::string& runId)
        {
            if (auto* made = runs.find (runId);
                made != nullptr && ! made->isFinished() && made->launchRequested
                  && (made->kind == "media" || made->kind == "mic"))
            {
                made->launchNotBefore = launchAt;
                made->launchRequestedAtTick = tick + seatLeadTicks;
            }
        };

        for (const auto& groupId : groups)
        {
            const auto* armed = runs.find (groupId);

            if (armed == nullptr)
                continue;

            const auto groupCue = armed->cue;
            const auto group = document.findById (groupCue);
            const auto& plan = timePlanOf (group);

            if (plan.source.empty() || (! source.empty() && plan.source != source))
                continue;

            const auto* map = codeMapOf (plan.source);

            if (map == nullptr || map->state != timecode::CodeState::running || ! std::isfinite (map->speed)
                  || ! (map->speed > 0.0))
                continue;

            codeSeated.insert (groupId);

            //  The groups above, as a seek of a scene reads them: what `seatPlan` hangs a member under.
            std::vector<std::string> above;
            std::map<std::string, std::string> runFor;
            runFor[groupCue] = groupId;

            for (auto parent = armed->parent; ! parent.empty();)
            {
                const auto* up = runs.find (parent);

                if (up == nullptr)
                    break;

                above.insert (above.begin(), up->cue);
                runFor[up->cue] = up->id;
                parent = up->parent;
            }

            above.push_back (groupCue);

            for (const auto& member : plan.members)
            {
                /*  WHAT OF IT LIVES UNDER THE GROUP: playing - heard, or a scene
                    entered; asked for and not heard yet; loaded and waiting for
                    the code. A run on its way out is its stop's. */
                std::vector<std::string> playingRuns, askedRuns, loadedRuns;

                for (const auto* child : runs.childrenOf (groupId))
                {
                    if (child->cue != member.cue || child->isFinished() || child->state == runState::stopping)
                        continue;

                    if (child->state == runState::playing && (child->isGroup() || child->startedAtTick >= 0))
                        playingRuns.push_back (child->id);
                    else if (child->launchRequestedAtTick != 0)
                        askedRuns.push_back (child->id);
                    else
                        loadedRuns.push_back (child->id);
                }

                const auto cutAsked = [&]
                {
                    if (locate)
                        for (const auto& id : askedRuns)
                            endChaser (id, tick);
                };

                //  A FREE RUNNER: never seated, never moved (AIH) - only what was asked for on the old line, cut.
                if (! chasesTheCode (member.chase))
                {
                    cutAsked();
                    continue;
                }

                const auto cue = document.findById (member.cue);
                std::optional<std::int64_t> frame;

                if (cue.isValid())
                    frame = frameOnCode (*map, member.at, plan.showRate);

                if (! frame.has_value())
                {
                    cutAsked();
                    continue;
                }

                const auto seconds = chase::codeSecondsSince (*map, *frame, launchAt);
                const auto byJump = member.chase == "jump";

                //--------------------------------------------------------------
                /*  A SCENE (AIV): playing between its time and its end - its
                    own pre-wait and its round, which the walk knows or the
                    scene never ends. One playing is sought within itself, its
                    members re-seated around the second (`seekGroup`); one not,
                    or one a seek cannot move, is seated with the round solved
                    there. */
                if (member.kind == "group")
                {
                    const auto round = membersOf (cue);
                    const auto whole = solveRound (document, lengths, cue, round, 0.0);
                    const auto over = whole.length.has_value() && seconds >= member.preWait + *whole.length;

                    if (seconds < 0.0 || over)
                    {
                        if (locate)
                        {
                            for (const auto& id : playingRuns)
                                endChaser (id, tick);

                            cutAsked();
                        }

                        continue;
                    }

                    if (! playingRuns.empty())
                    {
                        if (! locate)
                            continue;

                        const std::vector<std::string> rest (supplied.begin() + static_cast<std::ptrdiff_t> (std::min (taken, supplied.size())),
                                                             supplied.end());
                        const auto made = seekGroup (engine, tick, playingRuns.back(), seconds, rest);

                        taken += made.size();
                        used.insert (used.end(), made.begin(), made.end());

                        for (const auto& id : made)
                            placeSeated (id);

                        if (! made.empty())
                        {
                            cutAsked();
                            continue;
                        }

                        endChaser (playingRuns.back(), tick);
                    }
                    else if (! locate && ! (askedRuns.empty() && loadedRuns.empty()))
                    {
                        continue;
                    }

                    for (const auto& id : askedRuns)
                        endChaser (id, tick);

                    for (const auto& id : loadedRuns)
                        endChaser (id, tick);

                    const auto inRound = solveRound (document, lengths, cue, round, seconds - member.preWait);

                    if (inRound.runs.empty())
                        continue;

                    std::vector<PlannedRun> wanted;

                    PlannedRun self;
                    self.cue = member.cue;
                    self.ancestors = above;
                    self.offset = seconds;
                    wanted.push_back (self);

                    for (auto run : inRound.runs)
                    {
                        run.ancestors.insert (run.ancestors.begin(), above.begin(), above.end());
                        wanted.push_back (std::move (run));
                    }

                    const auto from = used.size();
                    seatPlan (engine, tick, wanted, runFor, nextId);

                    for (auto index = from; index < used.size(); ++index)
                        placeSeated (used[index]);

                    continue;
                }

                //--------------------------------------------------------------
                const auto target = chaseTargetOf (cue, member, plan, *map, launchAt, member.preWait, lengths);
                const auto sounding = target.has_value() && target->sounding();

                /*  NOT PLAYING THERE - its time ahead, its material over - and
                    playing: ended, as a jump ends what it abandons. Loaded and
                    waiting, it is the group's hook's: kept for a time still in
                    its window, let go otherwise (TG.7). */
                if (! sounding)
                {
                    if (locate)
                    {
                        for (const auto& id : playingRuns)
                            endChaser (id, tick);

                        cutAsked();
                    }

                    continue;
                }

                /*  PLAYING, AND IT SHOULD BE: brought into step where it is,
                    never started again. Into another of its slices by a seek -
                    the run's slice is handler state, so a replay seeks the same -
                    said, since a seek has a gap; in its own, or with none, by a
                    jump on the voice; a movie by a cut, its locked sounds with
                    it. What else of it was on its way is cut. */
                if (! playingRuns.empty())
                {
                    if (! locate)
                        continue;

                    cutAsked();

                    for (const auto& id : loadedRuns)
                        endChaser (id, tick);

                    auto* run = runs.find (playingRuns.back());

                    if (run == nullptr)
                        continue;

                    if (run->kind == "media" && run->range != target->range)
                    {
                        const auto id = run->id;

                        if (seekMedia (engine, tick, id, target->file, false))
                            if (auto* sought = runs.find (id))
                            {
                                sought->launchNotBefore = launchAt;
                                sought->launchRequestedAtTick = tick + seatLeadTicks;
                                sought->warning = runWarning::sought;
                                sought->chaseState = chaseWords::jumped;
                            }

                        continue;
                    }

                    steerChaser (engine, *run, cue, member, plan, *map, member.preWait, settings, byJump, true);

                    if (run->kind == "video")
                    {
                        std::vector<std::string> sounds;

                        for (const auto* child : runs.childrenOf (run->id))
                            if (child->kind == "media" && ! child->isFinished() && followsAMovie (document.findById (child->cue)))
                                sounds.push_back (child->id);

                        for (const auto& sound : sounds)
                            if (auto* locked = runs.find (sound))
                                steerChaser (engine, *locked, cue, member, plan, *map, member.preWait, settings, byJump, true,
                                             run);
                    }

                    continue;
                }

                //  NOT PLAYING, AND IT SHOULD BE: seated - first sight, only if nothing of it is on its way.
                if (! locate && ! (askedRuns.empty() && loadedRuns.empty()))
                    continue;

                for (const auto& id : askedRuns)
                    endChaser (id, tick);

                for (const auto& id : loadedRuns)
                    endChaser (id, tick);

                /*  A PICTURE fired again, as a follower's is (RD.8): a movie
                    sought to the second the code will be at when it comes up, a
                    horizon ahead; a still shown. */
                if (member.kind == "video")
                {
                    const auto id = nextId (groupId, member.cue);
                    createRun (id, member.cue, "video", groupId);

                    if (auto* made = runs.find (id))
                    {
                        made->preWaitTicks = 0;
                        made->postWaitTicks = ticksFor (numberOf (cue, "postWait"));
                    }

                    fireNow (engine, tick, id);

                    if (textOf (cue, "source") == "movie")
                    {
                        const auto showsAt = audio != nullptr ? videoSampleAhead() : launchAt;

                        if (const auto there = chaseTargetOf (cue, member, plan, *map, showsAt, member.preWait, lengths);
                            there.has_value() && there->sounding())
                            seekMovie (engine, tick, id, there->file, false);
                    }

                    continue;
                }

                //  A SOUND, at its offset, by load to time's own builder.
                PlannedRun wants;
                wants.cue = member.cue;
                wants.ancestors = above;
                wants.offset = target->file;
                wants.range = target->range;
                wants.pass = target->pass;

                const auto from = used.size();
                seatPlan (engine, tick, { wants }, runFor, nextId);

                for (auto index = from; index < used.size(); ++index)
                    placeSeated (used[index]);
            }
        }

        return used;
    }
}
