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
#include <wfg/engine/cue/Solver.h>

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

                        /*  AND IT STOPS BEING A PREPARATION. `prepare` answers
                            a question about the GO that has not happened, and
                            this is that GO - whether it goes straight to a
                            launch or into a pre-wait first. */
                        armed->prepare.clear();
                    }

                return live->id;
            }

        /*  AND A GROUP THE HORIZON PREPARED IS ADOPTED, not made a second time.

            This is the other door: `fireStandby`'s descent adopts every group
            BETWEEN the pointer and the list, and the pointer's own cue arrives
            here. Without it, GO on a prepared scene would start a second run of
            it beside the one holding the voices - one scene, two schedulers,
            every member launched twice.

            No `enterAt`: the pointer was ON the group, so it enters at its
            first member, which is what an empty one means. */
        if (fireAtOnce && kind == "group")
            if (const auto* ready = runs.preparedRunOf (cueId))
            {
                const auto adopted = ready->id;
                adoptPrepared (adopted, {}, true);
                return adopted;
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
            /*  THE SMALLEST HORIZON THERE IS, and it has been here since PR 2.3
                without a word for itself: a media cue at standby is armed, and
                `armed` is exactly what §13.6's vocabulary calls a preparation
                with nothing to verify. Saying it on the row costs one
                assignment and is the difference between an operator seeing that
                the next cue is ready and having to know that it always is. */
            run->prepare = preparedness::armed;
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

    std::vector<juce::ValueTree> Runner::descentTo (const juce::ValueTree& list,
                                                   const std::string& cueId) const
    {
        /*  Walking up and then reversing, because the document knows parents
            and not paths.

            STRICT ANCESTORS, so a group at the pointer is not in its own
            descent: `fireStandby` fires that one itself, and the horizon adds
            it deliberately. Two callers, one shape - which matters, because
            adoption is one of them recognising the other's work, and a descent
            they disagreed about would be a scene created twice. */
        std::vector<juce::ValueTree> ancestors;

        for (auto node = document.findById (cueId).getParent();
             node.isValid() && node != list;
             node = node.getParent())
        {
            if (node.getType().toString() == "Group")
                ancestors.push_back (node);
        }

        std::reverse (ancestors.begin(), ancestors.end());
        return ancestors;
    }

    std::string Runner::horizonRootFor (const juce::ValueTree& list,
                                        const std::string& cueId) const
    {
        const auto chain = descentTo (list, cueId);

        if (! chain.empty())
            return chain.front()[idProperty].toString().toStdString();

        const auto cue = document.findById (cueId);

        return cue.isValid() && cue.getType().toString() == "Group" ? cueId : std::string {};
    }

    void Runner::revokePrepared (Engine& engine, std::int64_t tick, const std::string& runId)
    {
        juce::ignoreUnused (engine);

        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished())
            return;

        /*  Copied first, because finishing a child can change what the table
            answers about the parent's children while the walk is inside it. */
        std::vector<std::string> children;

        for (const auto* child : runs.childrenOf (runId))
            children.push_back (child->id);

        for (const auto& child : children)
            revokePrepared (engine, tick, child);

        run = runs.find (runId);

        if (run == nullptr)
            return;

        /*  DONE AND NOT FAILED. Nothing went wrong: a scene was got ready and
            then not wanted, which is exactly what anticipation is allowed to
            cost. `warning` is where the reason goes, beside `no-channel`, and
            for the same kind of reason - a run that did something smaller than
            it meant to and is not an error. */
        run->state = runState::done;
        run->endedAtTick = tick;
        run->warning = runWarning::revoked;
        run->prepare.clear();

        /*  AND THE VOICE AND THE SLOTS COME BACK. `holdsTrack()` is a track and
            an unfinished run, so the state above is the whole of freeing the
            voice; the slots go back to the head of each queue, exactly as they
            do at any other ending (§13.4). */
        runs.releaseSlotsOf (runId);

        for (auto& job : scheduled)
            if (job.run == runId)
                job.retired = true;
    }

    //==============================================================================
    bool Runner::isPreparable (const juce::ValueTree& cue) const
    {
        /*  PER PARAMETER AND NEVER PER CUE (§3.12). */
        const auto element = cue.getType().toString();

        /*  A MEDIA CUE ALWAYS, because an arm is revocable by construction:
            a voice reserved and a file made ready are undone by letting go of
            them, and nothing outside this machine heard anything. */
        if (element == "Media")
            return true;

        if (element != "Osc")
            return false;

        /*  AN OSC CUE ONLY WHERE THERE IS SOMETHING TO PUT BACK.

            §13.1, and it is a condition rather than a preference: a value
            pre-sent to a target that cannot be asked what it held is a value
            nobody can restore, so anticipating it would trade a saved moment
            for a desk left in a state the operator did not choose. Both halves
            are required - the node marked `anticipatable`, which is what its
            owner says about whether an early write is safe, AND the mount able
            to answer, which is what makes the restore exist at all.

            A node that is one without the other is a `wfg validate` warning
            rather than a silent decision here: the show still runs, and the cue
            fires at entry like any other. */
        if (mounts == nullptr)
            return false;

        const auto address = textOf (cue, "address");

        if (address.empty())
            return false;

        const auto* node = mounts->nodeAt (address);

        if (node == nullptr || ! node->anticipatable)
            return false;

        const auto* declaration = mounts->declarationOf (mounts->mountOf (address));
        return declaration != nullptr && declaration->canBeAsked();
    }

    void Runner::collectPresetsOf (const juce::ValueTree& node, const std::string& groupId,
                                   std::vector<std::string>& out) const
    {
        for (const auto& child : node)
        {
            if (! child.hasProperty (idProperty))
                continue;

            const auto element = child.getType().toString().toStdString();

            if (element == "Header" || element == "Footer")
            {
                collectPresetsOf (child, groupId, out);
                continue;
            }

            if (doc::ShowDocument::ownerForElement (element) != "cue")
                continue;

            if (child[juce::Identifier ("preset")].toString().toStdString() == groupId)
                out.push_back (child[idProperty].toString().toStdString());

            collectPresetsOf (child, groupId, out);
        }
    }

    std::vector<std::string> Runner::preparableIn (const juce::ValueTree& group) const
    {
        std::vector<std::string> out;

        /*  THE DERIVED LINES FIRST, WHICH IS §13.7's ORDER AND HAS A REASON.

            A written header cue may reasonably depend on what the presets set -
            position the source, then move it - and the reverse dependency has
            no natural example. A header is a sequence whatever the group's mode
            says, and §3.12 puts prepare and commit there precisely because
            preparation has an order. */
        std::vector<std::string> derived;
        collectPresetsOf (group, group[idProperty].toString().toStdString(), derived);

        for (const auto& cueId : derived)
            if (const auto cue = document.findById (cueId); isPreparable (cue))
                out.push_back (cueId);

        for (const auto& cueId : membersOf (group.getChildWithName ("Header")))
        {
            /*  A cue that is both a written header cue AND marked for this
                group's header is one cue, not two. It happens where somebody
                dragged a header cue onto its own group's header, which is a
                reasonable thing to do by accident and must not spawn the cue
                twice. */
            if (std::find (out.begin(), out.end(), cueId) != out.end())
                continue;

            if (const auto cue = document.findById (cueId); isPreparable (cue))
                out.push_back (cueId);
        }

        return out;
    }

    bool Runner::beginPreparation (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                                   const std::function<std::string()>& drawId,
                                   std::vector<std::string>& used)
    {
        const auto cues = preparableIn (group);

        if (cues.empty())
            return false;

        job.phase = groupPhase::preparing;
        job.phaseCues = cues;
        job.nextMember = cues.size();
        job.launched = 0;
        job.awaiting.clear();
        job.phaseRuns.clear();

        /*  ISSUED IN HEADER ORDER AND NOT WAITED ON ONE AT A TIME, which is the
            one place a preparation is not a sequence and could not be.

            A header phase runs its cues one after another because each reports
            done and the next begins. A PREPARED MEDIA CUE NEVER REPORTS DONE -
            being armed and not launched is the whole of what preparing it means
            - so a chain that waited for the first would wait for ever. What the
            phase waits for instead is that everything it issued has ARRIVED: an
            arm armed, a network cue finished. The order is still the header's,
            and a header whose cues write the same address still leaves the last
            one standing, because the sender coalesces by address inside a tick. */
        for (const auto& cueId : cues)
        {
            const auto made = spawnChild (engine, job.run, cueId, drawId());

            if (made.empty())
            {
                used.pop_back();
                continue;
            }

            used.back() = made;

            if (auto* child = runs.find (made))
                child->prepare = preparedness::armed;
        }

        return true;
    }

    const char* Runner::settledWord (const GroupJob& job, const juce::ValueTree& group) const
    {
        /*  IN THE ORDER AN OPERATOR WOULD WANT TO BE TOLD. A block waiting for
            a slot somebody else holds says so first, because that is the one
            with a cause outside itself; then a block that could not be got
            ready whole; then the ordinary answer. */
        auto missing = false;
        auto agreed = false;

        for (const auto* child : runs.childrenOf (job.run))
        {
            if (! child->pending.empty())
                return preparedness::pending;

            if (child->state == runState::failed)
                missing = true;

            /*  THE DESK AGREED. A network cue whose wait is `verified` and
                whose run reached `done` was pre-sent, asked about, and answered
                with the value that was written - which is the one thing in this
                vocabulary that is a statement about the OTHER box rather than
                about what Go.dot meant to do. */
            if (child->kind == "osc" && child->state == runState::done
                 && textOf (document.findById (child->cue), "wait") == "verified")
                agreed = true;
        }

        /*  A HEADER CUE THIS HORIZON COULD NOT TAKE is the other half of
            `partial`: §3.6's own word for a block that is not anticipatable all
            the way through, and §3.12's reason for deciding preparability per
            parameter rather than per cue. */
        if (missing || preparableIn (group).size()
                         != membersOf (group.getChildWithName ("Header")).size())
            return preparedness::partial;

        return agreed ? preparedness::verified : preparedness::armed;
    }

    bool Runner::preparationSettled (const GroupJob& job) const
    {
        for (const auto& cueId : job.phaseCues)
        {
            const Run* child = nullptr;

            for (const auto* candidate : runs.childrenOf (job.run))
                if (candidate->cue == cueId)
                    child = candidate;

            /*  Not spawned yet: the submit is applied on the next tick. */
            if (child == nullptr)
                return false;

            if (child->kind == "media")
            {
                /*  ARMED IS AS FAR AS A MEDIA PREPARE GOES, and `failed` is as
                    settled as armed: a cue whose file is missing has finished
                    being got ready, badly, and holding the whole block for it
                    would mean one absent sound stopped the scene from ever
                    being prepared. The row says `partial`. */
                if (child->track < 0 && ! child->isFinished())
                    return false;

                continue;
            }

            if (! child->isFinished())
                return false;
        }

        return true;
    }

    void Runner::adoptPrepared (const std::string& runId, const std::string& entersAt,
                                bool enters)
    {
        if (auto* run = runs.find (runId))
        {
            run->enterAt = entersAt;

            if (enters)
                run->state = runState::playing;

            /*  AND IT STOPS BEING A PREPARATION. `prepare` answers a question
                about the future - how ready is this for the GO that has not
                happened - and a scene that is playing has no future left to be
                ready for. */
            run->prepare.clear();
        }

        for (auto& job : scheduled)
        {
            if (job.run != runId || job.retired)
                continue;

            /*  ON BOTH THE RUN AND THE JOB, which is not belt and braces: the
                job took its own copy of `enterAt` when it was created and
                `beginPhase` reads that copy, so writing only the run would
                start the scene at member one wherever the pointer actually was. */
            job.enterAt = entersAt;

            /*  OUT OF THE HOLD AND INTO WHAT IS LEFT, but only for the group
                the press actually enters. `entering` is where a group starts,
                and from it `advanceGroups` tries the header - which now means
                the header's REMAINDER, because `job.prepared` names what the
                horizon already ran. */
            if (enters)
                job.phase = groupPhase::entering;
        }
    }

    bool Runner::inAncestryOf (const std::string& runId, const std::string& ofRun) const
    {
        for (auto at = ofRun; ! at.empty();)
        {
            if (at == runId)
                return true;

            const auto* run = runs.find (at);

            if (run == nullptr)
                return false;

            at = run->parent;
        }

        return false;
    }

    void Runner::askedFor (const std::string& runId)
    {
        if (auto* run = runs.find (runId))
            run->prepare.clear();
    }

    bool Runner::claimedByAJob (const std::string& runId) const
    {
        return std::any_of (scheduled.begin(), scheduled.end(),
                            [&runId] (const GroupJob& job)
                            {
                                return ! job.retired && job.hasTaken (runId);
                            });
    }

    //==============================================================================
    std::vector<std::string> Runner::loadToTime (Engine& engine, doc::ShowDocument& editable,
                                                 std::int64_t tick, const std::string& listId,
                                                 const std::vector<std::string>& supplied)
    {
        std::vector<std::string> used;
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

        const auto list = document.findById (listId);

        if (! list.isValid())
            return used;

        const auto aim = lists.aimOf (listId);

        if (! aim.isSet())
            return used;

        const auto plan = solve (document, durations, mounts,
                                 { listId, aim.cue, aim.offset });

        if (! plan.ok)
            return used;

        /*  WHOSE LIST A RUN BELONGS TO, by climbing its cue to the top. A jump
            is scoped to one list (§13.5's cross-list rule read from the other
            side), so this is what tells the sweep below what it may end. */
        const auto listOf = [this] (const std::string& cueId)
        {
            for (auto node = document.findById (cueId); node.isValid();
                 node = node.getParent())
                if (node.getType().toString() == "List")
                    return node[idProperty].toString().toStdString();

            return std::string {};
        };

        //----------------------------------------------------------------------
        /*  WHAT THE JUMP ABANDONS, ENDED BEFORE ANYTHING IS BUILT.

            Every run of THIS list the plan does not name: the group runs and
            their jobs, the members under them, the armed run at the old
            standby. Ended the way `run.kill` ends one - the whole descent, and
            NO FOOTER, because a footer is arbitrary and need not be an inverse.
            Running one here would be arbitrary work fighting the values this is
            about to send, and one that blocks on a fade would make the jump
            wait for it.

            THE HANDLER DOES IT ITSELF rather than submitting, exactly as a
            revocation does (§13.6): the jump is one record, and a replay
            reaches the same state from the identifiers already in it.

            AND THE CLAIMS COME BACK IN THIS SAME DRAIN, which is why the sweep
            is before the build rather than after: a claim released a tick later
            would leave the plan's runs queued behind runs the jump had already
            ended. */
        std::vector<std::string> wanted;

        for (const auto& wants : plan.runs)
            wanted.push_back (wants.cue);

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.isFinished() || listOf (snapshot.cue) != listId)
                continue;

            if (std::find (wanted.begin(), wanted.end(), snapshot.cue) != wanted.end())
                continue;

            if (auto* run = runs.find (snapshot.id))
            {
                run->state = runState::done;
                run->endedAtTick = tick;
                run->prepare.clear();
                runs.releaseSlotsOf (run->id);

                if (run->track >= 0 && audio != nullptr)
                    audio->stop (run->track);
            }

            for (auto& job : scheduled)
                if (job.run == snapshot.id)
                    job.retired = true;
        }

        //----------------------------------------------------------------------
        /*  THE POINTER, positionally after the target (§3.5).

            Through a WRITABLE document handed in by the command, because the
            Runner reads the show and does not edit it - the same reference the
            `go` handler uses to move standby, for the same reason: the pointer
            is a decision somebody made and lives in the file, while everything
            else this function touches is engine state. */
        editable.setAttribute (standbyAddressOf (listId), plan.standby);

        //----------------------------------------------------------------------
        /*  AND THE TREE, OUTERMOST FIRST.

            `RunTable::create` links a run into its parent's children only if
            the parent already exists, so a group made after its member would
            have a member it never heard of. The plan lists the target's
            ancestors outermost first for exactly this reason. */
        std::map<std::string, std::string> runFor;

        for (const auto& wants : plan.runs)
        {
            const auto cue = document.findById (wants.cue);

            if (! cue.isValid() || cue.getType().toString() != "Group")
                continue;

            const auto id = nextId();
            const auto parent = wants.ancestors.empty()
                                  ? std::string {}
                                  : runFor[wants.ancestors.back()];

            runs.create (id, wants.cue, "group", parent);
            runFor[wants.cue] = id;

            auto* run = runs.find (id);

            if (run == nullptr)
                continue;

            run->state = runState::playing;
            run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
            run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

            /*  TWO OF THESE HAVE TO BE WRITTEN BY HAND AND IT IS NOT OBVIOUS
                WHICH. A run made by `create` leaves `iterations` at one and
                `seed` at nought; `iterations` is otherwise set when a group is
                FIRED and `seed` and `round` when a round is DRAWN, and a jump
                does neither. A group adopted without them ends after one round,
                or draws its next shuffle from a seed the show never used. */
            run->iterations = static_cast<int> (numberOf (cue, "loops"));
            run->seed = static_cast<std::uint64_t> (numberOf (cue, "seed"));
            run->iteration = 0;
            run->round = membersOf (cue);
        }

        //----------------------------------------------------------------------
        /*  THEN THE CUES THAT MAKE A SOUND, each under the group it belongs to. */
        for (const auto& wants : plan.runs)
        {
            const auto cue = document.findById (wants.cue);

            if (! cue.isValid() || cue.getType().toString() == "Group")
                continue;

            const auto id = nextId();
            const auto parent = wants.ancestors.empty()
                                  ? std::string {}
                                  : runFor[wants.ancestors.back()];

            runs.create (id, wants.cue, kindOfCue (cue), parent);
            runFor[wants.cue] = id;

            auto* run = runs.find (id);

            if (run == nullptr)
                continue;

            run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
            run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

            /*  ALREADY OVER. Its run exists and is `done` so that its group
                knows it has been played - a member missing from the finished
                end is a group that thinks it has not started. */
            if (wants.when == planned::finished)
            {
                run->state = runState::done;
                run->endedAtTick = tick;
                continue;
            }

            /*  STILL TO COME, at its remaining offset. A timeline schedules
                everything at entry, so the members after the aim are waiting
                with a due tick rather than absent - absent, the group would
                spawn them a second time. */
            if (wants.when == planned::due)
            {
                run->state = runState::waiting;
                run->dueTick = tick + ticksFor (wants.startsIn);
                continue;
            }

            /*  AND THE ONES MAKING A NOISE. Armed with their launch asked for
                and their offset on them: the arm applies it as the clip's own
                offset, which M17 measured landing on the sample. */
            run->startOffset = wants.offset;
            run->startRange = std::max (wants.range, 0);
            run->launchRequested = true;
            run->launchRequestedAtTick = tick;

            armMedia (engine, cue, id);
        }

        //----------------------------------------------------------------------
        /*  AND THE JOBS THAT WILL CARRY IT ON.

            The scheduler continues from here on the next tick, because a group
            job re-reads the round from the run every tick rather than keeping a
            copy - PR 3.5's finding, built so a prune could reach the round in
            progress, and paying again here.

            The job's own list of runs it has taken charge of is filled at this
            moment too, because the loop that claims a child on sight does not
            run on a job's first tick. */
        for (const auto& wants : plan.runs)
        {
            const auto found = runFor.find (wants.cue);

            if (found == runFor.end())
                continue;

            const auto cue = document.findById (wants.cue);

            if (! cue.isValid() || cue.getType().toString() != "Group")
                continue;

            GroupJob job;
            job.run = found->second;
            job.phase = groupPhase::members;
            job.phaseCues = membersOf (cue);
            job.nextMember = job.phaseCues.size();

            for (const auto* child : runs.childrenOf (job.run))
            {
                job.taken.push_back (child->id);
                job.phaseRuns.push_back (child->id);

                if (! child->isFinished() && child->state != runState::waiting)
                    job.awaiting = child->id;
            }

            job.launched = job.phaseRuns.size();

            /*  A SEQUENCE ADVANCES ON THE MEMBER IT IS WAITING FOR, so
                `nextMember` is where the plan left off rather than the end of
                the list - otherwise the chain would stop at the jump. */
            if (textOf (cue, "mode") != "timeline")
            {
                const auto* awaited = runs.find (job.awaiting);
                const auto at = awaited != nullptr
                                  ? std::find (job.phaseCues.begin(), job.phaseCues.end(),
                                               awaited->cue)
                                  : job.phaseCues.end();

                job.nextMember = at != job.phaseCues.end()
                                   ? static_cast<std::size_t> (at - job.phaseCues.begin()) + 1
                                   : job.phaseCues.size();
            }

            scheduled.push_back (job);
        }

        //----------------------------------------------------------------------
        /*  AND THE VALUES: A MINIMAL CORRECTION, NOT A SHOTGUN BLAST (§3.13).

            What is compared here is the mounted TREE - what Go.dot last wrote -
            rather than what the desk currently holds, and the difference
            matters: a value somebody moved by hand on the desk is not in this
            comparison and will not be corrected. Reading the desk first is the
            bulk read-back, which is the next PR and which this is deliberately
            not pretending to be.

            Sent through the ordinary write, so a replay reproduces it exactly
            and, having no sender, does not move the rig. */
        for (const auto& value : plan.values)
        {
            if (mounts != nullptr)
                if (const auto* now = mounts->valueOf (value.address);
                    now != nullptr && *now == value.value)
                    continue;

            engine.submit (origin::engine, "node.set",
                           { osc::Value::string (value.address), value.value });
        }

        /*  WHERE IT LANDED, which is §3.13's second pointer. After a jump this
            agrees with the aim; after the next GO it does not, and that
            divergence is what a running view shows. */
        lists.landedAt (listId, { aim.cue, aim.offset });

        return used;
    }

    std::vector<std::string> Runner::prepareStandby (Engine& engine, std::int64_t tick,
                                                     const juce::ValueTree& list,
                                                     const std::string& cueId,
                                                     const std::vector<std::string>& supplied)
    {
        juce::ignoreUnused (tick);

        std::vector<std::string> used;
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

        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return used;

        auto chain = descentTo (list, cueId);

        /*  THE POINTER'S OWN CUE IS IN THE HORIZON when it is a group, which is
            the difference between this and a GO's descent: GO fires that group
            itself, and there is nothing here to fire. */
        if (cue.getType().toString() == "Group")
            chain.push_back (cue);

        if (chain.empty())
            return used;

        std::string parentRun;

        for (const auto& group : chain)
        {
            const auto groupId = group[idProperty].toString().toStdString();

            /*  ALREADY RUNNING, or already prepared by an earlier tick: leave
                it and descend through it. A horizon that rebuilt what it had
                built a moment ago would re-arm every voice in the block every
                time the pointer twitched. */
            if (const auto* live = runs.liveRunOf (groupId))
            {
                parentRun = live->id;
                continue;
            }

            if (const auto* ready = runs.preparedRunOf (groupId))
            {
                parentRun = ready->id;
                continue;
            }

            const auto id = nextId();

            runs.create (id, groupId, "group", parentRun);

            auto* run = runs.find (id);

            if (run == nullptr)
                continue;

            /*  NOT LIVE, AND SAYING SO. Everything that asks whether a cue is
                running goes through `liveRunOf`, which skips this state - so a
                stop aimed at a scene nobody has entered is §3.8's silent no-op,
                and the pointer at the end of a manual group leaves rather than
                wrapping into a second round of a group nobody started. */
            run->state = runState::preparing;
            run->preWaitTicks = ticksFor (numberOf (group, "preWait"));
            run->postWaitTicks = ticksFor (numberOf (group, "postWait"));
            run->iterations = static_cast<int> (numberOf (group, "loops"));

            GroupJob job;
            job.run = id;

            /*  A GROUP WITH NOTHING PREPARABLE GOES STRAIGHT TO THE HOLD, which
                is a complete answer rather than a failure: a scene whose header
                is one memo has nothing to do ahead, and the run still exists so
                that GO has something to adopt and the pointer moving away has
                something to revoke. */
            const auto started = beginPreparation (engine, job, group, nextId, used);

            if (! started)
                job.phase = groupPhase::prepared;

            /*  ASKED FOR AGAIN, because `beginPreparation` CREATES RUNS and the
                run table is a vector: every pointer into it taken before that
                call is a pointer into memory the growth may have moved.

                It cost an hour to find, and the shape of the mistake is worth
                the sentence: the media case never showed it, because a scene
                whose header holds nothing preparable creates no children at
                all - so the pointer stayed valid and every test passed. It took
                a header with a network cue in it, which is the first thing this
                function ever creates a run for. */
            run = runs.find (id);

            if (run == nullptr)
                continue;

            /*  NOTHING PREPARABLE IS NOT NOTHING PREPARED. The block's members
                are still armed below, so a scene whose header is one memo reads
                `armed` and not `idle` - and `partial` when the header had cues
                this horizon could not take ahead. */
            run->prepare = started
                             ? preparedness::preparing
                             : (membersOf (group.getChildWithName ("Header")).empty()
                                  ? preparedness::armed
                                  : preparedness::partial);

            scheduled.push_back (job);
            parentRun = id;
        }

        /*  AND WHAT THE INNERMOST WOULD LAUNCH FIRST, armed underneath it.

            `armablesFor` is the same lookahead standby has done since PR 3.13,
            asked of the POINTER'S OWN CUE: the first member of a sequence, the
            offset-nought members of a timeline, recursively. Of the pointer's
            cue and not of the outermost group, because the pointer sitting on
            member three is a statement about member three - asking the group
            would arm member one and hold a voice for a cue nobody is about to
            fire. What is new is the PARENT. Armed parentless, as it was, a nested member would be
            adopted by whichever group got to it; armed under the prepared run
            it is inside the block, so a revocation reaches it by following
            `children` and never has to go looking. */
        if (parentRun.empty())
            return used;

        for (const auto& id : armablesFor (cue))
        {
            if (runs.hasChildFor (parentRun, id))
                continue;

            /*  AN ARM THE POINTER ALREADY MADE IS ADOPTED AND DRAWS NOTHING.

                The pointer moving from a top-level media cue into a scene left
                a parentless armed run behind; taking it into the block is what
                PR 3.13 called an arm that buys something. Nothing is CREATED
                there, so nothing is drawn and the record carries nothing for
                it - which is exactly right, because a replay reaches the same
                run by the same road: the `audio.arm` that made it is in the log
                above this record. */
            const auto* standing = runs.liveRunOf (id);

            const auto adoptable = standing != nullptr
                                     && standing->parent.empty()
                                     && standing->state == runState::armed
                                     && ! standing->launchRequested
                                     && ! claimedByAJob (standing->id);

            if (standing != nullptr && ! adoptable)
                continue;

            const auto made = spawnChild (engine, parentRun, id,
                                          adoptable ? std::string {} : nextId());

            /*  MARKED AS A PROMISE. A phase takes charge of the children of its
                own cues and launches them, and this is a child of exactly that
                shape sitting under a group the pointer is merely passing
                through - so without the mark a manual scene would start the
                member the operator was reading about. `askedFor` is what takes
                it off, at the moment somebody presses something. */
            if (auto* child = runs.find (made))
                child->prepare = preparedness::armed;

            if (adoptable)
                continue;

            if (made.empty())
                used.pop_back();
            else
                used.back() = made;
        }

        return used;
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
            every one of them has to be live before the member can be its child. */
        const auto ancestors = descentTo (list, cueId);

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

            /*  THE HORIZON ALREADY MADE IT, AND GO TAKES IT.

                §13.6. The adoption is written here rather than in `spawnChild`
                because `fireStandby` always generates an identifier and so
                never reaches that function's parentless-arm branch at all. What
                adoption is, exactly: the run stops being a promise and becomes
                a scene - `playing`, like any group that has just been fired -
                and it is told where the pointer entered.

                ON BOTH THE RUN AND THE JOB, which is not belt and braces: the
                job took its own copy of `enterAt` when it was created, and
                `beginPhase` reads that copy. Writing only the run would start
                the scene at member one, wherever the operator's pointer
                actually was. */
            if (const auto* ready = runs.preparedRunOf (groupId))
            {
                const auto adopted = ready->id;

                adoptPrepared (adopted,
                               level + 1 < ancestors.size()
                                 ? ancestors[level + 1][idProperty].toString().toStdString()
                                 : cueId,
                               parentRun.empty());

                parentRun = adopted;
                createdGroup = true;
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

        /*  THE OPERATOR REACHED A MEMBER THE HORIZON HAD ARMED, and this is
            what turns the promise into a performance.

            The run is already there, under this group, holding a voice with its
            file ready - which is the whole point of arming ahead. What it was
            missing is somebody asking for it, and the press is that. Clearing
            the mark is the ask; the group's job launches it on the next tick,
            as it launches every member, so there is still one launcher and not
            two. */
        if (const auto* ahead = runs.liveRunOf (cueId))
            if (ahead->state == runState::armed && ! ahead->prepare.empty())
            {
                askedFor (ahead->id);
                return used;
            }

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
            for (auto& other : scheduled)
            {
                if (other.run != runId || other.retired)
                    continue;

                /*  THE THIRD ADOPTION DOOR. A prepared group nested inside
                    another is left standing by the press that adopted it and is
                    launched here, by its parent's job, at the moment §3.6 puts
                    it. Its job already exists - the horizon made it - so what
                    this is is the hold being let go of, not a job being
                    created.

                    A group that is merely already running falls through and
                    changes nothing, which is what the guard was written for:
                    a run scheduled twice would have two jobs spawning the same
                    members, launching them twice and ending them twice under
                    one identifier. */
                if (other.phase == groupPhase::preparing
                     || other.phase == groupPhase::prepared)
                {
                    other.enterAt = run->enterAt;
                    other.phase = groupPhase::entering;
                }

                return;
            }

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
        run->prepare.clear();

        /*  Armed already if it came through a pre-wait or through standby; this
            is the arm for a cue fired from cold, which is the case that pays
            the disk. */
        if (run->track < 0)
            armMedia (engine, cue, runId);
    }


    void Runner::claimSlotsFor (const juce::ValueTree& cue, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr)
            return;

        for (const auto& destination : cue)
        {
            const auto element = destination.getType().toString();
            const auto isFeed = element == "Feed";
            const auto isInsert = element == "Insert";

            if (! isFeed && ! isInsert)
                continue;

            const auto slotId = destination[juce::Identifier (isFeed ? "slot" : "channel")]
                                  .toString().toStdString();
            const auto slot = document.findById (slotId);

            /*  A destination naming a slot the show does not have is the
                `refers` column's warning at validate time and nothing here: it
                will fail the routing when the arm reaches it, with a reason. */
            if (! slot.isValid())
                continue;

            /*  A SHARED RACK CHANNEL IS NEVER CLAIMED. §3.9e: a reverb many
                cues send into is a bus with a chain, and a bus is shared by
                construction with nothing to allocate. */
            if (isInsert
                  && slot[juce::Identifier ("access")].toString() == "shared")
                continue;

            const auto* holder = runs.holderOf (slotId);

            if (holder == nullptr)
            {
                run->claims.push_back (slotId);
                continue;
            }

            /*  BUSY, AND THE TWO KINDS ANSWER DIFFERENTLY - which is §3.9e's
                whole table: a slot has a failure policy of its own kind.

                A PROCESSOR INPUT WAITS. The claim lands when the holder's run
                ends and the row says *pending* meanwhile; GO has already
                returned (§4.1), and a holder that never ends on its own is the
                operator's to end.

                A RACK CHANNEL DEGRADES. The cue plays dry and says so, because
                a scene that stops because a reverb was busy is worse than a
                scene that is dry - and §3.9c's edit-time analysis is what keeps
                it from happening in the show at all. */
            if (isInsert)
            {
                run->warning = runWarning::noChannel;
                continue;
            }

            run->pending.push_back (slotId);
        }
    }

    void Runner::armMedia (Engine& engine, const juce::ValueTree& cue, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->track >= 0)
            return;

        /*  THE SLOTS FIRST, and above the return below, because a claim is a
            fact about the document rather than about the audio side: the cue's
            `Feed` and `Insert` children against the pool the show declared. A
            replay takes them exactly as a hosted session does, which is what
            makes the whole lifecycle reproducible without a record of its own
            (§13.4). */
        claimSlotsFor (cue, runId);

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

        /*  READ THE SAME WAY THE LEVEL IS, and the reason it is worth a line of
            its own: this row has existed since Phase 2, the grammar has always
            accepted it, `validate()` has always refused it beside a range - and
            nothing has ever read it, so a show that asked to start two seconds
            in has always started at the top. Found by auditing §13's claims
            against the code rather than by anybody hearing it. */
        /*  WHERE SOMEBODY JUMPED TO WINS OVER WHERE THE CUE SAYS IT STARTS,
            and only because a jump is the more recent statement about this
            particular run. The document's `startOffset` is a decision about
            every performance; the run's is a decision about this rehearsal, and
            §4.10 keeps them in different places for exactly that reason. */
        request.startOffset = run->startOffset > 0.0 ? run->startOffset
                                                     : numberOf (cue, "startOffset");

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

        /*  ONE PIECE OF ARITHMETIC, TWO KINDS OF DESTINATION.

            A `Route` sends the cue to a bus. A `Feed` sends it to a processor
            input, which means to that slot's own channels OF the slot's bus -
            the same coefficients landing at one more offset (§13.3). Written
            once so the two cannot come to disagree about what a gains list
            means, and so `bad-route` keeps one meaning for both. */
        const auto emit = [&] (int firstChannel, int width,
                               const std::vector<double>& gains) -> bool
        {
            if (width <= 0)
            {
                problem = "a destination of no width";
                return false;
            }

            /*  A cue routed nowhere yet is an ordinary state for a show being
                written, and it is silent rather than wrong. */
            if (gains.empty())
                return true;

            /*  THE SHAPE IS THE CHECK. A gains list is the cue's channels times
                the destination's width, row by row, so a length that does not
                divide by the width is not a shorter routing - it is a different
                one, and a client that wrote it meant something the show cannot
                do. `validate()` refuses the same shape when the show loads;
                this is the same question asked of a file that is finally
                open. */
            if (gains.size() % static_cast<std::size_t> (width) != 0)
            {
                problem = "gains do not divide by the destination width";
                return false;
            }

            const auto inputs = static_cast<int> (gains.size()) / width;

            if (inputs > trackChannels)
            {
                problem = "the cue is wider than a track";
                return false;
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

            return true;
        };

        /*  The bus a destination lands in, or an invalid tree. A destination
            naming a bus the show does not have is a routing the rig cannot
            honour: it fails the RUN rather than the load, because a show with
            one mis-pointed cue is still a show somebody has to run tonight. */
        const auto busNamed = [&] (const std::string& busId)
        {
            const auto bus = document.findById (busId);

            if (! bus.isValid() || bus.getType().toString() != "Bus"
                  || bus.getParent() != audioNode)
                return juce::ValueTree {};

            return bus;
        };

        for (const auto& destination : cue)
        {
            const auto element = destination.getType().toString();

            if (element == "Route")
            {
                const auto bus = busNamed (destination[juce::Identifier ("bus")]
                                             .toString().toStdString());

                if (! bus.isValid())
                {
                    problem = "route names no bus of this show";
                    return {};
                }

                if (! emit (static_cast<int> (bus[juce::Identifier ("firstChannel")]),
                            static_cast<int> (bus[juce::Identifier ("width")]),
                            gainsOf (destination)))
                    return {};

                continue;
            }

            if (element == "Feed")
            {
                /*  A FEED IS A ROUTING AND A CLAIM IN ONE OBJECT, and this is
                    the routing half: the audio reaches the processor through an
                    ordinary bus, and the claim that keeps a second cue out of
                    the position and the LFO state behind that input is the same
                    object carrying it there (§13.3). Two separate objects would
                    let a show route a cue somewhere it had not claimed. */
                const auto slot = document.findById (destination[juce::Identifier ("slot")]
                                                       .toString().toStdString());

                if (! slot.isValid() || slot.getType().toString() != "Slot")
                {
                    problem = "feed names no slot of this show";
                    return {};
                }

                const auto bus = busNamed (slot[juce::Identifier ("bus")].toString().toStdString());

                if (! bus.isValid())
                {
                    problem = "the slot this feed names has no bus of this show";
                    return {};
                }

                const auto slotFirst = static_cast<int> (slot[juce::Identifier ("firstChannel")]);
                const auto slotWidth = static_cast<int> (slot[juce::Identifier ("width")]);
                const auto busFirst = static_cast<int> (bus[juce::Identifier ("firstChannel")]);
                const auto busWidth = static_cast<int> (bus[juce::Identifier ("width")]);

                /*  Checked at load too, and again here for the reason every
                    arm-time check exists: this is the moment the thing is
                    actually used, and a document edited since it was read is a
                    document nobody validated. */
                if (slotFirst < 0 || slotWidth <= 0 || slotFirst + slotWidth > busWidth)
                {
                    problem = "the slot does not fit in its bus";
                    return {};
                }

                if (! emit (busFirst + slotFirst, slotWidth, gainsOf (destination)))
                    return {};

                continue;
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
        /*  A POINTER AT NOTHING IS NOT THE SAME AS A CUE THAT IS NOT RUNNING,
            and telling the two apart is what the `refers` column bought.

            §3.8 makes a fade aimed at a cue that has finished a silent no-op:
            the cue was real and it ended, which happens. A fade aimed at an
            identifier this show does not contain is a different thing - it
            names nothing, and it will name nothing on every GO for the rest of
            the run. That is a `bad-target`, said out loud, and `wfg validate`
            has already said it once on a laptop with nothing plugged in. */
        if (! targetCueId.empty() && ! document.findById (targetCueId).isValid())
        {
            FadeJob orphan;
            orphan.self = self;
            orphan.failure = runError::badTarget;
            running.push_back (orphan);
            return;
        }

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

        job.address = address;
        job.pending = *value;

        const auto seconds = numberOf (cue, "timeout");
        job.ticksAllowed = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));

        /*  A PREPARED CUE ASKS BEFORE IT WRITES, and that order is the whole of
            §13.1 made operational.

            The mark is the run's own `prepare`: a horizon set it, and it means
            "this is ready for a GO that has not happened". A cue in that
            position must be undoable, and undoing a write needs the value that
            was there first - so the target is asked, the answer is kept on the
            run as the restore value, and only then does the write go out.

            The ordinary path does the OPPOSITE and is right to: it forgets the
            remembered answer at the moment it writes and asks afterwards,
            because what it wants to know is whether the device took what it was
            given. Same two operations, opposite order, different question. */
        if (selfRun->prepare.empty())
        {
            writeOscNow (job);
            sending.push_back (job);
            return;
        }

        job.reading = true;
        job.mountId = mounts->mountOf (address);

        if (const auto* node = mounts->nodeAt (address))
            job.typeTag = node->typeTags;

        if (const auto* declaration = mounts->declarationOf (job.mountId))
        {
            job.host = declaration->host;
            job.queryPort = declaration->queryPort;
        }

        /*  FORGOTTEN BEFORE IT IS ASKED FOR, exactly as the verify does and for
            the same reason one step earlier: an answer left over from an
            earlier cue on this node would be taken for what the target holds
            NOW, and the restore would put back a value from a different moment. */
        mounts->forgetReadback (address);

        sending.push_back (job);
    }

    void Runner::writeOscNow (OscJob& job)
    {
        if (mounts == nullptr)
            return;

        const auto written = mounts->write (job.address, job.pending);

        if (! written.ok)
        {
            job.failure = written.reason;
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
                                         { declaration->host, declaration->port,
                                           declaration->rateCap },
                                         job.address, written.value);

        if (job.wait == OscWait::verified)
        {
            job.mountId = written.mountId;
            job.expected = written.value;

            /*  FORGOTTEN BEFORE IT IS ASKED FOR, and this line is the whole
                difference between verifying and appearing to. Without it an
                answer the target gave to an earlier cue would still be sitting
                there, would match, and every verified cue on that node would
                report done without anybody being asked anything. */
            mounts->forgetReadback (job.address);

            if (const auto* node = mounts->nodeAt (job.address))
                job.typeTag = node->typeTags;

            if (declaration != nullptr)
            {
                job.host = declaration->host;
                job.queryPort = declaration->queryPort;
            }
        }
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

            /*  STILL ASKING WHAT WAS THERE FIRST. §13.1: a pre-send that
                cannot be put back is not anticipation, it is a change nobody
                asked for made early - so the read comes before the write and
                the write waits for it.

                What arrives is a `mount.readback` command applied like any
                other, which is what makes this replayable: the answer is in the
                log, and a replay reaches the same restore value on the same
                tick with no network in the room. */
            if (job.reading)
            {
                ++job.ticksWaited;

                if (const auto* held = mounts != nullptr
                                         ? mounts->readbackOf (job.address) : nullptr)
                {
                    /*  KEPT ON THE RUN, because the job will be gone long
                        before the pointer moves away and the restore is needed. */
                    if (auto* run = runs.find (job.self))
                    {
                        run->restoreAddress = job.address;
                        run->restoreAtom = held->toAtom();
                    }

                    job.reading = false;
                    job.ticksWaited = 0;
                    writeOscNow (job);

                    if (! job.failure.empty())
                        continue;

                    /*  A `none` or `sent` wait now behaves as it always did,
                        one tick later than an unprepared cue - which is the
                        price of being able to undo it. */
                    if (job.wait != OscWait::verified)
                    {
                        engine.submit (origin::engine, "run.ended", one (job.self));
                        job.finished = true;
                    }

                    continue;
                }

                /*  THE TARGET NEVER ANSWERED, so there is nothing to restore to
                    and nothing is written: the cue fails here rather than
                    pre-sending a value it could not take back. It is left for
                    entry like any other un-anticipatable cue, and the block
                    reads `partial`. */
                if (job.ticksWaited > job.ticksAllowed)
                {
                    engine.submit (origin::engine, "run.failed",
                                   { osc::Value::string (job.self),
                                     osc::Value::string (oscError::timeout) });
                    job.finished = true;
                    continue;
                }

                if (asker != nullptr && ! job.asked)
                {
                    job.asked = true;
                    asker->ask ({ job.mountId, job.host, job.queryPort,
                                  job.address, job.typeTag });
                }

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

            /*  STILL WAITING FOR A FLUSH THAT WILL TAKE IT, which a rate cap
                makes an ordinary thing rather than a wiring fault: the message
                is queued, in order, holding the newest value for its address,
                and it will go. What the cue asked for was that the value reach
                the target, so it keeps waiting - up to its own timeout, which
                is the same patience a `verified` cue has. */
            if (outcome == tree::MountSender::Outcome::pending
                 && sender_->stillQueued (job.ticket))
            {
                ++job.ticksWaited;

                if (job.ticksWaited <= job.ticksAllowed)
                    continue;

                engine.submit (origin::engine, "run.failed",
                               { osc::Value::string (job.self),
                                 osc::Value::string (oscError::timeout) });
                job.finished = true;
                continue;
            }

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
            /*  A POINTER AT NOTHING, said out loud on the first tick the job
                lives. Reported from here rather than from `beginFade` for the
                reason every report is: only the tick hook may submit, because
                a handler that did would produce the record twice on replay. */
            if (! job.failure.empty())
            {
                engine.submit (origin::engine, "run.failed",
                               { osc::Value::string (job.self),
                                 osc::Value::string (job.failure) });
                job.retired = true;
                continue;
            }

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

        /*  A RUN THE POINTER ALREADY ARMED IS ADOPTED, not made a second time.

            Standby on a group arms what the group would launch first, which is
            the whole reason arming ahead exists: the disk is paid while the
            operator reads the next line. That arm creates a run with no parent.
            If the group then SPAWNED a second run for the same cue, the arm
            would have bought nothing - the first run would sit holding a voice
            nobody was going to launch, the second would pay the disk again with
            the operator's hand already down, and a client watching /godot/run
            would see the same cue twice.

            So the group takes the one that is there. It is adopted by id, which
            is what keeps it replayable: the record carries the identifier
            either way, and a replay re-supplies it. */
        /*  AND IT LOST ITS PARENTLESS HALF, because the horizon gives every
            prepared member a parent.

            The test PR 3.13 wrote was "unfinished, armed, and nobody's child",
            which was exactly right when the only thing that armed ahead was the
            standby pointer and everything it armed was parentless. A horizon
            arms the same runs UNDER the block it is preparing - so the test
            that stopped an arm buying nothing would have stopped adopting the
            very runs this phase exists to prepare, and a timeline's members
            would be spawned a second time, both runs landing in the phase's own
            list and both being launched: one cue, two voices, a tick apart.

            So an adoptable run is one that is unfinished, still `armed`, not
            yet asked to launch, taken charge of by no job, and either
            parentless OR held under a run in the spawning run's own ancestry -
            which is where a prepared member is, whether its own group armed it
            or an ancestor's horizon did. */
        if (id.empty())
            if (const auto* armed = runs.liveRunOf (cueId))
                if (armed->state == runState::armed
                     && ! armed->launchRequested
                     && ! claimedByAJob (armed->id)
                     && (armed->parent.empty() || inAncestryOf (armed->parent, parentRun)))
                    id = armed->id;

        if (id.empty())
            id = ids.generate();

        if (auto* existing = runs.find (id))
        {
            /*  ADOPTION, and the parent is the whole of it: the waits and the
                arm were settled when the run was created, and re-reading them
                here would be reading the document at a different moment from
                the one the run was born at.

                THE OLD PARENT LETS GO, which matters now that there is one. A
                run adopted out of a prepared block that stayed in that block's
                `children` would be killed twice over by a revocation, counted
                twice by "have all my children finished", and shown twice by any
                client drawing the run tree. Until GO it IS under the run that
                prepared it, which is what makes a revocation before GO reach
                it; from GO it is under the group that took it. */
            if (auto* previous = runs.find (existing->parent);
                previous != nullptr && existing->parent != parentRun)
                previous->children.erase (std::remove (previous->children.begin(),
                                                       previous->children.end(), id),
                                          previous->children.end());

            existing->parent = parentRun;

            if (auto* parent = runs.find (parentRun))
                if (std::find (parent->children.begin(), parent->children.end(), id)
                      == parent->children.end())
                    parent->children.push_back (id);

            /*  AND SPAWNING IT IS ASKING FOR IT. A run the horizon armed ahead
                carries `prepare`, which is what stops a phase taking charge of
                a cue nobody has called for; a group spawning it as its own
                member IS that call. Without this a preset member - armed by an
                ancestor's header and adopted here - would sit armed for ever,
                because the phase that adopted it would refuse to launch it. */
            askedFor (id);

            return id;
        }

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

            /*  THE PHASE, MIRRORED FOR SOMEBODY TO LOOK AT, and mirrored here
                rather than published from the job because `ParameterTree` sees
                the run table and never the scheduler's own state.

                A readout and not a decision, so writing it from a hook breaks
                no rule: the same shape as `rangeIteration`, which `advanceRanges`
                computes from the sample counter a few functions down. A replay
                runs no hooks and so leaves it empty, which is what a readout
                does there. */
            run->phase = job.phase;

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

            /*  THE HORIZON AT WORK, and then holding.

                Nothing here is scheduled and nothing is waited on one at a
                time: what the phase issued was issued at once (see
                `beginPreparation`), and what it waits for is that all of it has
                arrived. A media child is armed and left alone - being armed and
                not launched is the whole of what preparing it means - and a
                network child is LAUNCHED, because for an anticipatable node the
                pre-send is the cue's execution and there is nothing else it
                could mean to prepare one. */
            if (job.phase == groupPhase::preparing)
            {
                /*  IT TAKES CHARGE OF NOTHING, and that is deliberate.

                    `taken` is a phase saying "this run is mine", and it is what
                    stops a later phase adopting a run instead of spawning one.
                    A preparation must leave its children ADOPTABLE: the header
                    phase that runs after GO is supposed to find the media cue
                    this armed and launch that very run, and a prepare that had
                    claimed it would make the header spawn a second one beside
                    it - the arm buying nothing, which is the failure PR 3.13
                    was written about.

                    What it remembers instead is `prepared`, by CUE, which is
                    both the "have I launched this one" test here and the list
                    the header's remainder subtracts. */
                for (const auto* child : runs.childrenOf (job.run))
                {
                    /*  A media prepare is an arm and is never launched: being
                        armed and not sounding is the whole of what preparing a
                        media cue means. */
                    if (child->kind == "media")
                        continue;

                    if (std::find (job.prepared.begin(), job.prepared.end(), child->cue)
                          != job.prepared.end())
                        continue;

                    /*  AND A NETWORK CUE'S PREPARE IS ITS EXECUTION: for an
                        anticipatable node the pre-send is the cue, so it is
                        launched here and does not run again at entry. */
                    job.prepared.push_back (child->cue);
                    engine.submit (origin::engine, "run.launch", one (child->id));
                }

                if (preparationSettled (job))
                {
                    job.phase = groupPhase::prepared;
                    run->prepare = settledWord (job, group);
                }

                continue;
            }

            /*  AND THE HOLD, which does nothing at all and has to be written
                down as a phase for exactly that reason: `finishPhase` moves a
                group on to the next phase with anything in it and ends the run
                when none has, and there is no branch in it that waits. A
                prepared group falling through would run its own footer and end
                the scene before anybody pressed GO. */
            if (job.phase == groupPhase::prepared)
                continue;

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
            /*  AND A RUN NOBODY HAS ASKED FOR IS NOT ONE OF THEM.

                A horizon arms what the scene would launch next UNDER the scene,
                so that a revocation reaches it - which puts a run in this list
                that looks exactly like a member the operator called for and is
                not. A manual group taking it would start the member the pointer
                was reading about, with nobody having pressed anything, and the
                whole of §3.6's "the operator is the parent" would be gone.

                `prepare` is the mark, because it already means the thing being
                asked: this is ready for a GO that has not happened. */
            for (const auto* child : runs.childrenOf (job.run))
                if (inPhase (child->cue) && ! job.hasTaken (child->id)
                     && child->prepare.empty())
                {
                    job.taken.push_back (child->id);
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
                const auto& next = job.phaseCues[job.nextMember];

                /*  ALREADY THERE, AND UNCLAIMED - the same test `beginPhase`
                    makes of a phase's FIRST cue, moved onto the path that
                    advances to the next one.

                    It only ever had to cover one cue before: a GO descending
                    into a manual group creates the member the pointer is on,
                    and that member is where the phase starts. A horizon
                    prepares whatever it can reach, which is not always the
                    first thing in a list - so without this a prepared cue
                    standing second in a header gets a second run when its turn
                    comes, and the one that was prepared keeps its voice until
                    the show is reloaded. */
                const auto standing = runs.childrenOf (job.run);

                const auto unclaimed =
                    std::any_of (standing.begin(), standing.end(),
                                 [&job, &next] (const Run* child)
                                 {
                                     return child->cue == next && ! job.hasTaken (child->id);
                                 });

                if (unclaimed)
                {
                    for (const auto* child : standing)
                        if (child->cue == next && ! job.hasTaken (child->id))
                            askedFor (child->id);
                }
                else
                {
                    engine.submit (origin::engine, "run.spawn",
                                   { osc::Value::string (job.run),
                                     osc::Value::string (next) });
                }

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
        auto cues = phase == groupPhase::members
                      ? drawRound (engine, group, job.run)
                      : membersOf (group.getChildWithName (phase == groupPhase::header
                                                              ? "Header" : "Footer"));

        /*  THE HEADER'S REMAINDER, when a horizon ran part of it already. Empty
            in every other case, so this costs a group nobody prepared nothing
            at all. See `GroupJob::prepared`. */
        if (phase == groupPhase::header && ! job.prepared.empty())
            cues.erase (std::remove_if (cues.begin(), cues.end(),
                                        [&job] (const std::string& cueId)
                                        {
                                            return std::find (job.prepared.begin(),
                                                              job.prepared.end(), cueId)
                                                     != job.prepared.end();
                                        }),
                        cues.end());

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

        const auto childRuns = runs.childrenOf (job.run);

        /*  ALREADY THERE AND TAKEN CHARGE OF BY NOBODY: adopt it rather than
            spawn a second beside it.

            Asked of EVERY cue here and not only the first, because a timeline
            spawns its whole round at once - and the horizon arms the
            offset-nought members of a timeline ahead, which is exactly this
            set. Without it one cue gets two runs a tick apart, both in the
            phase's own list, both launched: two voices playing the same file
            slightly out of step, which is the failure PR 3.13's parentless test
            was written to prevent, returning because the parent is no longer
            empty.

            The `unclaimed` word is what makes it survive a loop: a group's
            second round plays the same cues as its first, so "is there a child
            for this cue" answers yes on every round after the first. What is
            being asked is whether something has appeared that no phase has
            taken charge of. */
        const auto unclaimedFor = [&job, &childRuns] (const std::string& cueId)
        {
            return std::any_of (childRuns.begin(), childRuns.end(),
                                [&job, &cueId] (const Run* child)
                                {
                                    return child->cue == cueId && ! job.hasTaken (child->id);
                                });
        };

        /*  A phase beginning a cue is somebody asking for it: whatever the
            horizon armed under this group for that cue stops being a promise
            here. */
        const auto askForChildOf = [this, &job, &childRuns] (const std::string& cueId)
        {
            for (const auto* child : childRuns)
                if (child->cue == cueId && ! job.hasTaken (child->id))
                    askedFor (child->id);
        };

        if (timeline)
        {
            for (const auto& cue : cues)
            {
                if (unclaimedFor (cue))
                    askForChildOf (cue);
                else
                    engine.submit (origin::engine, "run.spawn",
                                   { osc::Value::string (job.run), osc::Value::string (cue) });
            }

            job.nextMember = cues.size();
            return true;
        }

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
        if (unclaimedFor (cues[first]))
        {
            askForChildOf (cues[first]);
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
        /*  A GROUP THE HORIZON IS HOLDING IS NOT A GROUP THAT HAS FINISHED
            ANYTHING. Nothing routes here from the two prepare phases today -
            `advanceGroups` returns before it can - and this says so at the door
            rather than leaving it to a reading of a loop three hundred lines
            long. What it would otherwise do is run the group's footer and end
            the scene before anybody pressed GO. */
        if (job.phase == groupPhase::preparing || job.phase == groupPhase::prepared)
            return;

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
        const auto list = focus.list (document);
        const auto standby = list.isValid()
                               ? list[juce::Identifier ("standby")].toString().toStdString()
                               : std::string {};

        if (standby == armedStandby)
            return;

        armedStandby = standby;

        /*  WHAT THE POINTER LEFT BEHIND.

            A horizon prepares ONE block - §3.12 extends anticipation from a row
            to a block and no further - so anything parentless still in
            `preparing` that is not the block the pointer is in now is a scene
            got ready for a GO that is not coming. Its voices and its slots go
            back.

            Submitted rather than done here, because a hook decides and a
            handler applies: `wfg replay` runs no hooks, so a revocation that
            happened only inside one would be missing from every replay - and
            the replay would then hold voices the session let go of. */
        const auto keep = horizonRootFor (list, standby);

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.state != runState::preparing
                 || ! snapshot.parent.empty()
                 || snapshot.cue == keep)
                continue;

            /*  WHAT WAS PRE-SENT GOES BACK FIRST, and it goes back as an
                ORDINARY WRITE.

                §13.1: anticipation is only as good as its revocation, and a
                revocation of a value on somebody else's desk is putting the old
                one there. `node.set` is how any client writes a mounted node,
                so this is that command with the value the target held before
                the horizon touched it - read before the write, kept on the run.

                An ordinary command rather than a private path, because a replay
                then reproduces the restore exactly as it reproduces every other
                write: the record is in the log with the value in it, and the
                mounted tree comes out the same with no network in the room.

                BEFORE the revocation, so that a client watching sees the desk
                put back and then the runs end, rather than a scene vanishing
                and a value changing afterwards for no visible reason. */
            for (const auto* run : runs.descendantsOf (snapshot.id))
            {
                if (run->restoreAddress.empty())
                    continue;

                if (const auto value = osc::Value::fromAtom (run->restoreAtom))
                    engine.submit (origin::engine, "node.set",
                                   { osc::Value::string (run->restoreAddress), *value });
            }

            engine.submit (origin::engine, "run.revoke", one (snapshot.id));
        }

        if (standby.empty())
            return;

        const auto cue = document.findById (standby);

        if (! cue.isValid())
            return;

        /*  A POINTER INSIDE A SCENE IS A HORIZON, AND IT IS ASKED FOR ABOVE THE
            NULL-PLAYER GATE.

            PRD §3.12 extends anticipation "from one row to a block": the
            pointer landing on a group, or on a member inside one, is the moment
            to get the whole block ready - its headers run outermost first, its
            slots claimed, its values pre-sent - rather than only the one cue
            the pointer is on.

            ABOVE THE GATE BELOW because a preparation is a fact about the
            document: a header of network cues has nothing to do with a sound
            card, and a `wfg serve` without `--hosted` must prepare exactly as a
            hosted session does or the log would not reproduce. Same argument as
            12.1's hooks and as PR 4.3's claims.

            It RETURNS rather than falling through to the arm, because
            `prepareStandby` does that arm itself - under the block, where a
            revocation can find it. */
        if (cue.getType().toString() == "Group" || ! descentTo (list, standby).empty())
        {
            engine.submit (origin::engine, "run.prepare", one (standby));
            return;
        }

        /*  A GROUP AT STANDBY ARMS WHAT IT WOULD LAUNCH FIRST, which is the
            other half of arming ahead and was owed from PR 3.3.

            A pointer on a group is a pointer on a whole scene, and the scene's
            first sound is as much "the next thing" as a media cue would be. Not
            arming it meant GO on a group paid the disk with the operator's hand
            already down - the exact cost arming ahead exists to avoid, moved
            one level of nesting away where it was harder to see. */
        /*  AND HERE IS THE NULL-PLAYER GATE, moved down to where it belongs.

            It used to stand at the head of this function, which meant a session
            with no audio side noticed nothing at all about where the pointer
            was. That was harmless while the only thing this did was ask for a
            voice; it is not harmless now, because a preparation is a fact about
            the DOCUMENT - a claim on a processor input, a header of network
            cues - and a `wfg serve` without `--hosted` has to make it exactly as
            a hosted session does, or the log would not reproduce.

            What genuinely needs a Player is this loop, and only this loop. */
        if (audio == nullptr)
            return;

        for (const auto& id : armablesFor (cue))
        {
            /*  Already running or already armed: nothing to do. `audio.arm`
                would answer with the live run and change nothing, but not
                asking is cheaper and keeps the log about things that
                happened. */
            if (runs.liveRunOf (id) != nullptr)
                continue;

            engine.submit (origin::engine, "audio.arm", one (id));
        }
    }

    std::vector<std::string> Runner::armablesFor (const juce::ValueTree& cue) const
    {
        const auto element = cue.getType().toString();

        /*  ONLY A MEDIA CUE HAS ANYTHING TO MAKE READY. Asking to arm a memo
            would be a rejection every time the pointer passed over one, which
            would fill the log with a refusal about something nobody did wrong. */
        if (element == "Media")
        {
            const auto id = cue[idProperty].toString().toStdString();
            return id.empty() ? std::vector<std::string> {} : std::vector<std::string> { id };
        }

        if (element != "Group")
            return {};

        const auto timeline = textOf (cue, "mode") == "timeline";

        std::vector<std::string> out;

        for (const auto& child : cue)
        {
            if (! child.hasProperty (idProperty))
                continue;

            const auto childElement = child.getType().toString().toStdString();

            /*  A header's cues run before the members and are cues in their own
                right, but they are not what the group LAUNCHES first in the
                sense that matters here - they are memos and messages far more
                often than sound, and a header that did hold a media cue would
                be armed by the scheduler when the header ran. Skipping them
                keeps this about the members. */
            if (doc::ShowDocument::ownerForElement (childElement) != "cue")
                continue;

            /*  A disabled member is not spawned, so arming it would reserve a
                voice for a cue that is never going to play - and a voice held
                by nothing is a cue that fails with `no-track` later. */
            if (child.hasProperty (juce::Identifier ("enabled"))
                  && ! static_cast<bool> (child[juce::Identifier ("enabled")]))
                continue;

            if (! timeline)
            {
                /*  A SEQUENCE LAUNCHES ONE THING, so the first enabled member
                    is the whole answer - and recursively, because that member
                    may be a group of its own. */
                return armablesFor (child);
            }

            /*  A TIMELINE LAUNCHES EVERYTHING AT ENTRY and the pre-waits are
                offsets (§3.6), so what starts at the entry instant is every
                member whose offset is nought. The rest have time to be armed by
                the scheduler while the first ones play, which is what their
                offsets are.

                Reading the wait rather than assuming: a scene whose members all
                start together is one where every one of them is armed ahead,
                and a scene that opens with one is one where one is. */
            if (numberOf (child, "preWait") > 0.0)
                continue;

            for (auto& id : armablesFor (child))
                out.push_back (std::move (id));
        }

        return out;
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
            /*  A PENDING CLAIM HOLDS THE WHOLE CUE, and not the part of it
                that wants the slot.

                A media cue with a `Feed` to a busy processor input and a
                `Route` to foldback launches neither until the claim lands. Half
                a cue is not a cue; sending it into a slot somebody else holds
                is the fighting §3.9b exists to prevent; and the foldback
                arriving alone would be an operator hearing the cue and finding
                it is not the one they fired.

                GO has already returned (§4.1), the row says *pending* (§3.9e),
                and a holder that never ends on its own - an infinite loop - is
                the operator's to end. `run.late` reports the shortfall when it
                does land, with no new arithmetic: the intended launch tick is
                the tick GO was applied on. */
            if (! run->pending.empty())
                continue;

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

            /*  SLOT NOUGHT UNLESS SOMEBODY JUMPED INTO A LATER RANGE.

                A cue with no ranges plays out of the first slot, which is what
                Phase 2 did without having to say so, and a ranged cue entering
                its FIRST range is the same slot - so every run the scheduler
                makes still launches slot nought and nothing about a GO has
                changed. What is new is that a load-to-time can put a run
                anywhere in the playlist (§3.24), and which slot it launches is
                which range that is. */
            const auto slot = run->startRange;

            if (audio->launchAtSample (run->track, slot, target))
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
                    const auto at = std::min (static_cast<std::size_t> (std::max (slot, 0)),
                                              ranges.size() - 1);

                    run->rangeStartedAtSample = target;
                    run->passesWanted = ranges[at].loops;
                    run->passSamples = samplesForRange (ranges[at], audio->sampleRate());
                    run->boundaryPlacedAt = -1;
                    run->rangesFinished = false;

                    engine.submit (origin::engine, "run.range",
                                   { osc::Value::string (run->id),
                                     osc::Value::int32 (static_cast<std::int32_t> (at)) });
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

            /*  A RUN THAT WILL NEVER GIVE AN EDGE, because it never sounded.

                The test below waits for a playing-to-stopped edge, which is the
                right question for every run that played. A run that was ARMED
                and never launched - the pointer reached its cue, the voice was
                reserved and the file made ready - and is then killed gives no
                such edge, ever: it stays `stopping`, and `holdsTrack()` is
                `track >= 0 && ! isFinished()`, so it holds its voice for the
                rest of the session. The sweep in `advanceWaits` does not reach
                it either, because that one deliberately skips anything holding
                a track: a group and a fade hold none, and it was written for
                them.

                So a show whose operator armed eight cues and killed them has
                eight voices gone, and the symptom arrives later and somewhere
                else - the NEXT cue the pointer reaches fails with `no-track`.

                `stopIssued` is what makes this safe rather than a race.
                `enforceStops` sets it when it has told the audio side, and its
                stop is immediate and reaches every slot of the track, so a
                launch that was placed for a sample in the future has been
                cancelled by the time this runs. Never sounded, told to stop,
                and not sounding now: there is nothing left to wait for. */
            if (run->state == runState::stopping && run->stopIssued
                  && ! run->sawPlaying && ! playing)
            {
                engine.submit (origin::engine, "run.ended", one (run->id));
                continue;
            }

            /*  A run that was sounding and is not any more has ended. The
                launcher clip stops itself at the end of its length - the file's,
                less whatever a start offset skips - so this is the ordinary way
                a cue finishes as well as how a stop is noticed. */
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
        /*  AND THE HORIZON REACHING A BLOCK, which lives here for the reason
            `audio.arm` does: it is an ACTION. It reserves voices, claims slots
            and writes values to somebody else's desk, all before anybody has
            pressed anything.

            A COMMAND AND NOT SOMETHING THE HOOK DOES, which is the rule every
            decision in this engine follows: `wfg replay` runs no hooks, so a
            preparation that happened only inside one would be absent from every
            replay - and a replay would then diverge from the session it is
            reproducing at the first GO into a prepared scene, because the run
            it was supposed to adopt would not exist.

            VARIADIC, like `go`, because how many runs a horizon makes is a
            property of how deep the pointer is rather than a constant: a member
            three manual groups down prepares three of them. */
        registry.add ({ "run.prepare",
                        "The horizon reached this cue: get its block ready ahead of any GO -"
                        " headers run, slots claimed, media armed - and hold.",
                        { { "cue", 's', false }, { "run", 's', true, true } },
                        true,
                        [&engine, &runner, &document, &focus]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            std::vector<std::string> supplied;

                            for (std::size_t n = 1; n < args.size(); ++n)
                                supplied.push_back (args[n].getString());

                            const auto made = runner.prepareStandby (engine, context.tick,
                                                                     list, cueId, supplied);

                            std::vector<osc::Value> applied { osc::Value::string (cueId) };

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        /*  WHERE SOMEBODY IS POINTING, which is a question and not an act.

            §3.13's state position: a client drags a finger along the list and
            asks what the show WOULD be there. Nothing moves - no sound, no
            value, no pointer - and that is the whole difference between this
            and `list.loadToTime`, which takes the same coordinate and makes it
            true.

            IT IS STILL A COMMAND AND STILL LOGGED, because §4.11 says every
            gesture-reachable action is one and because a replay that skipped it
            would publish a different `solve` than the session did - a readout
            that disagreed with the log about a show nobody had touched. */
        registry.add ({ "list.aim",
                        "Points at a position in a list - a cue and how far into it - and asks"
                        " what the show would be there. Changes nothing.",
                        { { "list", 's', false }, { "cue", 's', false },
                          { "offset", 'd', false } },
                        true,
                        [&runner, &document] (CommandContext&,
                                              const std::vector<osc::Value>& args)
                        {
                            const auto listId = args[0].getString();
                            const auto cueId = args[1].getString();

                            const auto list = document.findById (listId);

                            if (! list.isValid() || list.getType().toString() != "List")
                                return Outcome::rejected (reason::unknownId);

                            /*  AN EMPTY CUE CLEARS THE AIM, which is a position
                                too: nobody is pointing at anything, and the
                                solve says nothing rather than answering about a
                                cue that was deleted underneath it. */
                            if (cueId.empty())
                            {
                                runner.listState().aimAt (listId, {});
                                return Outcome::ok (args);
                            }

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            runner.listState().aimAt (listId,
                                                      { cueId, args[2].asDouble() });
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  AND THE JUMP ITSELF, which is `list.aim`'s question made true.

            ONE RECORD, whose applied arguments carry every run identifier it
            drew in the order it drew them - the `go` pattern, because a replay
            never draws one of its own. Everything the jump does is inside the
            handler: what it abandons is ended, the pointer moves, the tree is
            built and the values go out, all in one drain. A jump made of six
            records would be a jump a replay could interleave differently.

            IT TAKES NO POSITION OF ITS OWN and reads the list's aim, which is
            what a client has been dragging. Two commands and one coordinate:
            §3.13's two pointers are the question and the answer. */
        registry.add ({ "list.loadToTime",
                        "Makes the list's aim true: ends what the jump abandons, moves the"
                        " pointer, rebuilds the runs mid-way and sends what differs.",
                        { { "list", 's', false }, { "run", 's', true, true } },
                        true,
                        [&engine, &runner, &document]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto listId = args[0].getString();
                            const auto list = document.findById (listId);

                            if (! list.isValid() || list.getType().toString() != "List")
                                return Outcome::rejected (reason::unknownId);

                            std::vector<std::string> supplied;

                            for (std::size_t n = 1; n < args.size(); ++n)
                                supplied.push_back (args[n].getString());

                            const auto made = runner.loadToTime (engine, document, context.tick,
                                                                 listId, supplied);

                            std::vector<osc::Value> applied { osc::Value::string (listId) };

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.revoke",
                        "The pointer moved away before a GO: give back everything the horizon"
                        " was holding for this run, and finish it.",
                        { { "run", 's', false } },
                        true,
                        [&engine, &runner] (CommandContext& context,
                                            const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            runner.revokePrepared (engine, context.tick, runId);
                            return Outcome::ok (args);
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
