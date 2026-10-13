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
    A LEADER'S POSITION, MADE AND SEATED (namespace draft §58, RD.8; the
    Runner's, in a file of its own beside Runner.cpp). pair/Position.h says
    what a position is and why it is text; this is where the Runner says what
    it holds and where a follower is seated at it.

    WHAT IS CARRIED AND WHAT IS NOT, decided here run by run:

      - EVERY LIVE RUN, and the finished runs of every live scene, since a
        scene's job counts its members by their runs: a timeline missing the
        runs of its members already over would wait for them for ever.
      - NOT WHAT THE HORIZON ONLY MADE READY: a standby's arm, a scene
        prepared ahead (PRD §3.12). Each machine prepares for itself (AGE) -
        its own disk, its own pre-sends, held on a follower - and the seat
        drops the follower's own and lets its horizon prepare the standby the
        seat put there, under the names the leader's did: the counts carried
        are the leader's less what its horizon made, so the follower's draws
        land on the same names.
      - NOT DOH!'S BOOKS: the GO it would take back, what that GO changed on
        the desks, the cues it paused to carry on. They are the record of a GO
        this machine did not take under this state, and a Doh! applied on
        them here would take back something else; the seat forgets this
        machine's own as a jump forgets them (`forgetGoOnJump`). A Doh! the
        leader takes within its window after a seat is refused here and said
        (RD.6's count), and the summary shows the result; `pair.realign` seats
        again.
      - NOT A PASS RECORDING LANES, which is this machine's hands on its own
        faders; the faders flipped and the lanes armed are carried.
      - A SOUND'S SPEED A FADE OR AN EDIT MOVED is carried as the fade's job
        when one is moving it; a speed left somewhere by one that ended is not,
        and the summary says its playhead drifts.

    HOW THE FOLLOWER'S OWN RUNS ARE ENDED. A run of this machine's the
    position holds as it is - the same name, cue, kind and parent, both live -
    stands, and is corrected: its fields set from the leader's, a sound sought
    to the leader's playhead when it is more than two ticks out. Every other
    run of its own is ended the way a jump ends what it abandons (`endHere`):
    done on the model, no footer, no post-wait, no fade, no value put back -
    a seat is not a stop, and the machine following holds everything meant for
    a device anyway (RD.1), so nothing reaches a device either way - its voice
    stopped, its slots and jobs let go; a picture it showed taken down (AGH:
    video is not held, and the leader is not showing it). And then it is taken
    out of the run table, finished ones with it: the follower holds the
    leader's run table, so the leader's run can be made under the same name and
    every name drawn from then on finds the same names taken on both machines.
*/

#include <wfg/engine/cue/Runner.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/document/Schema.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace wfg::cue
{
    namespace
    {
        constexpr double tickSeconds = 1.0 / static_cast<double> (TickClock::rateHz);

        double secondsOf (std::int64_t ticks) noexcept
        {
            return static_cast<double> (ticks) * tickSeconds;
        }

        /*  THE ROWS `state.xml` HOLDS, as EphemeralState collects them, but
            every one at its value or its default - a standby the leader
            cleared must clear the follower's - and never a row that names this
            machine's hardware (RD.3): a port's identifier, the patch's settled
            flags, a display's identifier are each machine's own. */
        void collectRows (const doc::ShowDocument& document, const juce::ValueTree& node,
                          std::vector<pair::Position::Row>& out)
        {
            const auto elementName = node.getType().toString().toStdString();
            const auto* element = doc::Schema::instance().element (elementName);
            const auto segment = doc::ShowDocument::containerSegmentFor (elementName);
            const auto identified = node.hasProperty (idProperty);

            if (element != nullptr && (identified || ! segment.empty()))
            {
                const auto prefix = ! segment.empty()
                                      ? "/godot/" + std::string (segment)
                                      : "/godot/" + std::string (doc::ShowDocument::ownerForElement (elementName)) + "/"
                                          + node[idProperty].toString().toStdString();

                for (const auto& attribute : element->attributes)
                {
                    if (attribute.persist() != doc::Persist::state || attribute.isMachine())
                        continue;

                    auto address = prefix + "/" + std::string (attribute.name());

                    if (auto value = document.getAttribute (address))
                        out.push_back ({ std::move (address), std::move (*value) });
                }
            }

            for (const auto& child : node)
                collectRows (document, child, out);
        }

        std::vector<std::string> listIdsOf (const doc::ShowDocument& document)
        {
            std::vector<std::string> out;

            for (const auto& list : document.root().getChildWithName ("Lists"))
                if (list.hasType ("List"))
                    out.push_back (list[idProperty].toString().toStdString());

            return out;
        }

        /*  The kinds whose run carries on by a road of its own when seated,
            fired again under the leader's name and sought to its playhead: a
            movie, a patch, an OSC cue's curves. */
        bool refiredKind (const pair::SeatedRun& run)
        {
            return run.kind == "video" || run.kind == "process" || (run.kind == "osc" && run.curving);
        }

        /*  A run asked for and not yet begun by anything: armed, its launch
            never asked - a member a sequence spawned ahead of its turn, a
            sampler's armed member. Made as its group's job makes one. */
        bool armedAhead (const pair::SeatedRun& run)
        {
            return run.state == runState::armed && ! run.launchRequested && ! run.launched && run.launchTick == 0;
        }
    }

    //==========================================================================
    std::vector<const Run*> Runner::carriedRuns() const
    {
        std::vector<const Run*> out;
        std::set<std::string> left, liveGroups;

        for (const auto& run : runs.all())
        {
            /*  WHAT THE HORIZON MADE READY, AND EVERYTHING UNDER IT: each
                machine prepares for itself. */
            if ((! run.parent.empty() && left.count (run.parent) > 0) || (! run.isFinished() && run.onlyPrepared()))
            {
                left.insert (run.id);
                continue;
            }

            if (! run.isFinished())
            {
                out.push_back (&run);

                if (run.isGroup())
                    liveGroups.insert (run.id);

                continue;
            }

            //  A member a live scene has played, which its job counts.
            if (! run.parent.empty() && liveGroups.count (run.parent) > 0)
                out.push_back (&run);
        }

        return out;
    }

    double Runner::playheadOf (const Run& run, std::int64_t tick) const
    {
        if (run.isWaiting())
            return -secondsOf (std::max<std::int64_t> (0, run.dueTick - tick));

        if (run.kind == "media" && run.range >= 0)
            return run.slicePlayed;

        return run.position;
    }

    //==========================================================================
    pair::Position Runner::positionNow (std::int64_t tick, std::uint64_t counter) const
    {
        pair::Position at;
        at.counter = counter;
        at.tick = tick;

        collectRows (document, document.root(), at.rows);

        for (const auto& listId : listIdsOf (document))
            at.lists.push_back ({ listId, lists.aimOf (listId), lists.positionOf (listId), lists.historyOf (listId) });

        at.lastGoTick = lastGoTick;
        at.lastDohTick = lastDohTick;
        at.goCount = goCount;

        const auto carried = carriedRuns();
        std::set<std::string> carriedIds;

        for (const auto* run : carried)
        {
            carriedIds.insert (run->id);

            pair::SeatedRun seated;
            seated.id = run->id;
            seated.cue = run->cue;
            seated.kind = run->kind;
            seated.parent = run->parent;
            seated.state = run->state;
            seated.launchTick = run->launchRequestedAtTick;
            seated.dueTick = run->dueTick;
            seated.postWaitBegan = run->postWaitBegan;
            seated.startedTick = run->startedAtTick;
            seated.endedTick = run->endedAtTick;
            seated.launchRequested = run->launchRequested;
            seated.launched = run->launchedAtSample > 0;
            seated.position = run->position;
            seated.slicePlayed = run->slicePlayed;
            seated.range = run->range;
            seated.pass = run->rangeIteration;
            seated.rate = run->rateNow;
            seated.ownRate = run->ownRate;
            seated.iteration = run->iteration;
            seated.iterations = run->iterations;
            seated.seed = run->seed;
            seated.round = run->round;
            seated.roundStartedTick = run->roundStartedAtTick;
            seated.firstRoundTick = run->firstRoundAtTick;
            seated.pruned = run->pruned;
            seated.stopAfter = run->stopAfter;
            seated.enterAt = run->enterAt;
            seated.ownLevel = run->ownLevel;
            seated.trim = run->trim;
            seated.strip = run->strip;
            seated.sampler = run->sampler;
            seated.held = run->held;
            seated.heldBy = run->heldBy;
            seated.solo = run->solo;
            seated.closing = run->closing;
            seated.lostStrips = run->lostStrips;
            seated.moved = run->moved;
            seated.stopAsked = run->stopAsked;
            seated.stopEndsWait = run->stopEndsWait;
            seated.skipFooter = run->skipFooter;
            seated.killed = run->killed;
            seated.asserted = run->asserted;
            seated.advanceRequested = run->advanceRequested;
            seated.advanceTo = run->advanceTo;
            seated.goSerial = run->goSerial;
            seated.curving = run->kind == "osc" && isCurving (run->id);
            at.runs.push_back (std::move (seated));
        }

        //  THE JOBS CARRYING THEM ON: each live scene's place, each fade's progress.
        for (const auto& job : scheduled)
            if (! job.retired && carriedIds.count (job.run) > 0)
                at.groups.push_back (job);

        for (const auto& job : running)
        {
            if (job.retired)
                continue;

            const auto held = job.heldRun();

            if (! held.empty() && carriedIds.count (held) == 0)
                continue;

            if (job.reportsSelf && ! job.self.empty() && carriedIds.count (job.self) == 0)
                continue;

            auto copy = job;
            copy.handSerial = 0;        // this machine's count of hands, which means nothing on the other
            at.fades.push_back (std::move (copy));
        }

        for (const auto& id : finishing)
            if (carriedIds.count (id) > 0)
                at.finishing.push_back (id);

        if (dcas != nullptr)
            at.dcas = dcas->all();

        /*  EACH INCOMING CODE'S MAP (namespace draft §60, TG.5; AIP), as this
            machine's `code.heard` records left it: a follower seated while a
            code runs is locked to it from the seat, as if it had applied every
            turn the leader heard. */
        at.codes = codesHeard;

        /*  EACH TEXT CUE'S FIELD (namespace draft §61, AKM): tonight's values,
            so a follower seated after a score changed shows the score. */
        if (textFields != nullptr)
            at.fields = textFields->all();

        if (lanes != nullptr && lanes->flipped())
        {
            at.laneCue = lanes->cue();
            at.laneArmed.assign (lanes->armedKeys().begin(), lanes->armedKeys().end());
        }

        at.suspended.assign (suspended.begin(), suspended.end());
        at.liftedAt = liftedAt;

        for (const auto& [cueId, bed] : paused)
            at.paused.push_back ({ cueId, bed.run, bed.resumes, bed.at.from, bed.at.range, bed.media });

        /*  THE NAMES, LESS WHAT THE HORIZON MADE (AGE): the follower's own
            horizon prepares the standby the seat puts there, and draws the
            same names only from the same counts. */
        at.names = namer.position();

        {
            std::set<std::string> left;

            for (const auto& run : runs.all())
            {
                if ((! run.parent.empty() && left.count (run.parent) > 0) || (! run.isFinished() && run.onlyPrepared()))
                {
                    left.insert (run.id);

                    if (auto found = at.names.counts.find ({ RunNames::causeOf (run.parent, run.cue), run.cue });
                        found != at.names.counts.end() && found->second > 0)
                        --found->second;
                }
            }
        }

        return at;
    }

    //==========================================================================
    pair::Summary Runner::summaryNow (std::int64_t tick, std::uint64_t counter) const
    {
        pair::Summary out;
        out.tick = tick;
        out.counter = counter;

        for (const auto& listId : listIdsOf (document))
            out.standby.push_back ({ listId, document.getAttribute (standbyAddressOf (listId)).value_or (std::string {}) });

        /*  ONE PASS OVER THE LIVE RUNS, every fifth tick: what the horizon only
            made ready left out, and what is under it - rarely anything, so the
            set of them is nearly always empty and never looked in. */
        std::set<std::string> left;
        out.runs.reserve (16);

        for (const auto& run : runs.all())
        {
            if (run.isFinished())
                continue;

            if (run.onlyPrepared() || (! left.empty() && ! run.parent.empty() && left.count (run.parent) > 0))
            {
                left.insert (run.id);
                continue;
            }

            const auto looping = run.kind == "media" && run.range >= 0 && run.passSamples > 0 && samplesPerTick > 0;

            out.runs.push_back ({ run.id, run.cue, run.state,
                                  static_cast<std::int64_t> (std::llround (playheadOf (run, tick) / tickSeconds)),
                                  run.kind == "media" ? run.range : -1,
                                  looping ? static_cast<std::int64_t> (std::llround (static_cast<double> (run.passSamples)
                                                                                      / static_cast<double> (samplesPerTick)))
                                          : std::int64_t { 0 } });
        }

        return out;
    }

    //==========================================================================
    void Runner::seatAt (Engine& engine, doc::ShowDocument& editable, std::int64_t tick, const pair::Position& at)
    {
        seatSaid.clear();

        const auto note = [this] (std::string sentence)
        {
            if (std::find (seatSaid.begin(), seatSaid.end(), sentence) == seatSaid.end())
                seatSaid.push_back (std::move (sentence));
        };

        /*  HOW FAR THE LEADER'S WORLD HAS MOVED ON since the position was true:
            nought for a seat applied in its slot, more for one that arrived
            late. And how far a sound seated now has to be ahead of where it
            was then: that, the launch latency, and the lead a seated sound's
            cold arm is given (`seatLeadTicks`), after which it is launched on
            the sample its offset is for. On the record clock, as every record
            of the leader's is applied here - Δ behind the leader in time,
            which RD.9's launch mapping takes up for every sound at once. */
        const auto since = std::max<std::int64_t> (0, tick - at.tick);
        const auto launchIn = static_cast<std::int64_t> (std::max (0, latencyTicks()) + seatLeadTicks);
        const auto lead = since + launchIn;

        //----------------------------------------------------------------------
        /*  THE ROWS, through the document's own door, so each is checked as a
            client's write would be: one this show cannot take - a standby on a
            cue it does not have - is said and left. */
        for (const auto& row : at.rows)
        {
            if (editable.getAttribute (row.address) == std::optional<std::string> (row.value))
                continue;

            if (const auto edit = editable.setAttribute (row.address, row.value); ! edit.ok)
                note ("this show could not take " + row.address + " (" + edit.reason + ")");
        }

        //----------------------------------------------------------------------
        /*  DOH!'S BOOKS, FORGOTTEN as a jump forgets them: they describe a GO
            this machine did not take under the state it now has. */
        for (const auto& listId : listIdsOf (document))
        {
            forgetGoOnJump (engine, tick, listId);
            lists.setResume (listId, {});
        }

        goRecord = {};
        goChanges = {};
        lists.setDohOffer ({});
        lists.setDohForget ({});
        takenBackSerials.clear();
        leftByGo.clear();
        pendingRelaunches.clear();
        relaunchAsked.clear();
        marks.clear();
        lastDohList.clear();
        playheadsOwed.clear();
        persistentLeft.clear();
        persistentAllLeft = false;

        //----------------------------------------------------------------------
        //  WHAT EACH LIST KEEPS BESIDE ITS ROWS, on this machine's clock.
        for (const auto& list : at.lists)
        {
            lists.aimAt (list.id, list.aim);
            lists.landedAt (list.id, list.landed);
            lists.seatHistory (list.id, list.history);
        }

        lastGoTick = at.lastGoTick;
        lastDohTick = at.lastDohTick;
        goCount = at.goCount;
        currentGo = 0;

        suspended = std::set<std::string> (at.suspended.begin(), at.suspended.end());
        liftedAt = at.liftedAt;
        paused.clear();

        for (const auto& bed : at.paused)
            paused[bed.cue] = { bed.run, bed.resumes, { bed.from, bed.range }, bed.media };

        owed.clear();
        playheads.clear();

        //----------------------------------------------------------------------
        /*  WHICH OF THIS MACHINE'S RUNS STAND: live here and live there, with
            the same name, cue, kind and parent - and a parent that stands. */
        std::map<std::string, const pair::SeatedRun*> theirs;

        for (const auto& run : at.runs)
            theirs.emplace (run.id, &run);

        std::set<std::string> kept;

        for (const auto& own : runs.all())
        {
            if (own.isFinished() || own.onlyPrepared())
                continue;

            const auto found = theirs.find (own.id);

            if (found == theirs.end())
                continue;

            const auto& their = *found->second;

            if (their.isFinished() || their.cue != own.cue || their.kind != own.kind || their.parent != own.parent)
                continue;

            /*  IN THE SAME STATE, a sound on its way to its launch and one
                launched read as one: each machine launches on a tick of its
                own. A run stopping on one and not the other is made again. */
            const auto launching = [] (const std::string& state)
            {
                return state == runState::armed || state == runState::playing;
            };

            if (their.state != own.state && ! (launching (their.state) && launching (own.state)))
                continue;

            if (! own.parent.empty() && kept.count (own.parent) == 0)
                continue;

            kept.insert (own.id);
        }

        //----------------------------------------------------------------------
        /*  EVERY OTHER ENDED, AS A JUMP ENDS WHAT IT ABANDONS, AND TAKEN OUT.
            The jobs that would otherwise find a run of the same name made
            again below - a picture's, a patch's, a curve's, a send's - let go
            of it now; a scene's job is retired by `endHere`, a fade's are all
            replaced below. */
        std::set<std::string> gone;

        for (const auto& own : runs.all())
            if (kept.count (own.id) == 0)
                gone.insert (own.id);

        for (const auto& id : gone)
            endHere (id, tick);

        for (auto& job : showing)
            if (gone.count (job.self) > 0)
            {
                if (job.placed && ! job.removed && videoSink != nullptr)
                    videoSink->remove (job.self, -1);

                job.removed = true;
            }

        std::erase_if (showing, [&gone] (const VideoJob& job) { return gone.count (job.self) > 0; });
        std::erase_if (curving, [&gone] (const CurveJob& job) { return gone.count (job.self) > 0; });
        std::erase_if (processing, [&gone] (const ProcessJob& job) { return gone.count (job.self) > 0; });
        std::erase_if (sending, [&gone] (const OscJob& job) { return gone.count (job.self) > 0; });
        std::erase_if (videoFades, [&gone] (const VideoFade& job)
                       {
                           return gone.count (job.self) > 0 || gone.count (job.target) > 0;
                       });

        for (const auto& id : gone)
            runs.remove (id);

        //  THE HOOKS' MEMORY OF RUNS NOW GONE, and of the standby they prepared.
        armedStandby.clear();
        blocksToGiveBack.clear();
        owedPutBacks.clear();
        waitingForVoice.clear();
        stripEdges.clear();

        //----------------------------------------------------------------------
        /*  THE LEADER'S RUNS, under the leader's names. Scenes and sounds go
            through `seatPlan`, as a jump's do - each at its playhead, a scene
            at its second, a member in its wait or its post-wait at the tick it
            comes due; what `seatPlan` does not make is made here after it: a
            scene's members over already, a member spawned ahead of its turn,
            a movie, a patch or curves fired again and sought, a fade's run
            carried on by its job. */
        const auto ancestorsOf = [&theirs] (const pair::SeatedRun& run)
        {
            std::vector<std::string> out;

            for (auto parent = run.parent; ! parent.empty();)
            {
                const auto found = theirs.find (parent);

                if (found == theirs.end())
                    break;

                out.insert (out.begin(), found->second->cue);
                parent = found->second->parent;
            }

            return out;
        };

        std::vector<PlannedRun> wanted;
        std::map<std::pair<std::string, std::string>, std::deque<std::string>> named;
        std::vector<const pair::SeatedRun*> after;
        std::set<std::string> outsidePlan;

        for (const auto& their : at.runs)
        {
            if (kept.count (their.id) > 0)
                continue;

            //  Made by its parent when its parent is made - a movie's own sounds - or not at all.
            if (! their.parent.empty() && outsidePlan.count (their.parent) > 0)
            {
                outsidePlan.insert (their.id);
                continue;
            }

            const auto cueNode = document.findById (their.cue);

            if (! cueNode.isValid())
            {
                note ("cue " + their.cue + " runs on the leader and is not in this show");
                outsidePlan.insert (their.id);
                continue;
            }

            /*  IN A WAIT, whatever its kind, `seatPlan` makes it waiting for the
                tick it comes due, as a jump does. Otherwise only a scene, a
                sound and a code (namespace draft §59, TC.7) are its to make. */
            const auto waits = their.state == runState::waiting || their.state == runState::postWait;

            if (their.isFinished()
                  || (! waits && (armedAhead (their) || refiredKind (their)
                                    || (their.kind != "group" && their.kind != "media" && their.kind != "mic"
                                          && their.kind != "timecode"))))
            {
                after.push_back (&their);

                if (! their.isFinished())
                    outsidePlan.insert (their.id);

                continue;
            }

            PlannedRun wants;
            wants.cue = their.cue;
            wants.ancestors = ancestorsOf (their);

            if (their.state == runState::waiting)
            {
                wants.when = planned::due;
                wants.startsIn = secondsOf (std::max<std::int64_t> (0, their.dueTick - tick));
            }
            else if (their.state == runState::postWait)
            {
                wants.when = planned::postWait;
                wants.startsIn = secondsOf (std::max<std::int64_t> (0, their.dueTick - tick));
            }
            else if (their.kind == "group")
            {
                wants.when = planned::sounding;
                wants.offset = secondsOf (std::max<std::int64_t> (0, tick - their.launchTick));
                wants.round = std::max (1, their.iteration);
                wants.roundFrom = their.iteration >= 1
                                    ? secondsOf (std::max<std::int64_t> (0, their.roundStartedTick - their.launchTick))
                                    : -1.0;
            }
            else
            {
                /*  A SOUND AT ITS PLAYHEAD, as far on as the leader's will be
                    when this one starts: the seat's lateness, and the launch
                    latency and the cold arm's lead it is launched after
                    (`launchNotBefore`, below), at the speed it plays. Inside
                    its slice, passes and all, for one that plays slices. */
                wants.when = planned::sounding;

                /*  A CODE (TC.7) at the frame the leader's will have reached
                    when this one starts, counted from its `run.started` -
                    nothing of a code is a voice launched - and at the speed of
                    the clock, which is the only speed a code has; its
                    position is how far past its start it is. */
                const auto heardThere = their.kind == "timecode" ? their.startedTick != -1 : their.launched;
                const auto speed = their.kind == "timecode" ? 1.0 : std::abs (their.rate);
                const auto moved = heardThere ? secondsOf (lead) * speed : 0.0;
                const auto ranges = rangesOf (cueNode);

                if (their.range >= 0 && static_cast<std::size_t> (their.range) < ranges.size())
                {
                    const auto& slice = ranges[static_cast<std::size_t> (their.range)];
                    const auto length = slice.out - slice.in;
                    auto played = their.slicePlayed + moved;

                    if (slice.loops > 0 && length > 0.0)
                        played = std::min (played, static_cast<double> (slice.loops) * length - 0.001);

                    const auto passes = length > 0.0 ? std::floor (played / length) : 0.0;
                    wants.range = their.range;
                    wants.pass = static_cast<int> (passes) + 1;
                    wants.offset = slice.in + (played - passes * length);
                }
                else
                {
                    wants.offset = their.position + moved;
                }
            }

            wanted.push_back (std::move (wants));
            named[{ their.parent, their.cue }].push_back (their.id);
        }

        std::map<std::string, std::string> runFor;

        for (const auto& own : runs.all())
            if (kept.count (own.id) > 0)
                runFor[own.cue] = own.id;

        const NameDraw nextId = [this, &named, &note] (const std::string& under, const std::string& ofCue)
        {
            auto& queue = named[{ under, ofCue }];

            if (! queue.empty())
            {
                auto id = queue.front();
                queue.pop_front();
                return id;
            }

            /*  A RUN THIS MACHINE PLACES UNDER ANOTHER PARENT THAN THE LEADER
                DID - a cue moved since in one of the two shows - takes a name
                of its own, and is said. */
            note ("cue " + ofCue + " is placed otherwise here than on the leader");
            return namer.next (RunNames::causeOf (under, ofCue), ofCue);
        };

        seatPlan (engine, tick, wanted, runFor, nextId);

        /*  AND EACH SOUND IT MADE LAUNCHED ON THE SAMPLE ITS OFFSET IS FOR, not
            as soon as its cold arm answers; its launch asked for then too, so
            one launched on time is not reported late. */
        const auto launchSample = (tick + launchIn) * static_cast<std::int64_t> (samplesPerTick);

        for (const auto& wants : wanted)
            if (wants.when == planned::sounding)
                if (const auto found = runFor.find (wants.cue); found != runFor.end() && kept.count (found->second) == 0)
                    if (auto* sound = runs.find (found->second);
                        sound != nullptr && (sound->kind == "media" || sound->kind == "mic" || sound->kind == "timecode"))
                    {
                        sound->launchNotBefore = launchSample;
                        sound->launchRequestedAtTick = tick + seatLeadTicks;

                        /*  A CODE (TC.7) on that same sample, and with the sounds
                            of its group seated beside it - waited for when one
                            is slow to arm, so the two still start together. */
                        for (auto& job : timecoding)
                            if (job.self == sound->id)
                                job.firedAtTick = sound->launchRequestedAtTick;
                    }

        for (const auto* their : after)
        {
            if (runs.find (their->id) != nullptr)
                continue;

            const auto cueNode = document.findById (their->cue);

            if (their->isFinished())
            {
                createRun (their->id, their->cue, their->kind, their->parent);

                if (auto* made = runs.find (their->id))
                {
                    made->state = their->state;
                    made->endedAtTick = tick - retentionTicks - 1;
                }

                continue;
            }

            if (armedAhead (*their))
            {
                spawnChild (engine, their->parent, their->cue, their->id, tick);
                continue;
            }

            if (refiredKind (*their))
            {
                createRun (their->id, their->cue, their->kind, their->parent);

                if (auto* made = runs.find (their->id))
                {
                    made->preWaitTicks = 0;
                    made->postWaitTicks = ticksFor (numberOf (cueNode, "postWait"));
                }

                fireNow (engine, tick, their->id);

                /*  A movie at the second the leader's will be at when its
                    picture comes up, a horizon ahead as a sound's launch is;
                    curves on the tick clock, at the second the leader's are. */
                if (their->kind == "video")
                    seekMovie (engine, tick, their->id,
                               their->position + secondsOf (since + std::max (0, latencyTicks())) * std::abs (their->rate),
                               true);
                else if (their->kind == "osc")
                    seekCurves (their->id, their->position + secondsOf (since), tick);

                continue;
            }

            /*  A FADE OR A STOP CUE'S RUN carries on by its job, carried below;
                a memo, a start cue or a stop cue's own run that ends on the
                next tick ends on this machine's next tick, as the leader's
                does (`finishing`, carried); any other - a MIDI message, an OSC
                cue waiting for its device's answer - has nothing here to end
                it, and is made over and said: the leader's ends in moments,
                and the summary says so meanwhile. */
            const auto drivenByAFade = std::any_of (at.fades.begin(), at.fades.end(),
                                                    [their] (const FadeJob& job) { return job.self == their->id; });
            const auto endsNextTick = std::find (at.finishing.begin(), at.finishing.end(), their->id)
                                        != at.finishing.end();

            createRun (their->id, their->cue, their->kind, their->parent);

            if (auto* made = runs.find (their->id))
            {
                made->preWaitTicks = ticksFor (numberOf (cueNode, "preWait"));
                made->postWaitTicks = ticksFor (numberOf (cueNode, "postWait"));

                if (drivenByAFade || endsNextTick)
                {
                    made->state = their->state;

                    if (endsNextTick)
                        finishing.push_back (their->id);
                }
                else
                {
                    made->state = runState::done;
                    made->endedAtTick = tick;
                    note ("cue " + their->cue + " (" + their->kind + ") is over here and still runs on the leader");
                }
            }
        }

        //----------------------------------------------------------------------
        /*  WHAT EACH RUN CARRIES THAT ITS CUE DOES NOT SAY, on every run the
            seat made or kept: its clocks, its rounds, its level, the hand on
            it, a held clip, a stop asked of it - and a sound kept, sought to
            the leader's playhead when it is more than two ticks out. */
        for (const auto& their : at.runs)
        {
            auto* run = runs.find (their.id);

            if (run == nullptr || run->isFinished())
                continue;

            const auto sound = run->kind == "media" || run->kind == "mic";

            if (! sound && ! refiredKind (their))
                run->launchRequestedAtTick = their.launchTick;

            if (run->isWaiting())
            {
                run->dueTick = their.dueTick;
                run->postWaitBegan = their.postWaitBegan;
            }

            /*  WHEN IT WAS HEARD is this machine's own: a sound seated here is
                reported started when its launch is placed here, and a pause
                counts its playhead from that. */

            if (run->isGroup())
            {
                run->iteration = their.iteration;
                run->iterations = their.iterations;
                run->seed = their.seed;
                run->round = their.round;
                run->roundStartedAtTick = their.roundStartedTick;
                run->firstRoundAtTick = their.firstRoundTick;
                run->pruned = their.pruned;
                run->stopAfter = their.stopAfter;
                run->enterAt = their.enterAt;
                run->closing = their.closing;
                run->lostStrips = their.lostStrips;

                if (their.state == runState::stopping || their.state == runState::playing)
                    run->state = their.state;
            }

            run->ownLevel = their.ownLevel;
            run->trim = their.trim;
            run->held = their.held;
            run->heldBy = their.heldBy;
            run->solo = their.solo;
            run->moved = their.moved;
            run->stopAsked = their.stopAsked;
            run->stopEndsWait = their.stopEndsWait;
            run->skipFooter = their.skipFooter;
            run->killed = their.killed;
            run->asserted = their.asserted;
            run->advanceRequested = their.advanceRequested;
            run->advanceTo = their.advanceTo;
            run->goSerial = their.goSerial;

            if (their.state == runState::stopping && ! run->isGroup())
                run->state = runState::stopping;

            /*  A SOUND THAT STOOD, more than two ticks from where the leader's
                will be once the seat is applied: sought there, its voice
                stopped and asked for again on the same track. */
            if (kept.count (their.id) > 0 && run->kind == "media" && their.launched && run->launchedAtSample > 0)
            {
                const auto wantedAt = their.position + secondsOf (since) * std::abs (their.rate);

                if (std::abs (run->position - wantedAt) > secondsOf (pair::playheadToleranceTicks)
                      && seekMedia (engine, tick, run->id, their.position + secondsOf (lead) * std::abs (their.rate), true))
                {
                    run->launchNotBefore = launchSample;
                    run->launchRequestedAtTick = tick + seatLeadTicks;
                }
            }

            /*  A CODE THAT STOOD (TC.7), out by as much: moved to the frame the
                leader's will have reached when this one is placed again, on the
                same sample a sound sought here launches on. */
            if (kept.count (their.id) > 0 && run->kind == "timecode" && their.startedTick != -1 && run->startedAtTick >= 0)
            {
                const auto wantedAt = their.position + secondsOf (since);

                if (std::abs (run->position - wantedAt) > secondsOf (pair::playheadToleranceTicks)
                      && seekTimecode (run->id, their.position + secondsOf (lead)))
                    run->launchNotBefore = launchSample;
            }
        }

        //----------------------------------------------------------------------
        /*  THE JOBS, AS THE LEADER'S ARE: each scene's place among its members
            in place of what `seatPlan` worked out - a scene in its header or
            its footer, a sequence awaiting its third member, a round drawn -
            and every fade as far on as the leader's will be. */
        for (const auto& job : at.groups)
        {
            if (runs.find (job.run) == nullptr)
                continue;

            GroupJob* mine = nullptr;

            for (auto& candidate : scheduled)
                if (! candidate.retired && candidate.run == job.run)
                {
                    if (mine != nullptr)
                        mine->retired = true;

                    mine = &candidate;
                }

            if (mine == nullptr)
            {
                scheduled.push_back (job);
                continue;
            }

            *mine = job;
            mine->retired = false;
        }

        running.clear();

        for (auto job : at.fades)
        {
            const auto held = job.heldRun();

            if ((! held.empty() && runs.find (held) == nullptr)
                  || (job.reportsSelf && ! job.self.empty() && runs.find (job.self) == nullptr))
                continue;

            job.ticksDone = std::min (job.ticksTotal, job.ticksDone + static_cast<int> (since));

            if (! job.dca.empty() && dcas != nullptr)
                job.handSerial = dcas->handSerialOf (job.dca);

            running.push_back (std::move (job));
        }

        //----------------------------------------------------------------------
        //  TONIGHT'S NUMBERS THAT ARE NOT THE SHOW'S.
        if (dcas != nullptr)
        {
            dcas->clear();

            for (const auto& [dca, trim] : at.dcas)
                dcas->set (dca, trim);
        }

        /*  THE CODES' MAPS, THE LEADER'S (TG.5), their samples on this
            machine's clock already - the follower moved them with the ticks -
            in place of whatever this machine had: its own watchers' turns are
            the gate's while it follows, and the leader's from here on arrive as
            records. */
        codesHeard = at.codes;

        //  THE FIELDS, THE LEADER'S (61), in place of this machine's - a text cue up is drawn again.
        if (textFields != nullptr)
            textFields->replaceAll (at.fields);

        if (lanes != nullptr)
        {
            lanes->free();

            if (! at.laneCue.empty())
            {
                lanes->flip (at.laneCue);

                for (const auto& key : at.laneArmed)
                    lanes->setArmed (key, true);
            }
        }

        //----------------------------------------------------------------------
        /*  AND THE NAMES (AGE): the leader's salt and counts, less what its
            horizon made, which this machine's horizon now makes again under
            the same names. Last, since every run made above was counted. */
        namer.seat (at.names);

        /*  A SOUND THIS MACHINE CANNOT PLAY, said: its run is made as the
            leader's and fails here, `media-missing`, as `armMedia` judges it -
            the run is the leader's, and the summary says it is not playing. */
        if (audio != nullptr)
            for (const auto& their : at.runs)
                if (const auto* run = runs.find (their.id); run != nullptr && run->kind == "media" && ! their.isFinished())
                    if (run->media.empty()
                          || (! mediaFolder.empty() && ! juce::File (juce::String (mediaPathOf (run->media))).existsAsFile()))
                        note ("cue " + their.cue + " has no sound to play here");
    }
}
