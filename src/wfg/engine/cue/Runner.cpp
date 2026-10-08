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

#include <wfg/engine/cue/LaneCommands.h>
#include <wfg/engine/cue/TakeCommands.h>
#include <wfg/engine/cue/FxRows.h>

#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/DohSetting.h>
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/cue/SamplerLayout.h>
#include <wfg/engine/tree/Touches.h>
#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/cue/Solver.h>
#include <wfg/engine/cue/Override.h>

#include <wfg/engine/midi/MidiMessages.h>

#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountProbe.h>
#include <wfg/engine/tree/MountSender.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/video/DcaOpacity.h>
#include <wfg/engine/video/VideoRamp.h>
#include <wfg/engine/osc/OscValue.h>

/*  For `juce::Decibels` alone, which turns a send's level into a coefficient.
    Written out rather than leaned on: this file reaches it today through
    Tracktion's own headers, and the strict Linux job compiles against
    libstdc++, where a transitive include that happens to work here is exactly
    the kind of thing that does not there. */
#include <juce_audio_basics/juce_audio_basics.h>

#include <wfg/engine/cue/InsertChain.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/document/LevelLane.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <set>
#include <utility>

namespace wfg::cue
{
    namespace
    {
        /*  `idProperty` is ShowWalk.h's since 2026-09-18, when this file began
            reading routing through the schema `Reader` that lives there; a
            second copy of the same identifier here made every use of it
            ambiguous, and one definition is the right number anyway. */

        /*  Silence, spelled as the parameter table spells it. Written here
            rather than included from CueMatrix because this is the cue layer
            and it names no audio type - one number repeated is cheaper than a
            dependency that would let a Tracktion header in. */
        constexpr double silenceDb = -120.0;

        /*  HOW FAR INSIDE ITS LAST RANGE A SEEK PAST THE END LANDS (namespace
            draft §30.4, RH): a millisecond before the out-point - the end, and
            the same floor a seek to the top of a file keeps - so a hand that
            ran off the end of the ruler hears the end, not the top again. */
        constexpr double seekHairSeconds = 0.001;

        std::string kindOfCue (const juce::ValueTree& cue)
        {
            const auto element = cue.getType().toString();

            if (element == "Cue")   return "memo";
            if (element == "Group") return "group";
            if (element == "Media") return "media";
            if (element == "Mic")   return "mic";
            if (element == "Video") return "video";
            if (element == "Fade")  return "fade";
            if (element == "Transport") return "transport";
            if (element == "Osc")   return "osc";
            if (element == "Midi")  return "midi";
            if (element == "Start") return "start";

            return {};
        }

        std::vector<osc::Value> one (const std::string& text)
        {
            return { osc::Value::string (text) };
        }

        /*  A `node.set` of a device's address: the address, then every value of
            the message (namespace draft §45) - one for a node of one. */
        std::vector<osc::Value> addressAnd (const std::string& address, const osc::Values& values)
        {
            std::vector<osc::Value> args { osc::Value::string (address) };
            args.insert (args.end(), values.begin(), values.end());
            return args;
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

        /*  A MEDIA CUE'S SENDS' LANES (namespace draft §28), by the bus each
            send feeds - the lists the write door and `validate` judged, read
            here by the same judge (PX). A send with no lane, which is every
            send written before there were lanes, has no entry, and a mic
            cue's send has none (QA). */
        std::map<std::string, std::vector<doc::LanePoint>> sendLanesOf (const juce::ValueTree& cue)
        {
            std::map<std::string, std::vector<doc::LanePoint>> out;

            if (! cue.hasType ("Media"))
                return out;

            for (const auto& child : cue)
            {
                if (! child.hasType ("Send"))
                    continue;

                auto points = doc::readLevelLane (child[juce::Identifier ("levelLane")].toString().toStdString())
                                  .points;

                if (! points.empty())
                    out[child[juce::Identifier ("bus")].toString().toStdString()] = std::move (points);
            }

            return out;
        }

        /** What those lanes ask for at one second of the file, by bus. */
        std::map<std::string, double> sendLaneOffsets (const std::map<std::string, std::vector<doc::LanePoint>>& lanes,
                                                       double seconds)
        {
            std::map<std::string, double> out;

            for (const auto& [bus, points] : lanes)
                out[bus] = doc::laneLevelDb (points, seconds);

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
        /*  A SAMPLING CHANNEL LET GO (Phase 9c): said by the run table's own
            release, which the handlers that end a run make live and on replay. */
        runs.onRelease = [this] (const std::string& slotId, const std::string&, const std::string& toRun)
        {
            takeReleased (slotId, toRun);
        };
    }

    Runner::~Runner()
    {
        runs.onRelease = nullptr;
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

        /*  THE SECOND OF THE FILE A SLICE'S CLOCK IS AT, `played` samples after
            the slice's start (namespace draft §33): its first pass from
            `firstFrom`, `first` samples long, then pass after pass of `pass`
            from the in-point `origin`. A slice nobody moved has a first pass
            that is an ordinary one from the in-point, and these are then the
            same integers the playhead always used - `(e - pass) % pass` is
            `e % pass`. Nought `pass` is a run that does not wrap. */
        double secondInSlice (std::int64_t played, double origin, double firstFrom,
                              std::int64_t first, std::int64_t pass, double rate) noexcept
        {
            if (pass > 0 && played >= first)
                return origin + static_cast<double> ((played - first) % pass) / rate;

            return (played < first ? firstFrom : origin) + static_cast<double> (played) / rate;
        }

        //  The same, on the file's samples at a speed, where `played` is a fraction.
        double secondInSlice (double played, double origin, double firstFrom,
                              double first, double pass, double rate) noexcept
        {
            if (pass > 0.0 && played >= first)
                return origin + std::fmod (played - first, pass) / rate;

            return (played < first ? firstFrom : origin) + played / rate;
        }

        /*  How many passes a slice's clock has finished, `played` samples after
            its start: none during the first, one more at every pass after. */
        std::int64_t passesDoneIn (double played, double first, double pass) noexcept
        {
            if (played < first || ! (pass > 0.0))
                return 0;

            return 1 + static_cast<std::int64_t> (std::floor ((played - first) / pass));
        }

        /*  A FILE'S SECOND THROUGH A SLICE'S LOOP, either way (namespace draft
            §41) - the reader's own arithmetic (patch 0002's `bentFrameFor`) in
            seconds: `u` is where the file would be with no loop; in the slice
            or above it, it is that; below it going backwards, round its end
            again; bouncing, out and back. No slice, no loop. */
        double loopedSecond (double u, const Run::SlicePoints* slice, int direction) noexcept
        {
            if (slice == nullptr || ! (slice->out > slice->in))
                return u;

            const auto length = slice->out - slice->in;
            const auto v = u - slice->in;

            if (slice->pingPong)
            {
                if (v < 0.0 && direction > 0)
                    return u;

                auto w = std::fmod (v, 2.0 * length);

                if (w < 0.0)
                    w += 2.0 * length;

                return slice->in + (w < length ? w : 2.0 * length - w);
            }

            if (v >= 0.0 && (direction < 0 || v < length))
                return u;

            auto w = std::fmod (v, length);

            if (w < 0.0)
                w += length;

            return slice->in + w;
        }

        //  The slice a run is in, if it has ranges.
        const Run::SlicePoints* sliceOf (const Run& run) noexcept
        {
            if (run.range < 0 || static_cast<std::size_t> (run.range) >= run.playingSlices.size())
                return nullptr;

            return &run.playingSlices[static_cast<std::size_t> (run.range)];
        }

        /*  WHERE A BENT RUN'S FILE IS AT A SAMPLE: from its last turn, the
            speed's size times the way it goes, through its slice's loop. A cue
            with no ranges stays between its piece's two ends. */
        double bentSecondAt (const Run& run, double sample, double rate) noexcept
        {
            const auto moved = std::max (0.0, run.rateClock.sourceAt (sample) - run.turnSource) / rate;
            const auto u = run.turnFile + static_cast<double> (run.direction) * moved;
            const auto* slice = sliceOf (run);

            //  An end the show does not know is no bound (§41).
            if (slice == nullptr)
                return std::clamp (u, run.pieceStart, run.pieceEnd > run.pieceStart ? run.pieceEnd
                                                                                     : std::numeric_limits<double>::infinity());

            return loopedSecond (u, slice, run.direction);
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
            range.id = id;
            range.in = osc::parseDouble (value ("in")).value_or (0.0);
            range.out = osc::parseDouble (value ("out")).value_or (0.0);

            /*  One rather than nought when the row is absent, because the row's
                default is one pass and nought means FOR EVER. Reading a missing
                attribute as "loop this range for ever" would be the worst
                possible way to be wrong about it. */
            const auto loops = value ("loops");
            range.loops = loops.empty() ? 1 : std::atoi (loops.c_str());
            range.pingPong = value ("pingPong") == "true";

            out.push_back (range);
        }

        /*  DOCUMENT ORDER IS PLAYLIST ORDER, which is why nothing sorts here:
            `range/index` is derived from exactly this walk, so the strip's
            numbering and the order the graph plays them in are one fact. */
        return out;
    }

    audio::EqSettings Runner::eqOf (const juce::ValueTree& cue,
                                   const std::map<std::string, double>* moved) const
    {
        /*  THROUGH THE SCHEMA, for the reason resolveRouting gives: the
            canonical writer omits every attribute at its default, so a cue
            whose EQ nobody touched has no eq* attribute at all, and a raw read
            would make a flat EQ into a row of noughts - a high-pass at 0 Hz, a
            width of nought. The defaults are the table's and do not change
            under a running show. */
        static const Reader schema;

        /*  AND WHAT A LOCKED SHOW IS RIDING, first (2026-09-25): a row held
            live is the value the voice plays until somebody keeps it or lets
            it go. */
        const auto cueId = cue.getProperty ("id").toString().toStdString();
        const auto riding = [&] (const char* name) -> const std::string*
        {
            return liveLayer != nullptr ? liveLayer->rowOf (cueId, name) : nullptr;
        };

        const auto flag = [&] (const char* name)
        {
            if (const auto* held = riding (name))
                return *held == "true";

            return schema.flag (cue, "sound", name);
        };
        const auto number = [&] (const char* name)
        {
            /*  WHAT A FADE MOVED THE RUN'S TO (namespace draft §26), newer
                than the lock's ride. Numbers only: a fade moves no switch. */
            if (moved != nullptr)
                if (const auto faded = moved->find (std::string ("eq/") + name); faded != moved->end())
                    return static_cast<float> (faded->second);

            if (const auto* held = riding (name))
                if (const auto value = osc::parseDouble (*held))
                    return static_cast<float> (*value);

            return static_cast<float> (schema.number (cue, "sound", name));
        };
        const auto shape = [&] (const char* name)
        {
            const auto* held = riding (name);
            const auto word = held != nullptr ? *held : schema.text (cue, "sound", name);

            if (word == "lowShelf")  return audio::EqSettings::Shape::lowShelf;
            if (word == "highShelf") return audio::EqSettings::Shape::highShelf;

            return audio::EqSettings::Shape::peak;
        };

        audio::EqSettings out;
        out.on = flag ("eqOn");
        out.hpf = flag ("eqHpf");
        out.lpf = flag ("eqLpf");
        out.hpfFreq = number ("eqHpfFreq");
        out.lpfFreq = number ("eqLpfFreq");
        out.band[0] = { shape ("eqB1Shape"), number ("eqB1Freq"), number ("eqB1Gain"), number ("eqB1Q"),
                        flag ("eqB1On") };
        out.band[1] = { audio::EqSettings::Shape::peak, number ("eqB2Freq"), number ("eqB2Gain"),
                        number ("eqB2Q"), flag ("eqB2On") };
        out.band[2] = { audio::EqSettings::Shape::peak, number ("eqB3Freq"), number ("eqB3Gain"),
                        number ("eqB3Q"), flag ("eqB3On") };
        out.band[3] = { shape ("eqB4Shape"), number ("eqB4Freq"), number ("eqB4Gain"), number ("eqB4Q"),
                        flag ("eqB4On") };

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
        if (! fireAtOnce && kind != "media" && kind != "mic")
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
        /*  NOT A RUN DOH! TOOK BACK (2026-10-01, namespace draft §24): its
            voice is on its way out over the Doh fade, and a GO on the cue
            inside that fade is a GO on a cue that is not sounding - it starts
            the cue again, which is what the corrected GO is. */
        if (kind == "media" || kind == "mic")
            if (const auto* live = liveUntakenRunOf (cueId))
            {
                if (fireAtOnce && live->state == runState::armed)
                    if (auto* armed = runs.find (live->id))
                    {
                        /*  THE GO ADOPTS THE ARM, and tags it (§24): only one
                            nobody had asked for, which is what the arm was -
                            noted as it was first (D2), so a Doh of a GO nobody
                            heard hands it back exactly. */
                        if (armed->launchRequestedAtTick <= 0)
                        {
                            snapshotAdoption (armed->id, {});
                            armed = runs.find (live->id);
                            stampSubtree (armed->id, currentGo);
                        }

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

                        /*  A MIC CUE'S TAKE HEARS THIS GO TOO (2026-09-30).
                            `onGo` was acted on in `fireKind` alone, which a cue
                            armed ahead never reaches when it has no pre-wait -
                            so GO on the ordinary standby, the armed one, left
                            the take as it was: Scene 5's loop never looped.
                            With a pre-wait the GO arrives through `fireNow`
                            and `fireKind` answers it there, once. */
                        if (kind == "mic" && armed->preWaitTicks <= 0)
                            applyTakeOnGo (cue, live->id);
                    }

                return live->id;
            }

        /*  GO ON AN ARMED SAMPLER GROUP IS A REFRESH (PRD §3.27, §3.8's
            table), and not a second bank. Every member that has lost its strip
            to another group's takeover claims it back, in this group's own
            takeover mode - which the scheduler does on its next tick for every
            member with no run, once `lostStrips` is empty again. A complete
            group has nothing lost, so the GO changes nothing and decision N
            needs no exception.

            Only a SAMPLER group, deliberately. The comment above says a group
            fired again is ignored, and for every other group the code has
            never checked: a second GO on a running timeline group at the top
            of a list starts the scene again. Changing that is the author's
            call and not this phase's (namespace draft §16.12 records it). */
        if (fireAtOnce && kind == "group" && textOf (cue, "mode") == "sampler")
            if (const auto* live = liveUntakenRunOf (cueId))
            {
                if (auto* armed = runs.find (live->id))
                {
                    armed->lostStrips.clear();
                    takeOverFrom (armed->id, cue);
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
                adoptPrepared (adopted, {}, true, tick);
                return adopted;
            }

        auto id = runId;

        if (id.empty())
            id = ids.generate();

        createRun (id, cueId, kind, {});
        auto* run = runs.find (id);

        if (run == nullptr)
            return {};

        /*  THE WAITS, COPIED NOW. §4.10's rule applied to a duration: the run
            instantiates what the cue said when it was fired, so editing the cue
            during the wait changes the next run and not this one. */
        run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
        run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

        /*  A BED ESC PAUSED, CARRIED ON (K8): the assertion's resume point is
            this run's own start, as a jump's would be - a second of the file,
            or the slice it was in when the cue still has it. And whatever made
            this run, the pause is spent: a fire by name or a trigger starts the
            cue as it always did, and the next pass has nothing to carry on. */
        if (fireAtOnce)
        {
            if (resumeNext.has_value() && kind == "media")
            {
                const auto ranges = rangesOf (cue);
                const auto slices = static_cast<int> (ranges.size());

                if (resumeNext->range >= 0 && slices > 0)
                {
                    run->startRange = std::min (resumeNext->range, slices - 1);
                    run->startOffset = 0.0;

                    /*  AND INSIDE IT, AT THE SAME POINT AND IN THE SAME PASS
                        (the author, 2026-10-02, K8's review): the seconds the
                        slice had played, passes and all. Only into the slice
                        it was in - one the cue has lost since is entered at the
                        in-point of its last - and only into a pass the slice
                        still has: a loop count lowered since keeps the point
                        and gives it its last pass. */
                    if (resumeNext->range < slices && resumeNext->from > 0.0)
                    {
                        const auto& slice = ranges[static_cast<std::size_t> (run->startRange)];
                        const auto length = slice.out - slice.in;
                        auto from = resumeNext->from;

                        if (length > 0.0 && slice.loops > 0
                              && from >= static_cast<double> (slice.loops) * length)
                            from = static_cast<double> (slice.loops - 1) * length + std::fmod (from, length);

                        run->sliceFrom = length > 0.0 ? from : 0.0;
                    }
                }
                else if (resumeNext->range < 0 && slices == 0 && resumeNext->from > 0.0)
                {
                    run->startOffset = resumeNext->from;
                }

                /*  CARRIED ON, SO IT ARRIVES OVER A DE-CLICK (K8's review;
                    §24, GQ): armed at silence, up to its level over a tenth
                    of a second once it is heard. A bed the hook started from
                    its top had no resume point, and arrives as it always has. */
                run->deClick = true;
            }

            paused.erase (cueId);
        }

        /*  MADE READY AT THE POINT A DOH PAUSED IT (2026-10-02, D2, namespace
            draft §24.12): a sound the next GO carries on is armed by the
            standby where it was, with no pre-wait, silent, and arriving over
            the de-click at the level it had - whoever asks for the arm, so a
            surface's `audio.arm` of the cue is the same arm. Only a sound at
            the top of its list; one inside a running act is armed under it by
            the horizon (`spawnChild`). The mark is handler state and
            `audio.arm` a record, so a replay arms the same point. */
        if (! fireAtOnce)
            if (const auto* root = soundRootOf (cueId); root != nullptr && root->underRun.empty())
                armAtRoot (*run, *root, cue);

        /*  WHERE ITS ARM BEGINS, kept on the run (K8's review): what a pause
            counts from when it has no playhead to read, rather than the cue's
            offset as an edit since may have left it. */
        run->armedOrigin = run->startOffset > 0.0 ? run->startOffset : numberOf (cue, "startOffset");

        if (! fireAtOnce)
        {
            /*  THE SMALLEST HORIZON THERE IS, and it has been here since PR 2.3
                without a word for itself: a media cue at standby is armed, and
                `armed` is exactly what §13.6's vocabulary calls a preparation
                with nothing to verify. Saying it on the row costs one
                assignment and is the difference between an operator seeing that
                the next cue is ready and having to know that it always is. */
            run->prepare = preparedness::armed;

            if (kind == "mic")
                armMic (engine, cue, id);
            else
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
            else if (kind == "mic")
                armMic (engine, cue, id);

            return id;
        }

        fireKind (engine, tick, cue, kind, id);
        return id;
    }

    bool Runner::isManualGroup (const juce::ValueTree& cue) const
    {
        /*  A SAMPLER GROUP IS NOT ONE, whatever its `advance` says (Phase 6):
            the operator is not its parent the way they are a manual
            sequence's - no GO steps through its members, a hand on a strip
            launches them - so the pointer does not descend into it and a
            surface may fire it by name to arm it. */
        return cue.isValid()
                 && cue.getType().toString() == "Group"
                 && textOf (cue, "mode") != "timeline"
                 && textOf (cue, "mode") != "sampler"
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
            they disagreed about would be a scene created twice.

            AND IT STOPS AT THE FIRST GROUP THE OPERATOR IS NOT THE PARENT OF,
            which is new today and is the other half of the author's decision
            (2026-09-16). The pointer may now be parked on a member of an
            automatic or a timeline group; GO fires THAT CUE and not the scene,
            and starting the whole group from it is a second named gesture.

            The walk therefore climbs only while the groups it passes are ones
            the operator drives. A member of a manual sequence plays as part of
            its group, which is what the paragraph in `fireStandby` says and
            what §3.6 means by a group organising its members' time - so the
            manual ones above it still have to be live before the member can be
            their child. An automatic or timeline group is the MACHINE's to
            enter: making it live here would have GO start the scene the
            operator was trying to audition one cue of, which is precisely what
            they asked not to happen.

            STOPPING RATHER THAN FILTERING, because the two differ and only one
            of them is right. A manual group inside an automatic one inside a
            manual one is reached by stopping with just the innermost; filtering
            would have kept the outermost as well and made it the member's
            parent ACROSS a group the machine owns, which is a scene entered by
            a side door. The contiguous run of manual groups immediately around
            the cue is the whole of what GO may bring to life. */
        std::vector<juce::ValueTree> ancestors;

        for (auto node = document.findById (cueId).getParent();
             node.isValid() && node != list;
             node = node.getParent())
        {
            if (node.getType().toString() != "Group")
                continue;

            if (! isManualGroup (node))
                break;

            ancestors.push_back (node);
        }

        std::reverse (ancestors.begin(), ancestors.end());
        return ancestors;
    }

    std::vector<std::string> Runner::horizonGroupsFor (const juce::ValueTree& list,
                                                       const std::string& cueId) const
    {
        /*  THE SAME CHAIN `prepareStandby` BUILDS, by identifier: the groups
            between the cue and the list, outermost first, and the cue itself
            when it is a group. */
        std::vector<std::string> out;

        for (const auto& group : descentTo (list, cueId))
            out.push_back (group[idProperty].toString().toStdString());

        if (const auto cue = document.findById (cueId);
            cue.isValid() && cue.getType().toString() == "Group")
            out.push_back (cueId);

        return out;
    }

    bool Runner::leftBehind (const Run& run, const std::string& standby,
                             const std::vector<std::string>& horizon) const
    {
        /*  ONLY WHAT WAS MADE READY IN CASE, and never the pointer's own cue. */
        if (! run.onlyPrepared() || run.cue == standby)
            return false;

        /*  NOR THE SOUND OF THE MOVIE IT STANDS ON, armed with it (namespace
            draft 37.5, WJ) - nor of the block's first cue, for the same reason. */
        if (run.parent.empty())
            if (const auto sound = document.findById (run.cue); followsAMovie (sound))
            {
                const auto movie = sound[juce::Identifier ("lockedTo")].toString().toStdString();

                if (movie == standby || (! horizon.empty() && movie == horizon.front()))
                    return false;
            }

        /*  AT THE TOP OF A LIST, a block or an arm is left once it is not the
            block the pointer is in - the outermost of its groups (§13.6). */
        if (run.parent.empty())
            return horizon.empty() || run.cue != horizon.front();

        /*  AND UNDER ANOTHER RUN (2026-10-01, namespace draft §23.9, J2): a
            scene the horizon made ready under an act that is running - or
            inside a block the pointer is still in - once it is not one of the
            groups the pointer stands in. Only the top of a list was looked at,
            so a scene made ready under an act kept its voices and its pre-sends
            for as long as the act ran, and after.

            A BLOCK, AND ONLY ONE NOBODY HAS ADOPTED: its mark is the test. A GO
            that adopts a block under a running group clears the mark and leaves
            it `preparing` until that group's job launches it, a tick later - the
            GO's scene by then, nothing left behind. And so does a GO that
            enters the block's parent on the block's own row (IC).

            NOR ONE THAT GOES WITH ITS PARENT: under a preparation the pointer
            has left too, which is given back whole, or under a group being
            stopped, whose own job gives it back. */
        const auto* above = runs.find (run.parent);

        if (above != nullptr && above->state == runState::stopping)
            return false;

        /*  AN ARM, ONLY UNDER A MANUAL GROUP PLAYING ITS MEMBERS (2026-10-01,
            namespace draft §23.9, ID). The pointer on a sound inside an act
            that is running arms it under the act, marked; such a group takes
            only the members a GO asks for, so one the pointer passed over is
            never taken, and held its voice while the act ran - and after. Not
            the pointer's own, which was asked for at the top.

            And no other arm under another run: the horizon arms a scene's first
            sounds under the scene, and once a GO has entered it they wait there
            for its members to ask for them, the GO having moved the pointer on.
            So does a sound a GO entered an act at while the act's header runs -
            which is why the group must be in its members phase, where whatever
            it was told to enter at has already been asked for. */
        if (run.state == runState::armed)
        {
            if (above == nullptr || ! above->isGroup()
                  || ! isManualGroup (document.findById (above->cue)))
                return false;

            return std::any_of (scheduled.begin(), scheduled.end(),
                                [above] (const GroupJob& job)
                                {
                                    return job.run == above->id && ! job.retired
                                             && job.phase == groupPhase::members;
                                });
        }

        if (run.state != runState::preparing || run.prepare.empty())
            return false;

        const auto wanted = [&horizon] (const std::string& cueId)
        {
            return std::find (horizon.begin(), horizon.end(), cueId) != horizon.end();
        };

        if (above != nullptr && above->state == runState::preparing && ! wanted (above->cue))
            return false;

        return ! wanted (run.cue);
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
        /*  A MIC CUE TOO (Phase 9b): its channel claimed and its plugins set
            ahead, the gate shut - undone by letting go of the claim, and
            nothing outside this machine heard anything. */
        if (element == "Media" || element == "Mic")
            return true;

        if (element != "Osc")
            return false;

        /*  NOR A CUE OF SEVERAL MESSAGES, OR ONE WITH CURVES (namespace draft
            45, ZH): its first alone would be read back and pre-sent, the rest
            left for its GO - half a cue sent early - and a curve is a cue that
            plays. One of one message is prepared as it was. */
        for (const auto& child : cue)
            if (child.hasType ("Message") || child.hasType ("Curve"))
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

    std::vector<std::string> Runner::blockCuesIn (const juce::ValueTree& group) const
    {
        std::vector<std::string> out;

        /*  THE DERIVED LINES FIRST, WHICH IS §13.7's ORDER AND HAS A REASON.

            A written header cue may reasonably depend on what the presets set -
            position the source, then move it - and the reverse dependency has
            no natural example. A header is a sequence whatever the group's mode
            says, and §3.12 puts prepare and commit there precisely because
            preparation has an order. */
        collectPresetsOf (group, group[idProperty].toString().toStdString(), out);

        for (const auto& cueId : membersOf (group.getChildWithName ("Header")))
        {
            /*  A cue that is both a written header cue AND marked for this
                group's header is one cue, not two. It happens where somebody
                dragged a header cue onto its own group's header, which is a
                reasonable thing to do by accident and must not spawn the cue
                twice. */
            if (std::find (out.begin(), out.end(), cueId) == out.end())
                out.push_back (cueId);
        }

        return out;
    }

    std::vector<std::string> Runner::preparableIn (const juce::ValueTree& group) const
    {
        std::vector<std::string> out;

        for (const auto& cueId : blockCuesIn (group))
            if (const auto cue = document.findById (cueId); isPreparable (cue))
                out.push_back (cueId);

        return out;
    }

    bool Runner::beginPreparation (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                                   const std::function<std::string()>& drawId,
                                   std::vector<std::string>& used, std::int64_t tick,
                                   const std::set<std::string>& leaveOut)
    {
        auto cues = preparableIn (group);

        /*  NOT WHAT A DOH LEFT WITH A DEVICE'S OPERATOR (2026-10-01, namespace
            draft §24, HO). A pre-send the GO committed had already reached the
            device; pre-sent again here it would reach it twice, over whatever
            its operator has done since. Left out of the block, it fires at
            entry under the GO that enters it - the corrected GO, which runs it
            and sends nothing - so the device gets it once in all. `preparableIn`
            is left alone, so the block does not read `partial` for it. */
        cues.erase (std::remove_if (cues.begin(), cues.end(),
                                    [&leaveOut] (const std::string& cueId) { return leaveOut.count (cueId) > 0; }),
                    cues.end());

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
            const auto made = spawnChild (engine, job.run, cueId, drawId(), tick);

            if (made.empty())
            {
                used.pop_back();
                continue;
            }

            used.back() = made;

            auto* child = runs.find (made);

            if (child == nullptr)
                continue;

            child->prepare = preparedness::armed;

            /*  A MIC CUE IS ARMED HERE, AS A MEDIA CUE IS BY SPAWNING
                (2026-09-30, namespace draft §23.3): its channel claimed or
                queued for, its plugins set, the gate shut - which is what
                `isPreparable` has always said preparing one means. The
                preparing phase launched it instead, and a preparation opened a
                live microphone into the show before any GO. Here and not in
                `spawnChild`, because a group's ordinary member is armed when
                it is launched, and a claim taken at spawning would queue a
                sequence's next mic cue ahead of its time. */
            if (child->kind == "mic")
                if (const auto micCue = document.findById (cueId); micCue.isValid())
                    armMic (engine, micCue, made);
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
        /*  AGAINST THE WHOLE BLOCK, not against the written header. `partial`
            means "something here could not be got ready", so what it counts
            against is every cue the block WOULD have prepared - the derived
            lines and the written ones together. Compared with the written
            header alone, a scene whose header is entirely derived read
            `partial` for ever with nothing wrong with it, which is the shape
            the preset design encourages and what the Phase 4 driver found. */
        if (missing || preparableIn (group).size() != blockCuesIn (group).size())
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

            if (child->kind == "media" || child->kind == "mic")
            {
                /*  ARMED IS AS FAR AS A MEDIA PREPARE GOES, and `failed` is as
                    settled as armed: a cue whose file is missing has finished
                    being got ready, badly, and holding the whole block for it
                    would mean one absent sound stopped the scene from ever
                    being prepared. The row says `partial`. */
                /*  A MIC CUE THE SAME (2026-09-30): its arm is its whole
                    preparation. One queued behind its channel's holder holds no
                    track until that one lets go, and has got as far as it can -
                    the block says `pending`, as `settledWord` finds. */
                const auto queued = child->kind == "mic" && ! child->pending.empty();

                if (child->track < 0 && ! child->isFinished() && ! queued)
                    return false;

                continue;
            }

            if (! child->isFinished())
                return false;
        }

        return true;
    }

    void Runner::adoptPrepared (const std::string& runId, const std::string& entersAt,
                                bool enters, std::int64_t tick)
    {
        /*  AS IT WAS, FIRST (2026-10-02, D2, namespace draft §24.12): the block, everything under
            it and its jobs, so a Doh of a GO nobody heard hands the block back
            exactly - prepared again, its pre-sends still on the desk. The act
            whose job will take it, when it sits under one already running. */
        {
            std::string act;

            if (const auto* run = runs.find (runId))
                if (const auto* above = runs.find (run->parent);
                    above != nullptr && above->isGroup() && above->state != runState::preparing)
                    act = above->id;

            snapshotAdoption (runId, act);
        }

        /*  THE GO TAKES THE BLOCK, WHOLE (2026-10-01, namespace draft §24): the
            horizon made it nobody's, and the GO that adopts it makes it its
            own - its pre-sends, its arms, the groups nested in it - so a Doh of
            that GO finds all of it. */
        stampSubtree (runId, currentGo);

        if (auto* run = runs.find (runId))
        {
            run->enterAt = entersAt;

            if (enters)
            {
                run->state = runState::playing;

                /*  AND IT READS STARTED, as a scene fired cold does: the tick of
                    the GO that entered it (a readout, and the launch evidence
                    §24 reads). */
                run->launchRequestedAtTick = tick;

                /*  A SAMPLER GROUP ARMING TAKES OVER in this door as in
                    `fireKind`'s: a group the horizon prepared is entered here,
                    not fired there, and its takeover must not depend on which
                    road the GO took. */
                if (const auto cue = document.findById (run->cue);
                    cue.isValid() && textOf (cue, "mode") == "sampler")
                    takeOverFrom (runId, cue);
            }

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
    const std::map<std::string, std::vector<std::string>>& Runner::writtenAddresses() const
    {
        /*  A CACHE ASKED BY THE DOCUMENT'S REVISION, which is the shape PR 4.4
            settled on for the liveness analysis and for the same reason: the
            set changes only when somebody edits the show, and a second thing to
            invalidate by hand is a second thing to forget. */
        if (addressesFor == document.revision() && ! addressesWritten.empty())
            return addressesWritten;

        addressesFor = document.revision();
        addressesWritten.clear();

        if (mounts == nullptr)
            return addressesWritten;

        const std::function<void (const juce::ValueTree&)> visit
            = [&] (const juce::ValueTree& node)
        {
            /*  AND AN OSC CUE'S FURTHER MESSAGES, each an address it writes
                (namespace draft 45). */
            if (node.getType().toString() == "Osc" || node.getType().toString() == "Message")
            {
                const auto address = node[juce::Identifier ("address")].toString().toStdString();

                /*  AN EVENT IS NOT A VALUE, the same exclusion the solver makes
                    (§3.13 step 4) read from the other end: "fire the pyro" has
                    no state, so there is nothing to observe and asking would be
                    a question with no answer. */
                if (! address.empty())
                    if (const auto* mounted = mounts->nodeAt (address);
                        mounted != nullptr && mounted->kind != tree::Kind::event)
                        if (const auto mountId = mounts->mountOf (address); ! mountId.empty())
                        {
                            auto& addresses = addressesWritten[mountId];

                            if (std::find (addresses.begin(), addresses.end(), address)
                                  == addresses.end())
                                addresses.push_back (address);
                        }
            }

            for (int child = 0; child < node.getNumChildren(); ++child)
                visit (node.getChild (child));
        };

        visit (document.root());
        return addressesWritten;
    }

    void Runner::observeAfterStep (Engine& engine, std::int64_t tick)
    {
        if (asker == nullptr || mounts == nullptr)
            return;

        /*  ONE SWEEP PER STEP, noticed by counting rather than by being told.
            The step is written by a handler and the sweep is a decision, so
            this is the hook half of the rule the whole engine is built on -
            and it means a replay, which runs no hooks, re-injects the answers
            from the log instead of asking a desk that is not there. */
        if (stepsSeen == lists.stepsTaken())
            return;

        stepsSeen = lists.stepsTaken();

        for (const auto& [mountId, addresses] : writtenAddresses())
        {
            const auto* declaration = mounts->declarationOf (mountId);

            if (declaration == nullptr || ! declaration->canBeAsked())
                continue;

            /*  ONE A SECOND, PER MOUNT. Fifty ticks, which is the same clock
                everything else here counts in. */
            const auto last = observedAt.find (mountId);

            if (last != observedAt.end() && tick - last->second < 50)
                continue;

            observedAt[mountId] = tick;

            for (const auto& address : addresses)
            {
                std::string typeTag;

                if (const auto* node = mounts->nodeAt (address))
                    typeTag = node->typeTags;

                /*  WITH THE WRITES IT IS ASKED AFTER (2026-10-03, namespace
                    draft §24.13, OU). The step's own cues write in a later
                    drain than this - a header's run is spawned and launched by
                    records submitted on the ticks after the GO - so an answer
                    slower than that crosses their write, saying what the desk
                    held before it. Without the count it was kept as the desk's
                    first account since the write: a Doh read the desk as
                    already back, and put nothing back. */
                if (asker->ask ({ mountId, declaration->host, declaration->queryPort,
                                  address, typeTag, true, mounts->writesOf (address) }))
                    ++asked;
            }
        }

        juce::ignoreUnused (engine);
    }

    std::string Runner::assertCue (Engine& engine, std::int64_t tick,
                                   const std::string& cueId, const std::string& runId,
                                   std::optional<ResumePoint> resume)
    {
        /*  THE RESUME POINT RIDES TO THE RUN `fire` MAKES (K8), and to nothing
            else: handed over around the one call, and taken back after it
            whether or not a run was made - a cue fired while its run is still
            live is ignored (decision N), and the next fire must not find it. */
        resumeNext = resume;
        const auto made = fire (engine, tick, cueId, runId);
        resumeNext.reset();

        if (auto* run = runs.find (made))
            run->asserted = true;

        return made;
    }

    bool Runner::inPersistentSection (const std::string& cueId) const
    {
        for (auto node = document.findById (cueId); node.isValid(); node = node.getParent())
            if (node.getType().toString() == "Persistent")
                return true;

        return false;
    }

    double Runner::documentSpeedOf (const juce::ValueTree& cue) const
    {
        /*  The solver's reading (§22.5): one when the document cannot say, and
            never nought by accident - an unreadable number read as nought would
            be a stopped tape. */
        const auto speed = osc::parseDouble (textOf (cue, "rate")).value_or (1.0);

        /*  ITS SIZE (namespace draft §41): how far the file moves a second,
            whichever way - what this reading is used for, a time and a place
            in the file each made of the other. */
        return std::isfinite (speed) ? std::clamp (std::abs (speed), 0.0, 20.0) : 1.0;
    }

    bool Runner::killedSinceLift (const std::string& cueId) const
    {
        for (const auto& run : runs.all())
            if (run.cue == cueId && run.killed && run.endedAtTick >= liftedAt)
                return true;

        return false;
    }

    std::optional<Runner::ResumePoint> Runner::pausedAt (const std::string& cueId) const
    {
        const auto found = paused.find (cueId);

        if (found == paused.end())
            return std::nullopt;

        if (const auto heard = playheads.find (found->second.run); heard != playheads.end())
            return heard->second;

        if (! found->second.resumes)
            return std::nullopt;

        return found->second.at;
    }

    void Runner::pausePersistent (std::int64_t tick)
    {
        /*  ESC ON A PERSISTENT MEDIA CUE IS A PAUSE (the author, 2026-10-02;
            PRD §3.29, §4.4). Everything else about the press is unchanged: the
            bed comes down over the panic fade with the rest, as an abort, and
            the roots' stop ends it. What is added is which run it was, for the
            next step's assertion to carry it on.

            WHERE IT HAD GOT TO IS ITS PLAYHEAD (the author's answer to K8's
            review, 2026-10-02), and that is a readout of the sample clock: a
            hook reads it (`notePausedPlayheads`) and the `run.assert` record
            carries it, and no handler may. What this handler writes beside the
            run is the answer for a session with NO playhead to read - no audio
            side, or a run whose launch was never placed - from HANDLER STATE
            ONLY, so a replay writes the same: where the run's arm began, copied
            onto it when it was made (`armedOrigin`, never the cue's offset as
            an edit since may have left it), and the file's seconds since
            `run.started` at the cue's speed as the document says it. A cue
            with slices: the slice it is in, and the point it was armed at in
            the one it was armed into. */
        for (const auto& run : runs.all())
        {
            if (run.isFinished() || run.state == runState::preparing || run.takenBack || run.stopAsked
                  || ! run.parent.empty() || (run.kind != "media" && run.kind != "mic")
                  || ! inPersistentSection (run.cue))
                continue;

            PausedBed bed;
            bed.run = run.id;
            bed.media = run.media;

            if (run.kind == "media")
            {
                const auto cue = document.findById (run.cue);
                const auto heard = run.startedAtTick >= 0;

                if (! rangesOf (cue).empty())
                {
                    /*  THE SLICE IT IS IN, from `run.range` - a logged record -
                        or the one it was armed into when none has come yet;
                        and, in that one, the point it was armed at. */
                    bed.at.range = std::max (run.range >= 0 ? run.range : run.startRange, 0);
                    bed.at.from = bed.at.range == run.startRange ? run.sliceFrom : 0.0;
                    bed.resumes = heard || run.startRange > 0 || run.sliceFrom > 0.0;
                }
                else
                {
                    const auto elapsed = heard ? static_cast<double> (tick - run.startedAtTick)
                                                   / static_cast<double> (TickClock::rateHz)
                                               : 0.0;

                    bed.at.from = run.armedOrigin + std::max (elapsed, 0.0) * documentSpeedOf (cue);
                    bed.resumes = heard || run.startOffset > 0.0;
                }
            }

            paused[run.cue] = bed;
        }

        /*  THE PASS A STEP BEFORE THE PRESS OPENED IS TAKEN BACK (the author's
            third ruling): Esc takes the section down with everything else, and
            a pass still owed to that step - not run yet, or waiting for the
            desks' answers - would put it straight back after the press. The
            step is counted as asserted, so only a step after the press opens a
            pass; one decided before it and draining behind it is answered by
            `run.assert` itself (`escapedInDrain`). What an earlier pass owed is
            forgotten with it: the next step owes it again. */
        assertedFor = lists.stepsTaken();
        assertDue = -1;
        owed.clear();
        escapedAtTick = tick;
    }

    void Runner::notePausedPlayheads()
    {
        /*  A PLAYHEAD BELONGS TO ITS PAUSE, and goes when the pause has: a
            double Esc, a load-to-time, the fire that made the cue's next run. */
        for (auto entry = playheads.begin(); entry != playheads.end();)
        {
            const auto belongs = std::any_of (paused.begin(), paused.end(),
                                              [&entry] (const auto& bed) { return bed.second.run == entry->first; });

            entry = belongs ? std::next (entry) : playheads.erase (entry);
        }

        if (audio == nullptr)
            return;

        /*  READ ON THE FIRST TICK AFTER THE PRESS, AND BEFORE `updatePositions`
            moves it on (`beforeTick` calls this ahead of it): what the readout
            holds now is what the press's own tick made of the sample clock -
            the press drains after the hooks - so it is the second the bed had
            reached when Esc was pressed, not the one the panic fade has taken
            it to since. A run whose launch was never placed has no playhead,
            and keeps the handler's answer. */
        for (const auto& [cueId, bed] : paused)
        {
            juce::ignoreUnused (cueId);

            if (playheads.count (bed.run) > 0)
                continue;

            const auto* run = runs.find (bed.run);

            if (run == nullptr || run->kind != "media" || run->launchedAtSample <= 0)
                continue;

            ResumePoint at;

            /*  A SLICE'S PLAYHEAD IS HOW FAR THE SLICE HAS GOT, passes and all
                (the author's answer to K8's review): a looping bed carries on
                inside its loop, in the pass it was in. */
            if (run->rangeStartedAtSample > 0)
            {
                at.range = std::max (run->range >= 0 ? run->range : run->startRange, 0);
                at.from = run->slicePlayed;
            }
            else
            {
                at.from = run->position;
            }

            playheads[bed.run] = at;
        }
    }

    void Runner::submitAssert (Engine& engine, const std::string& cueId)
    {
        const auto plain = [&engine, &cueId]
        {
            engine.submit (origin::engine, "run.assert", { osc::Value::string (cueId) });
        };

        const auto found = paused.find (cueId);

        if (found == paused.end())
        {
            plain();
            return;
        }

        /*  THE PLAYHEAD WHEN ONE WAS READ, the handler's count when none was. */
        auto at = found->second.at;
        auto resumes = found->second.resumes;

        if (const auto heard = playheads.find (found->second.run); heard != playheads.end())
        {
            at = heard->second;
            resumes = true;
        }

        if (! resumes)
        {
            plain();
            return;
        }

        const auto cue = document.findById (cueId);

        /*  NOT ANOTHER RECORDING (K8's review): a second of one file is no
            place in another. A cue given a new file while it was paused starts
            that file from its top. */
        if (textOf (cue, "file") != found->second.media)
        {
            plain();
            return;
        }

        /*  ALL BUT OVER THERE, OR PAST IT, by the length the log knows: a bed
            paused in its last half second carries on from its top rather than
            ending the moment it is back (the guard Doh!'s resume uses, §24),
            and one past its end - a file swapped for a shorter one of the same
            name - has nothing there to play. A length not known resumes as it
            is; a slice always has somewhere to carry on, its loop or the next. */
        if (at.range < 0 && durations != nullptr)
        {
            const auto length = durations->find (textOf (cue, "file"));
            const auto speed = documentSpeedOf (cue);

            if (length != durations->end() && length->second > 0.0
                  && at.from >= length->second - 0.5 * speed)
            {
                plain();
                return;
            }
        }

        /*  THE RUN'S IDENTIFIER IS LEFT FOR THE HANDLER TO DRAW, as every
            assertion leaves it; the record it logs carries the one drawn. */
        engine.submit (origin::engine, "run.assert",
                       { osc::Value::string (cueId), osc::Value::string (std::string {}),
                         osc::Value::float64 (at.from),
                         osc::Value::int32 (static_cast<std::int32_t> (at.range)) });
    }

    void Runner::deClickLaunched()
    {
        /*  FROM THE TICK IT IS HEARD, NOT FROM THE ARM: a bed can spend ticks
            loading, and a ramp begun at the arm would be over before anything
            sounded. Its launch placed is the moment - a launch is placed a
            horizon ahead, which the five ticks cover. A run already on its way
            out has nothing to come up for, and a fade somebody set going on it
            already owns its level (it starts from the silence the arm left). */
        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || ! run->deClickOwed || run->launchedAtSample <= 0 || run->launchRequested)
                continue;

            run->deClickOwed = false;

            if (run->isFinished() || run->stopAsked || run->stopIssued || run->takenBack)
                continue;

            const auto faded = std::any_of (running.begin(), running.end(),
                                            [run] (const FadeJob& job)
                                            {
                                                return ! job.retired && ! job.movesRate && job.target == run->id;
                                            });

            if (faded)
                continue;

            FadeJob job;
            job.target = run->id;
            job.reportsSelf = false;
            job.fromDb = run->ownLevel;
            job.toDb = run->deClickTo;

            /*  OVER THE PANIC FADE, a cue the GO had stopped and a Doh made
                again (2026-10-03, D3): what fades back in, not a de-click. */
            job.ticksTotal = run->arrivalTicks > 0 ? run->arrivalTicks : deClickTicks;
            job.curve = FadeCurve::linear;
            job.stopWhenDone = false;

            /*  AND ON INTO THE REST OF A FADE ITS SCENE HAD MOVING when a Doh
                paused it (D2, HG): the same job, once it has arrived. */
            job.then = run->arrivalThen;

            running.push_back (job);
        }
    }

    void Runner::assertPersistent (Engine& engine, std::int64_t tick)
    {
        /*  ONE PASS PER STEP, noticed by counting - the same rule the sweep
            uses, and for the same reason: a step is written by a handler and
            the pass is a decision, so a replay runs neither and re-injects the
            `run.assert` records from the log instead. */
        if (assertedFor != lists.stepsTaken())
        {
            assertedFor = lists.stepsTaken();
            assertDue = tick;
        }

        /*  WHAT A PASS OWES, paid on the tick the old run has ended (K8): a bed
            the step found still fading under the Esc that paused it. Dropped
            when its pause has gone - a double Esc, a load-to-time, a fire that
            made the cue's next run - and suspended rather than put back when
            somebody killed it on its way out (decision S). */
        for (auto due = owed.begin(); due != owed.end();)
        {
            const auto found = paused.find (*due);

            if (found == paused.end() || suspended.count (*due) > 0)
            {
                due = owed.erase (due);
                continue;
            }

            const auto* live = runs.liveRunOf (*due);

            if (live != nullptr && live->id == found->second.run)
            {
                ++due;
                continue;
            }

            if (live == nullptr)
            {
                if (killedSinceLift (*due))
                    suspended.insert (*due);
                else
                    submitAssert (engine, *due);
            }

            due = owed.erase (due);
        }

        if (assertDue < 0)
            return;

        const auto lists_ = document.root().getChildWithName (juce::Identifier ("Lists"));

        std::vector<Plan> plans;

        for (const auto& list : lists_)
        {
            if (list.getType().toString() != "List")
                continue;

            const auto listId = list[idProperty].toString().toStdString();

            if (listId.empty() || ! list.getChildWithName ("Persistent").isValid())
                continue;

            /*  The pointer, read straight off the list: it is one property,
                and every helper that wraps it lives in a file this one does
                not include. And beside it, whether an empty one means the end
                of the list rather than the top of it. */
            const auto standby = list[juce::Identifier ("standby")].toString().toStdString();
            /*  Read as the BOOLEAN the schema types it as: a `T` row lands in
                the tree as a bool var, whose `toString()` is "1" and not
                "true" - which is JUCE's answer and not this file's to argue
                with. The same conversion the enabled flag is read through. */
            const auto ranOut = static_cast<bool> (list[juce::Identifier ("finished")]);

            plans.push_back (solvePersistent (document, durations, mounts, listId, standby, ranOut));
        }

        if (plans.empty())
        {
            assertDue = -1;
            return;
        }

        /*  WAITING FOR THE SWEEP, but not for ever. Every value the section
            asserts on a desk that can be asked was asked about on the step's own
            tick; comparing before the answers land would be comparing against
            last second's world, and a desk that has gone quiet must not stop the
            section asserting at all. Twenty-five ticks is half a second. */
        if (tick - assertDue < 25 && mounts != nullptr)
            for (const auto& plan : plans)
                for (const auto& value : plan.values)
                {
                    const auto mountId = mounts->mountOf (value.address);
                    const auto* declaration = mounts->declarationOf (mountId);

                    if (declaration != nullptr && declaration->canBeAsked()
                         && mounts->observedAtTick (value.address) < assertDue)
                        return;
                }

        assertDue = -1;

        /*  THE PASS DOH!'S OWN STEP OPENED (2026-10-01, namespace draft §24,
            HN) re-asserts nothing on a device left to its operator: the Doh
            itself never sends such a device anything. Its OSC and MIDI cues
            were named by the handler; the next step's pass asserts as ever.

            AND NOTHING AT ALL AFTER AN ESC between the GO and the Doh (PRD
            §3.32: a Doh never undoes an Esc). Esc brought every bed down with
            the rest, and the Doh's step put them back at the press, before any
            GO - the Doh undoing the Esc it is not allowed to undo. The section
            comes back at the next step, as it does after any Esc. */
        const auto leftAlone = [this] (const std::string& cueId)
        {
            return assertedFor == persistentLeftAt && (persistentAllLeft || persistentLeft.count (cueId) > 0);
        };

        for (const auto& plan : plans)
        {
            for (const auto& planned : plan.runs)
            {
                if (suspended.find (planned.cue) != suspended.end() || leftAlone (planned.cue))
                    continue;

                const auto cue = document.findById (planned.cue);
                const auto kind = kindOfCue (cue);

                if (kind == "media" || kind == "mic")
                {
                    /*  STILL SOUNDING, and left alone - unless it is the run
                        Esc paused, still on its way out over the panic fade:
                        that one is owed, and put back when it has gone (K8). */
                    if (const auto* live = runs.liveRunOf (planned.cue))
                    {
                        if (const auto found = paused.find (planned.cue);
                            found != paused.end() && found->second.run == live->id)
                            owed.insert (planned.cue);

                        continue;
                    }

                    /*  A RUN THE OPERATOR KILLED SUSPENDS THE CUE for the
                        session (decision S). Read off the run rather than
                        remembered at the moment of killing, because a kill is a
                        record and this is a hook: what the handler wrote is
                        what a replay would have written too. */
                    if (killedSinceLift (planned.cue))
                    {
                        suspended.insert (planned.cue);
                        continue;
                    }

                    /*  AND A BED ESC PAUSED CARRIES ON FROM WHERE IT WAS (K8). */
                    submitAssert (engine, planned.cue);
                    continue;
                }

                if (kind != "midi")
                    continue;

                engine.submit (origin::engine, "run.assert",
                               { osc::Value::string (planned.cue) });
            }

            for (const auto& value : plan.values)
            {
                if (suspended.find (value.writer) != suspended.end() || leftAlone (value.writer))
                    continue;

                /*  WHAT THE DESK HOLDS, in the order §13.10 established: what it
                    was seen to hold, then what Go.dot wrote, and where neither
                    is known the value is sent - which is also the answer for a
                    mount that cannot be asked at all, every step, because
                    nothing about it is ever known. */
                if (mounts != nullptr)
                {
                    const auto* now = mounts->observedOf (value.address);

                    if (now == nullptr)
                        now = mounts->valueOf (value.address);

                    if (now != nullptr && *now == value.value)
                        continue;
                }

                engine.submit (origin::engine, "run.assert",
                               { osc::Value::string (value.writer) });
            }
        }
    }

    std::string Runner::listOfCue (const std::string& cueId) const
    {
        for (auto node = document.findById (cueId); node.isValid(); node = node.getParent())
            if (node.getType().toString() == "List")
                return node[idProperty].toString().toStdString();

        return {};
    }

    void Runner::switchOverride (const juce::ValueTree& cue, const std::string& runId)
    {
        const auto targetId = textOf (cue, "target");
        const auto target = document.findById (targetId);

        if (overrideDocument == nullptr || targetId.empty() || ! target.isValid())
            return;

        const auto on = textOf (cue, "verb") == "enable";
        const auto before = overrideWord (target);

        /*  WHERE A STANDBY ON IT GOES, asked BEFORE the switch, while the cue
            is still a place the walk can start from (PQ): the next stop that is
            neither the cue nor inside it - a group switched off takes its
            members with it - or nowhere at the end of the list, which is a
            pointer at rest rather than a list run out. No rounds: a manual
            group's wrap would keep the walk inside the group being left. */
        const auto listId = listOfCue (targetId);
        juce::ValueTree list;

        for (auto node = target; node.isValid(); node = node.getParent())
            if (node.getType().toString() == "List")
                list = node;

        const auto standby = list.isValid() ? list[juce::Identifier ("standby")].toString().toStdString()
                                            : std::string {};
        std::string away;
        auto mustLeave = false;

        if (! on && ! standby.empty())
        {
            const auto standing = document.findById (standby);
            mustLeave = standby == targetId || standing.isAChildOf (target);

            if (mustLeave)
            {
                auto from = standby;

                for (std::size_t guard = 0; guard < 100000; ++guard)
                {
                    const auto next = nextStandby (list, from);

                    if (next.empty() || next == from)
                        break;

                    if (next != targetId && ! document.findById (next).isAChildOf (target))
                    {
                        away = next;
                        break;
                    }

                    from = next;
                }
            }
        }

        overrideDocument->setOverride (targetId, on ? "on" : "off");

        /*  NOTED ON ITS GO, for Doh! (PS): the first switch of a cue under it
            only, so what is put back is the override as the GO found it. */
        if (const auto serial = goOfRun (runId); serial != 0 && serial == goRecord.serial)
        {
            const auto noted = std::any_of (goRecord.overridesBefore.begin(), goRecord.overridesBefore.end(),
                                            [&targetId] (const auto& entry) { return entry.first == targetId; });

            if (! noted && overrideWord (target) != before)
                goRecord.overridesBefore.emplace_back (targetId, before);
        }

        if (mustLeave && ! mayStandOn (list, standby))
            overrideDocument->setAttribute (standbyAddressOf (listId), away);
    }

    std::string Runner::jumpStandby (Engine& engine, doc::ShowDocument& editable, std::int64_t tick,
                                     const std::string& listId, const std::string& target,
                                     bool andGo, std::uint64_t cause,
                                     const std::vector<std::string>& supplied, std::vector<std::string>& made)
    {
        juce::ValueTree list;

        for (const auto& candidate : document.root().getChildWithName (juce::Identifier ("Lists")))
            if (candidate[idProperty].toString().toStdString() == listId)
                list = candidate;

        if (! list.isValid())
            return reason::notInList;

        /*  ITS OWN LIST ONLY (PT), so a Doh! has one pointer to put back; and
            where a park would land (§3.5): a header's, a footer's or a sampler
            member's cue on its group, a persistent or a disabled one nowhere. */
        if (! isInList (list, target))
            return reason::notInList;

        const auto landed = nearestStop (list, target);

        if (landed.empty())
            return reason::notAStop;

        /*  JUMP TO: a park the show makes, and nothing else (PM). */
        if (! andGo)
        {
            editable.setAttribute (standbyAddressOf (listId), landed);
            editable.setAttribute (finishedAddressOf (listId), "false");
            return {};
        }

        /*  AND GO: what GO does to the cue it now stands on - the pointer on past
            it first, as GO moves it before it fires, then the fire - under the
            GO that fired the jump cue (PN), so its runs carry that serial, Doh!
            takes them back with the rest, and the fire does not count as one
            after the GO. The step is a fire's, carrying its cause, as a start
            cue's is. */
        const auto next = standbyAfterFiring (list, landed, &runs);
        editable.setAttribute (standbyAddressOf (listId), next);
        editable.setAttribute (finishedAddressOf (listId), next.empty() ? "true" : "false");

        noteFireOnList (landed, origin::engine, cause);
        markFire (engine, tick, landed, origin::engine, cause);
        lists.stepped (listId, { tick, landed, 'f', cause });

        setFireCause (cause);
        made = fireStandby (engine, tick, list, landed, supplied);
        setFireCause (0);

        return {};
    }

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

        /*  READ FROM THE HISTORY WHEN THERE IS ONE (2026-09-19): a cue the
            list has fired is placed, with everything fired before it, by the
            clock the steps kept; a cue never fired is read from the order. */
        const auto plan = solveAim (document, durations, mounts,
                                    { listId, aim.cue, aim.offset }, &lists.historyOf (listId));

        if (! plan.ok)
            return used;

        /*  THE SWITCHES FOR THIS RUN, WORKED OUT AGAIN (namespace draft §27, PK): the
            marks this list could have made - on its own cues, and on what its
            enable and disable cues aim at, wherever that is - are taken away,
            and the plan's are put on: what the cues before the place said, in
            the order they said it. Before anything is built, so the seat below
            reads the switches of the place it is seating. */
        {
            const auto allLists = document.root().getChildWithName (juce::Identifier ("Lists"));
            std::set<std::string> cleared;

            std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& node)
            {
                for (const auto& child : node)
                {
                    if (const auto childId = child[idProperty].toString().toStdString(); ! childId.empty())
                    {
                        cleared.insert (childId);

                        if (child.getType().toString() == "Transport")
                            if (const auto verb = textOf (child, "verb"); verb == "enable" || verb == "disable")
                                cleared.insert (textOf (child, "target"));
                    }

                    collect (child);
                }
            };

            for (const auto& candidate : allLists)
                if (candidate[idProperty].toString().toStdString() == listId)
                    collect (candidate);

            for (const auto& clearedId : cleared)
                if (! clearedId.empty() && document.findById (clearedId).isValid())
                    editable.setOverride (clearedId, "file");

            for (const auto& [switchedId, on] : plan.switched)
                if (document.findById (switchedId).isValid())
                    editable.setOverride (switchedId, on ? "on" : "off");
        }

        /*  WHOSE LIST A RUN BELONGS TO, by climbing its cue to the top. A jump
            is scoped to one list (§13.5's cross-list rule read from the other
            side), so this is what tells the sweep below what it may end. */
        const auto listOf = [this] (const std::string& cueId) { return listOfCue (cueId); };

        /*  WHETHER A CUE SITS IN THE LIST'S PERSISTENT SECTION, at any depth.
            The section is outside the jump (§3.29): the solver never plans
            it, so a sweep that asked only "is this run in the list" ended every
            sounding bed - and a jump is not a step, so nothing asserted it again
            until the next GO (2026-09-26, namespace draft §18.8). What the
            section should be doing after a jump is the assertion's question at
            the next step, never this sweep's. */
        const auto inPersistent = [this] (const std::string& cueId)
        {
            for (auto node = document.findById (cueId); node.isValid(); node = node.getParent())
                if (node.getType().toString() == "Persistent")
                    return true;

            return false;
        };

        //----------------------------------------------------------------------
        /*  WHAT THE JUMP ABANDONS, ENDED BEFORE ANYTHING IS BUILT.

            Every run of THIS list outside its persistent section: the group
            runs and their jobs, the members under them, the armed run at the
            old standby. Ended the way `run.kill` ends one - the whole descent,
            and NO FOOTER, because a footer is arbitrary and need not be an
            inverse. Running one here would be arbitrary work fighting the
            values this is about to send, and one that blocks on a fade would
            make the jump wait for it.

            THE PLAN'S OWN CUES INCLUDED (2026-09-26). The build below makes
            every planned cue afresh - a jump hands `seatPlan` an empty map -
            so a run of a planned cue left standing here was the same cue
            twice: a playing one sounding on under its own relaunch, where
            §3.25 says a cue at the wrong offset is stopped and relaunched, or
            the old standby's armed run holding a voice the relaunch needed,
            which failed it `no-track` once the persistent section began
            keeping its voices through a jump. This asked "does the plan name
            it" until then, and the build never adopted what it kept.

            THE HANDLER DOES IT ITSELF rather than submitting, exactly as a
            revocation does (§13.6): the jump is one record, and a replay
            reaches the same state from the identifiers already in it.

            AND THE CLAIMS COME BACK IN THIS SAME DRAIN, which is why the sweep
            is before the build rather than after: a claim released a tick later
            would leave the plan's runs queued behind runs the jump had already
            ended. */
        /*  A MIC CUE THE PLAN STILL NAMES KEEPS SOUNDING (Phase 9b, namespace
            draft 18.5): a live input has no offset to be relaunched at, and
            ending it would shut its gate and ring its channel out under the
            relaunch that has to wait for the channel. So a sounding one at the
            top of the list, which the plan names, is kept - and handed to the
            build, which adopts it rather than making it again. */
        std::map<std::string, std::string> runFor;

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.kind != "mic" || snapshot.isFinished() || snapshot.parent.length() > 0
                  || snapshot.state != runState::playing || listOf (snapshot.cue) != listId)
                continue;

            const auto stillPlanned = std::any_of (plan.runs.begin(), plan.runs.end(),
                                              [&snapshot] (const PlannedRun& wants)
                                              {
                                                  return wants.cue == snapshot.cue && wants.ancestors.empty()
                                                           && wants.when == planned::sounding;
                                              });

            if (stillPlanned)
                runFor[snapshot.cue] = snapshot.id;
        }

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.isFinished() || listOf (snapshot.cue) != listId || inPersistent (snapshot.cue))
                continue;

            if (runFor.count (snapshot.cue) > 0 && runFor[snapshot.cue] == snapshot.id)
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
        /*  AND THE TREE, OUTERMOST FIRST - shared with a group re-seated at a
            second of its own timeline, which is the same building with the
            scene's run already standing. */
        seatPlan (engine, tick, plan.runs, runFor, nextId);

        //----------------------------------------------------------------------
        /*  AND THE VALUES: A MINIMAL CORRECTION, NOT A SHOTGUN BLAST (§3.13).

            WHAT IS COMPARED IS THE FRESHEST THING KNOWN ABOUT THE TARGET:
            what it was last OBSERVED to hold, and what Go.dot last WROTE where
            there is no observation. The order matters and is the whole of what
            §13.10 added here. A fader somebody moved by hand on the desk is in
            the observation and not in the write, so before this the jump agreed
            with a tree that disagreed with the room and sent nothing; now it
            sends. And an observation is dropped the moment Go.dot writes that
            address, so "no observation" means "nothing has been seen since we
            last wrote", where the written value is the best account there is.

            Sent through the ordinary write, so a replay reproduces it exactly
            and, having no sender, does not move the rig. */
        /*  STASHED, AND SENT BY THE HOOK (2026-10-03, Doh! D3, namespace draft
            §24.13): a handler never submits, and this one did - so a replay,
            which re-runs the handler AND re-injects the logged record, wrote
            every jump value twice. The flush on the next tick compares and
            sends, after that tick's give-backs - the old horizon's restores
            could otherwise land over a value the jump had just sent. */
        auto& values = stashFor (tick);

        for (const auto& value : plan.values)
            values.values.emplace_back (value.address, value.value);

        /*  WHERE IT LANDED, which is §3.13's second pointer. After a jump this
            agrees with the aim; after the next GO it does not, and that
            divergence is what a running view shows. */
        lists.landedAt (listId, { aim.cue, aim.offset });

        /*  AND THE HISTORY FOLLOWS THE JUMP: the steps the plan was read from
            move to the clock the rebuilt runs are on, and the steps after the
            instant - the ones the jump has undone - go. */
        if (plan.how == "history")
            lists.retimed (listId, plan.instant, tick);

        /*  AND A KILLED PERSISTENT CUE COMES BACK (decision S). A suspension is
            run-local and for the session, and a load-to-time is the operator
            asking the same question again with a new answer - so it is the one
            gesture that lifts one.

            AND A BED ESC PAUSED IS FORGOTTEN (K8, the author's ruling): a jump
            re-solves the world, and the section's solved place is the top of
            each cue - so the next step starts a paused bed from there. */
        suspended.clear();
        liftedAt = tick;
        paused.clear();
        owed.clear();

        return used;
    }

    void Runner::seatPlan (Engine& engine, std::int64_t tick,
                           const std::vector<PlannedRun>& wanted,
                           std::map<std::string, std::string>& runFor,
                           const std::function<std::string()>& nextId,
                           const SeatResume* resume)
    {
        /*  THE TREE, OUTERMOST FIRST.

            `RunTable::create` links a run into its parent's children only if
            the parent already exists, so a group made after its member would
            have a member it never heard of. The plan lists the target's
            ancestors outermost first for exactly this reason.

            A GROUP ALREADY IN `runFor` IS KEPT, NOT MADE AGAIN (2026-09-18):
            that is how a scene is re-seated at another second of itself - the
            scene's own run stands, its members are rebuilt under it - and how
            the groups above it are left alone. A jump hands in an empty map
            and every group is new, which is what it always did. */
        for (const auto& wants : wanted)
        {
            const auto cue = document.findById (wants.cue);

            if (! cue.isValid() || cue.getType().toString() != "Group")
                continue;

            const auto standing = runFor.find (wants.cue);
            const auto madeHere = standing == runFor.end();
            const auto id = madeHere ? nextId() : standing->second;

            if (madeHere)
            {
                const auto parent = wants.ancestors.empty()
                                      ? std::string {}
                                      : runFor[wants.ancestors.back()];

                createRun (id, wants.cue, "group", parent);
                runFor[wants.cue] = id;
            }

            auto* run = runs.find (id);

            if (run == nullptr)
                continue;

            run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
            run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

            /*  A GROUP THE INSTANT HAS PASSED, OR NOT REACHED (2026-09-18).
                An inner scene the plan found over is a done run, so the group
                above knows it has been played; one still to come waits with
                its due tick, and spawns its own members when it fires - which
                is why the solver planned none of them. */
            if (wants.when == planned::finished)
            {
                run->state = runState::done;
                run->endedAtTick = tick - retentionTicks - 1;
                continue;
            }

            if (wants.when == planned::due)
            {
                run->state = runState::waiting;
                run->dueTick = tick + ticksFor (wants.startsIn);
                continue;
            }

            /*  OVER, AND IN THE WAIT WRITTEN AFTER IT (K9's review, MO): its
                round's sequence goes on when the wait has run. */
            if (wants.when == planned::postWait)
            {
                run->state = runState::postWait;
                run->dueTick = tick + ticksFor (wants.startsIn);
                run->postWaitBegan = tick;
                continue;
            }

            /*  A STANDING SCENE ON ITS WAY OUT STAYS ON ITS WAY OUT (2026-10-02,
                K3's review, namespace draft §23.14, KT). A seek re-seats where
                the scene is, not whether it is ending: written `playing` over a
                scene a fade-and-stop was bringing down, it kept the fade's
                level and lost the stop - its new job never entered the
                stopping branch, and a fade lands nothing on a group itself -
                so the scene played on, silent, to its natural end, its network
                and MIDI members still firing. A stop asked of it stands until
                a handler that gives a run back withdraws it (`stopAsked`, a
                handler's account, so a replay seats the same state); the fade
                still ends it at its tick, and a stop already landed brings the
                re-seated members down at once. A seek of a media run withdraws
                the ask (HB), but its fade's job still lands its stop on the
                voice - the same end, by the road a cue has. */
            run->state = ! madeHere && run->stopAsked ? runState::stopping : runState::playing;

            /*  WHEN IT STARTED, AS IF IT HAD. `started` publishes this tick
                and `position` counts from it, so a scene seated `offset`
                seconds in reads `offset` seconds in - and a client scrubbing
                it sees the head land where the hand put it. */
            const auto launchedBefore = run->launchRequestedAtTick;
            run->launchRequestedAtTick = tick - ticksFor (wants.offset);

            /*  AND ITS ROUNDS MOVE WITH IT (K9, 2026-10-02, namespace draft
                §23.18): where the round in progress began, and the first, are
                kept against the scene's own start, so a later seek reads the
                round's second off the same clock the scene now reads. */
            if (! madeHere && run->iteration >= 1)
            {
                const auto moved = run->launchRequestedAtTick - launchedBefore;
                run->roundStartedAtTick += moved;
                run->firstRoundAtTick += moved;
            }

            /*  TWO OF THESE HAVE TO BE WRITTEN BY HAND AND IT IS NOT OBVIOUS
                WHICH. A run made by `create` leaves `iterations` at one and
                `seed` at nought; `iterations` is otherwise set when a group is
                FIRED and `seed` and `round` when a round is DRAWN, and a jump
                does neither. A group adopted without them ends after one round,
                or draws its next shuffle from a seed the show never used.

                AND THE ROUND IS THE ONE THE PLAN IS IN (J1, 2026-10-01,
                namespace draft §23.8; the author: "fix it, own commit"). It was
                seated at nought - no round begun - so the round a jump landed
                in was never counted: `endOfRound` found nought below the loop
                count and drew another, and a scene that plays once played every
                member again. And the `go` HANDLER reads the same count, through
                `heldForAnotherRound`, so on a manual group's last member it sent
                the pointer back to the group's first member instead of on past
                the group. The solver gives no round for a group - a looping one
                is `unknown-round`, and the round it says it took is the first -
                so a run made here is in round one.

                A RUN ALREADY STANDING IN A ROUND KEEPS IT, with the count and
                the seed it was fired with and the round it drew: the scene a
                seek re-seats at a second of itself is in the round it was in.
                Since K9 (2026-10-02, §23.18) that is any scene the machine
                paces that has begun a round - one that loops, shuffles, plays
                some of its members or has a header among them, sought within
                the round it is in - so what this keeps is the round it is in,
                as it was drawn, not only round one. A standing run that never began
                one - sought in its own pre-wait - is seated as a new one is.
                Handler state on both sides: a replay seats the same round.

                ONE SOUGHT IN THE VERY DRAIN ITS FIRST ROUND IS DRAWN IN is
                seated here in round one, and the `run.round` already on its way
                behind the seek counts one more: it reads round two of one, and,
                playing once, still ends after this round. Its members spawned
                in that same drain are the older fault beside it - they are made
                a second time under the scene the seek has just seated.
                (K9's review, 2026-10-03, MS: no longer - `seekGroup` marks the
                scene re-seated in this tick, and the `run.round` and the
                spawns its old job decided are applied and ignored.) */
            if (madeHere || run->iteration < 1)
            {
                run->iterations = static_cast<int> (numberOf (cue, "loops"));
                run->seed = static_cast<std::int32_t> (numberOf (cue, "seed"));
                run->round = membersOf (cue);

                /*  THE ROUND THE PLAN SAYS (K9's review, MN): one, unless the
                    plan wrapped a scene's second into a later round of it. */
                run->iteration = std::max (1, wants.round);

                /*  Its round began when its pre-wait ended, as the seat dates
                    it (K9): the seconds a seated scene's offset counts include
                    its own pre-wait, the walk's convention - or where the plan
                    says a later round began. */
                run->firstRoundAtTick = run->launchRequestedAtTick + run->preWaitTicks;
                run->roundStartedAtTick = wants.roundFrom >= 0.0
                                            ? run->launchRequestedAtTick + ticksFor (wants.roundFrom)
                                            : run->firstRoundAtTick;
            }

            /*  A SCENE DOH! CARRIES ON is seated at the level it had at the
                press (D2, HG): a fresh run is at nought, and a scene a fade from
                outside had dipped would come back up. A level is a job's, so a
                handler may write it. */
            if (resume != nullptr)
                if (const auto level = resume->levels.find (wants.cue); level != resume->levels.end())
                    run->ownLevel = level->second;
        }

        //----------------------------------------------------------------------
        /*  THEN THE CUES THAT MAKE A SOUND, each under the group it belongs to. */
        for (const auto& wants : wanted)
        {
            const auto cue = document.findById (wants.cue);

            if (! cue.isValid() || cue.getType().toString() == "Group")
                continue;

            /*  A RUN ALREADY STANDING FOR IT - a mic cue kept sounding through
                a jump - is adopted as it is. */
            if (runFor.count (wants.cue) > 0)
                continue;

            const auto id = nextId();
            const auto parent = wants.ancestors.empty()
                                  ? std::string {}
                                  : runFor[wants.ancestors.back()];

            createRun (id, wants.cue, kindOfCue (cue), parent);
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
                /*  AND NEVER PUBLISHED: it exists for its group's bookkeeping
                    and nothing sounded here, so the seconds a finished run is
                    kept on the tree for reading would show a row of "done"
                    for every member behind every step of a scrub (the first
                    live one, 2026-09-18). The table keeps it; the tree does
                    not say it. */
                run->state = runState::done;
                run->endedAtTick = tick - retentionTicks - 1;
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

            /*  OVER, AND IN THE WAIT WRITTEN AFTER IT (K9's review, MO): its
                sequence moves on, or its round ends, when the wait has run -
                `run.done` at the deadline, as for any post-wait. */
            if (wants.when == planned::postWait)
            {
                run->state = runState::postWait;
                run->dueTick = tick + ticksFor (wants.startsIn);
                run->postWaitBegan = tick;
                continue;
            }

            /*  AND THE ONES MAKING A NOISE. Armed with their launch asked for
                and their offset on them: the arm applies it as the clip's own
                offset, which M17 measured landing on the sample. */
            run->startOffset = wants.offset;
            run->startRange = std::max (wants.range, 0);

            /*  A CUE WITH RANGES AT ITS PHASE (2026-10-05, namespace draft
                §30.6, S8). The solver says which range, which pass and where in
                it (`placeInRanges`), and the plan's offset is that second of the
                file - but the audio side takes an offset for a whole file and a
                slot for a range, not both, so a seat that handed over the
                offset alone started the range at its in-point, on its first
                pass: every scrub of a scene restarted its looping members, and
                a jump into one did the same. So the slice is entered part-way,
                by K8's `sliceFrom` - the passes before it and the point inside
                it, as `seekMedia` enters one since S2 - and the launch dates the
                slice's start back by all of it, so the pass count and the
                playhead read on from there. A range that plays for ever is
                placed at its start, pass one, by the solver's own rule (§3.24),
                and stays so. Doh! carries on from what it read below. */
            if (wants.range >= 0 && resume == nullptr)
            {
                const auto ranges = rangesOf (cue);

                if (static_cast<std::size_t> (wants.range) < ranges.size())
                {
                    const auto& slice = ranges[static_cast<std::size_t> (wants.range)];
                    const auto length = slice.out - slice.in;
                    const auto into = wants.offset - slice.in;

                    /*  Only a point the solver placed inside the slice: past
                        every range it hands back where the cue had got to, not
                        a second of the file, and that is the in-point's. */
                    if (length > 0.0 && into >= 0.0 && into < length)
                        run->sliceFrom = into + static_cast<double> (std::max (wants.pass - 1, 0)) * length;
                }
            }

            /*  CARRIED ON BY DOH! (D2, namespace draft §24.12): arriving over the de-click at the
                level it had at the press, and on into the rest of a fade its
                scene had moving then; inside the slice it was in, at the point
                its playhead had reached (K8's `sliceFrom`); and never at
                nought, which on a run means "where the cue says". A mic takes
                the de-click as its gate's fade-in. Written before the arm,
                which makes the arrival the run's own. */
            if (resume != nullptr)
            {
                run->resumes = true;

                /*  AND OVER THE PANIC FADE, a cue the GO had stopped (D3). */
                run->arrivalTicks = resume->arrivalTicks;

                if (! cue.hasType ("Mic"))
                {
                    run->deClick = true;

                    if (const auto level = resume->levels.find (wants.cue); level != resume->levels.end())
                        run->arrivalDb = level->second;

                    if (const auto rest = resume->carryOn.find (wants.cue); rest != resume->carryOn.end())
                        run->arrivalThen = rest->second;

                    if (const auto into = resume->sliceFrom.find (wants.cue);
                        into != resume->sliceFrom.end() && wants.range >= 0)
                        run->sliceFrom = into->second;

                    if (wants.range < 0)
                        run->startOffset = std::max (wants.offset, 0.001);
                }
            }

            run->armedOrigin = run->startOffset > 0.0 ? run->startOffset : numberOf (cue, "startOffset");
            run->launchRequested = true;
            run->launchRequestedAtTick = tick;

            if (cue.hasType ("Mic"))
                armMic (engine, cue, id);
            else
                armMedia (engine, cue, id);

            /*  A mic seated where it was (D2): its output level as it was at
                the press, after the arm has set its cue's. */
            if (resume != nullptr && cue.hasType ("Mic"))
                if (auto* mic = runs.find (id))
                    if (const auto level = resume->levels.find (wants.cue); level != resume->levels.end())
                        mic->ownLevel = level->second;
        }

        //----------------------------------------------------------------------
        /*  AND THE JOBS THAT WILL CARRY IT ON.

            The scheduler continues from here on the next tick, because a group
            job re-reads the round from the run every tick rather than keeping a
            copy - PR 3.5's finding, built so a prune could reach the round in
            progress, and paying again here.

            The job's own list of runs it has taken charge of is filled at this
            moment too, because the loop that claims a child on sight does not
            run on a job's first tick.

            ONLY FOR A GROUP THAT IS PLAYING: a scene found over has nothing to
            carry on, and one still to come is fired by its due tick, which
            makes its job then. */
        for (const auto& wants : wanted)
        {
            if (wants.when != planned::sounding)
                continue;

            const auto found = runFor.find (wants.cue);

            if (found == runFor.end())
                continue;

            const auto cue = document.findById (wants.cue);

            if (! cue.isValid() || cue.getType().toString() != "Group")
                continue;

            GroupJob job;
            job.run = found->second;
            job.phase = groupPhase::members;

            /*  THE ROUND IT IS IN, not the group's members as written (K9,
                2026-10-02, namespace draft §23.18): a scene a seek re-seats in
                its second round of a shuffle plays that round's order, and a
                "play two of five" that round's two. A run the seat made holds
                the members as written, which is the round it wrote. */
            const auto* seated = runs.find (job.run);
            job.phaseCues = seated != nullptr && seated->iteration > 0 ? seated->round
                                                                       : membersOf (cue);
            job.nextMember = job.phaseCues.size();

            /*  THE CHILDREN THIS SEAT MADE ARE THE ROUND'S (K9). A scene a seek
                re-seats keeps the runs of what it played before - ended, but
                still its children - and a round counts its own: taken, so no
                phase adopts one again, but not the runs the round waits on. */
            const auto madeBySeat = [&runFor] (const Run& child)
            {
                const auto mine = runFor.find (child.cue);
                return mine != runFor.end() && mine->second == child.id;
            };

            const Run* firstDue = nullptr;
            const Run* newestOver = nullptr;
            auto newestOverAt = std::size_t { 0 };
            auto lastSeated = std::size_t { 0 };
            auto anySeated = false;

            for (const auto* child : runs.childrenOf (job.run))
            {
                job.taken.push_back (child->id);

                if (! madeBySeat (*child))
                    continue;

                /*  ITS MEMBERS, AND ONLY THEM (2026-10-02, Doh! D2, namespace
                    draft §24.12): a scene Doh! carries on seats what its
                    header had fired too - sent again from their start - and a
                    header's run counted among the members the round waits on
                    would be waited for as one, or launched as one. Taken, as
                    every child is. */
                const auto at = std::find (job.phaseCues.begin(), job.phaseCues.end(), child->cue);

                if (at == job.phaseCues.end())
                    continue;

                const auto index = static_cast<std::size_t> (at - job.phaseCues.begin());

                job.phaseRuns.push_back (child->id);

                if (! child->isFinished() && child->state != runState::waiting)
                    job.awaiting = child->id;

                if (child->state == runState::waiting
                     && (firstDue == nullptr || child->dueTick < firstDue->dueTick))
                    firstDue = child;

                if (child->isFinished() && (newestOver == nullptr || index >= newestOverAt))
                {
                    newestOver = child;
                    newestOverAt = index;
                }

                lastSeated = std::max (lastSeated, index + 1);
                anySeated = true;
            }

            job.launched = job.phaseRuns.size();

            /*  A SCENE THE MACHINE PACES SEATED WITH NONE OF ITS MEMBERS (K9's
                review, 2026-10-03, MP): nothing in the plan placed them - their
                lengths unknown, a scene the walk could not reach - and a job in
                its members phase over no members waited for ever, a timeline
                spawning only when a round begins and a sequence only after a
                member it awaits. So it begins its round from the top: entered,
                its header passed over as already run, its round drawn as a GO
                draws one. Counted from nought, so the round drawn is its
                first. Handler state; the job is the scheduler's. Not for a
                scene Doh! carries on, which seats what it had as D2 says. */
            if (job.phaseRuns.empty() && roundSolvable (cue) && resume == nullptr)
            {
                if (auto* bare = runs.find (job.run))
                {
                    bare->iteration = 0;
                    bare->round.clear();
                }

                job.phase = groupPhase::entering;
                job.phaseCues.clear();
                job.prepared = membersOf (cue.getChildWithName ("Header"));
                scheduled.push_back (job);
                continue;
            }

            /*  A SEQUENCE ADVANCES ON THE MEMBER IT IS WAITING FOR, so
                `nextMember` is where the plan left off rather than the end of
                the list - otherwise the chain would stop at the jump.

                AND WHERE THE PLAN LEFT OFF IS ITS LAST MEMBER SEATED, not the
                one after the member sounding (K9). The solver seats every
                member of a timed sequence - over, sounding, or due at its
                second - and one counted on from the member sounding spawned
                the next member again when its turn came, beside the run seated
                due: it played twice (§23.8, "Not changed"). And a seat that
                lands between two members - in a post-wait, in the next one's
                pre-wait - has nothing sounding to wait on, so it waits on the
                member due first; with nothing awaited it waited for an armed
                member nothing would arm, for ever. */
            if (textOf (cue, "mode") != "timeline")
            {
                if (job.awaiting.empty() && firstDue != nullptr)
                    job.awaiting = firstDue->id;

                /*  AND WITH NOTHING UNFINISHED AT ALL - seated in the tick
                    between one member's end and the next one's spawn - IT WAITS
                    ON THE NEWEST MEMBER OVER (D2, namespace draft §24.12): that run has finished,
                    so on its first tick the sequence advances and spawns the
                    member after it. Awaiting nothing, it went looking for an
                    armed member to launch, found none, and held for ever. */
                if (job.awaiting.empty() && newestOver != nullptr)
                    job.awaiting = newestOver->id;

                const auto* awaited = runs.find (job.awaiting);
                const auto at = awaited != nullptr
                                  ? std::find (job.phaseCues.begin(), job.phaseCues.end(),
                                               awaited->cue)
                                  : job.phaseCues.end();

                job.nextMember = at != job.phaseCues.end()
                                   ? static_cast<std::size_t> (at - job.phaseCues.begin()) + 1
                                   : job.phaseCues.size();

                if (anySeated)
                    job.nextMember = std::max (job.nextMember, lastSeated);
            }

            scheduled.push_back (job);
        }
    }

    //==============================================================================
    bool Runner::seekableNow (const Run& run) const
    {
        if (run.isFinished())
            return false;

        /*  A SOUND: what `seekMedia` takes - not one Doh! took back, which is
            coming down under the Doh's fade. Whether there is a head to drag
            yet (launched, a length to drag against) is the pane's to see. */
        if (run.kind == "media")
            return ! run.takenBack && document.findById (run.cue).isValid();

        /*  AN OSC CUE WHOSE CURVES ARE PLAYING (namespace draft 45): its clock
            moves where it is sought - not one Doh! took back. */
        if (run.kind == "osc")
            return ! run.takenBack && isCurving (run.id);

        /*  NOR A SCENE DOH! TOOK BACK (K9's review, MV), coming down under
            the Doh's fade as a sound it took back is. */
        if (run.kind != "group" || run.takenBack)
            return false;

        const auto group = document.findById (run.cue);

        /*  THE MACHINE PACES IT, or there is no second to seek to: a manual
            group has a person between its members, and a sampler bank is
            played by hand. A scene the walk times as written can be seated
            at any second, in its own pre-wait too (HV); any other the machine
            paces, once it has begun a round - which its header, if it has
            one, has finished by then (K9, the author's ruling). */
        if (! roundSolvable (group))
            return false;

        /*  IN A LIST, climbed from the node in hand rather than asked of the
            show again (K9's review, MW: this is asked every tick). */
        auto inList = false;

        for (auto up = group.getParent(); up.isValid(); up = up.getParent())
            if (up.getType().toString() == "List")
            {
                inList = true;
                break;
            }

        if (! inList)
            return false;

        /*  NOR WHILE ITS FOOTER RUNS (K9's review, MU): its members are over
            and its release has begun. A seek ended the footer's cues and
            seated the round again, and the footer ran twice. A footer cue's
            run is the scene's child from the spawn that made it, so the run
            table says so. */
        for (const auto& child : run.children)
            if (const auto* below = runs.find (child); below != nullptr && isFooterCueOf (run, below->cue))
                return false;

        return seeksAsWritten (run, group) || (run.iteration >= 1 && ! run.round.empty());
    }

    bool Runner::seeksAsWritten (const Run& run, const juce::ValueTree& group) const
    {
        /*  WHAT THE RUN WAS FIRED WITH, not what the show says now (K9's
            review, MX): one round, and - once it has drawn it - that round the
            members as written, in order. A scene fired to loop twice and edited
            to once while it plays still plays two rounds, and a seek on it
            counts across them from the top only if this says so; asked of the
            document alone, its second was read as the walk's, past the end of
            the one round the edited show has. The shape the walk needs (no
            header, every member, in order) is still the document's. */
        if (! walkTimes (group) || run.iterations != 1)
            return false;

        return run.iteration < 1 || run.round == membersOf (group);
    }

    void Runner::mirrorSeekable()
    {
        /*  Written only where it changes, and asked only of a run that is
            playing - the only state a client offers the scrub in (K9's
            review, MW): a finished run, or one waiting or armed, reads false
            without a walk of the show, every tick. */
        for (const auto& snapshot : runs.all())
        {
            const auto now = snapshot.state == runState::playing && seekableNow (snapshot);

            if (now != snapshot.seekable)
                if (auto* run = runs.find (snapshot.id))
                    run->seekable = now;
        }
    }

    void Runner::mirrorRound()
    {
        /*  THE ROUND A SCRUB IS DRAWN OVER AND HELD TO (2026-10-05, namespace
            draft §30.6, S8). Until today a scene's strip was geared to the
            longest file among the rows on screen, or to a minute, and its ghost
            head was drawn against that and clamped to nothing - while the seek
            it sent was clamped to the round the scene is in (K9's LW). In a
            looping scene's second round the hand dragged a head pinned at the
            right edge, since `position` counts across every round, and let go
            somewhere the engine then moved to the round's start or end.

            So the engine says what it honours: for a scene a seek would move
            now (`seekable`, which this follows), the second of its own clock
            its round began at - `roundStartedAtTick`, the handler's stamp,
            against the start `position` counts from - and the round's length,
            the furthest end of its members and the wait after it, exactly as
            `seekGroup` clamps (`solveRound`, MO). A scene that plays once as
            written is one round, begun when its pre-wait ended.

            SOLVED ONCE A ROUND, since `solveRound` walks a copy of the scene:
            again only when the round, its start, the scene's own start (a seek
            re-dates it) or the show changes - the show's own half
            (`showRevision`), so a GO moving the pointer solves nothing again.
            Nought when nothing can be solved
            - a member's length unknown, the scene not one a seek would move -
            and the client then falls back to its old gearing. A readout: never
            logged, nought through a replay, which runs no hooks. */
        const auto revision = document.showRevision();

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.kind != "group")
                continue;

            auto* run = runs.find (snapshot.id);

            if (run == nullptr)
                continue;

            if (! run->seekable || run->iteration < 1)
            {
                run->roundFrom = 0.0;
                run->roundLength = 0.0;
                run->roundSolvedAt = -1;
                continue;
            }

            if (run->roundSolvedAt == run->roundStartedAtTick
                 && run->roundSolvedLaunch == run->launchRequestedAtTick
                 && run->roundSolvedIteration == run->iteration
                 && run->roundSolvedRevision == revision)
                continue;

            run->roundSolvedAt = run->roundStartedAtTick;
            run->roundSolvedLaunch = run->launchRequestedAtTick;
            run->roundSolvedIteration = run->iteration;
            run->roundSolvedRevision = revision;

            const auto group = document.findById (run->cue);
            const auto round = run->round.empty() ? membersOf (group) : run->round;
            const auto solved = solveRound (document, durations, group, round, 0.0);

            run->roundFrom = static_cast<double> (std::max (std::int64_t { 0 },
                                                            run->roundStartedAtTick - run->launchRequestedAtTick))
                               / static_cast<double> (TickClock::rateHz);
            run->roundLength = solved.length.has_value() && *solved.length > 0.0 ? *solved.length : 0.0;
        }
    }

    bool Runner::seekMedia (Engine& engine, std::int64_t tick, const std::string& runId,
                            double seconds, bool keepPass)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished() || run->kind != "media")
            return false;

        /*  NOT ONE DOH! TOOK BACK (2026-10-01): it is coming down over the
            Doh fade, and a seek would launch it again under that fade. */
        if (run->takenBack)
            return false;

        const auto cue = document.findById (run->cue);

        if (! cue.isValid())
            return false;

        /*  A CUE WITH RANGES IS A PLAYLIST OVER ONE FILE (§3.24), and the audio
            side takes an offset for a whole file and a slot for a range, not
            both - so a second inside a range is that range's slot entered
            part-way, by K8's `sliceFrom` (§23.17): the seconds of the slice
            played, passes and all. The arm starts the clip that far into its
            loop, and the launch dates the slice's start back by it, so the
            pass count, the playhead, the lane and the boundary all read on
            from there.

            UNTIL 2026-10-05 THE SECOND LANDED AT THE START OF THE RANGE THAT
            HELD IT, and a second no range held - in a gap, or past the end -
            at the first range's start, the top of the file: a lane recorded
            from 21.5 s started at the range's in-point, and a scrub across a
            looping range snapped back to it on every move (namespace draft
            §30, items 3 and 10). Now (§30.4):
            - inside a range, at that second: in the range the run was already
              in, on the pass it was on when `keepPass` asks (a scrub); on the
              range's first pass otherwise (a restart, a lane's pass);
            - in a gap between ranges, or before the first, at the start of
              the next range to begin;
            - at or past the last out-point, a hair inside the range that ends
              last (RH), so a seek past the end is the end, never the top. */
        const auto ranges = rangesOf (cue);
        const auto count = static_cast<int> (ranges.size());
        auto range = -1;
        auto at = seconds;
        auto into = 0.0;
        auto passKept = 0;

        if (count > 0)
        {
            const auto sliceAt = [&ranges] (int index) -> const RangeSpec&
            {
                return ranges[static_cast<std::size_t> (index)];
            };

            for (auto index = 0; index < count && range < 0; ++index)
                if (seconds >= sliceAt (index).in && seconds < sliceAt (index).out)
                    range = index;

            if (range < 0)
            {
                auto next = -1;
                auto last = 0;

                for (auto index = 0; index < count; ++index)
                {
                    if (sliceAt (index).in > seconds && (next < 0 || sliceAt (index).in < sliceAt (next).in))
                        next = index;

                    if (sliceAt (index).out > sliceAt (last).out)
                        last = index;
                }

                range = next >= 0 ? next : last;
                at = next >= 0 ? sliceAt (next).in
                               : std::max (sliceAt (last).in, sliceAt (last).out - seekHairSeconds);
            }

            const auto& slice = sliceAt (range);
            const auto length = slice.out - slice.in;

            into = std::max (0.0, at - slice.in);

            /*  THE PASS IT WAS ON, kept for a scrub inside the range it is in -
                the playhead's own count, a readout (`advanceRanges`). It only
                places the voice: no record carries it or depends on it, so a
                replay, whose count stays at the first pass, logs the same. A
                loop count lowered since keeps the point and gives it its last
                pass, as K8's resume does. */
            if (keepPass && range == run->range && run->rangeIteration > 1 && length > 0.0)
            {
                passKept = run->rangeIteration - 1;

                if (slice.loops > 0)
                    passKept = std::min (passKept, slice.loops - 1);

                into += static_cast<double> (passKept) * length;
            }
        }

        /*  THE VOICE IS STOPPED AND ASKED FOR AGAIN AT THE NEW SECOND, on the
            same track, at the level it was playing at - a fade that has
            brought it down stays down. The edge watcher would read the stop
            as the cue ending, so what it remembers is cleared: a run that
            has not been seen sounding gives no falling edge, and the launch
            that follows the arm is what it sees next.

            THE FLOOR IS A MILLISECOND, because nought is what "wherever the
            cue says" is spelled as on a run, and a scrub to the very top of
            the file is a decision about this run and not a return to the
            cue's own offset. */
        if (audio != nullptr && run->track >= 0)
            audio->stop (run->track);

        run->startOffset = range >= 0 ? 0.0 : std::max (seconds, 0.001);
        run->startRange = std::max (range, 0);
        run->sliceFrom = range >= 0 ? into : 0.0;
        run->armedOrigin = run->startOffset;
        run->positionOrigin = range >= 0 ? ranges[static_cast<std::size_t> (range)].in : seconds;
        run->position = at;
        run->range = range;
        run->rangeIteration = range >= 0 ? passKept + 1 : 0;
        run->rangeStartedAtSample = 0;
        run->rangesFinished = false;
        run->boundaryPlacedAt = -1;
        run->advanceRequested = false;
        run->sawPlaying = false;
        run->stopIssued = false;
        run->killIssued = false;
        run->armConfirmed = false;
        run->launchedAtSample = 0;
        run->launchRequested = true;
        run->launchRequestedAtTick = tick;

        /*  AND A SEEK IS ASKING FOR IT (2026-09-30, namespace draft §23.3): it
            launches the run, so the standby's arm scrubbed stops being a
            preparation, as a GO or a fire by name makes it stop. It was the one
            road to a launch that left the mark on - and Esc, which spares what
            was made ready and not asked for, would have spared a cue that was
            playing. */
        run->prepare.clear();

        /*  AND IT WITHDRAWS ANY STOP ASKED OF IT (2026-10-01, namespace draft
            §24, HB): the hand put the cue somewhere and wants it there. Cleared
            whatever `state` says - a hook can hand a stopped run back to
            `playing` with no record, so the state is no account of what was
            asked; this is the account, and it is kept by handlers alone. */
        run->stopAsked = false;
        run->stopAskedBy = 0;
        run->askedAgain = false;
        run->stopEndsWait = false;
        run->postWaitBegan = -1;

        if (run->state == runState::stopping)
            run->state = runState::playing;

        if (audio != nullptr && run->track >= 0)
            requestArmOn (engine, cue, *run, run->ownLevel);

        return true;
    }

    std::string Runner::startCurvePass (Engine& engine, std::int64_t tick, const std::string& cueId,
                                        double from, const std::string& runId)
    {
        if (! document.findById (cueId).isValid())
            return {};

        const auto made = fire (engine, tick, cueId, runId);

        if (made.empty())
            return made;

        //  FROM `from`: the clock of the run the fire made, or found playing.
        if (from > 0.0 && isCurving (made))
            seekCurves (made, from, tick);

        return made;
    }

    tree::MountListener::Wanted Runner::listenWanted() const
    {
        tree::MountListener::Wanted out;

        if (curveTable == nullptr || mounts == nullptr || ! curveTable->armed() || document.isLocked())
            return out;

        for (const auto& id : curveTable->armedCurves())
        {
            const auto curve = document.findById (id);

            if (! curve.isValid())
                continue;

            //  A curve's message is its parent: a further one, or the cue's own.
            const auto address = curve.getParent()[juce::Identifier ("address")].toString().toStdString();
            const auto mountId = address.empty() ? std::string {} : mounts->mountOf (address);
            const auto* declaration = mountId.empty() ? nullptr : mounts->declarationOf (mountId);

            if (declaration == nullptr || ! declaration->rx || ! declaration->canBeAsked())
                continue;

            auto& device = out[mountId];
            device.host = declaration->host;
            device.queryPort = declaration->queryPort;
            device.addresses.insert (address);
        }

        return out;
    }

    void Runner::recordCurves (Engine& engine, std::int64_t tick)
    {
        if (curveTable == nullptr || ! curveTable->armed())
            return;

        /*  THE LOCK KEEPS THE SHOW, and the curves a pass ends in are the show's:
            under it, an arming with no pass is let go of. */
        if (! curveTable->recording)
        {
            curvePassEnded.clear();

            if (document.isLocked())
                engine.submit (origin::engine, "curve.free", {});

            return;
        }

        if (curveTable->run.empty() || curveTable->run == curvePassEnded)
            return;

        const auto* run = runs.find (curveTable->run);
        const auto* job = static_cast<const CurveJob*> (nullptr);

        for (const auto& candidate : curving)
            if (candidate.self == curveTable->run)
                job = &candidate;

        /*  DROPPED: a kill or a Doh! took the run - what was ridden goes with it
            (ZL, as the faders' DM). */
        if (run == nullptr || run->takenBack || beingKilled (*run) || run->skipFooter)
        {
            curvePassEnded = curveTable->run;
            engine.submit (origin::engine, "curve.stop", { osc::Value::string ("dropped") });
            return;
        }

        const auto ending = curveTable->stopping || run->isFinished() || run->state == runState::stopping;

        //  SAMPLED, while it plays: each armed curve from its first value heard or ridden.
        if (! ending && job != nullptr)
        {
            auto place = placeOnCurves (job->secondsAt (tick), job->duration, job->loop);

            if (! (job->duration > 0.0) && ! job->loop)
                place.seconds = std::max (0.0, job->secondsAt (tick));

            for (const auto& target : job->targets)
                for (const auto& lane : target.lanes)
                {
                    if (! curveTable->isArmed (lane.id))
                        continue;

                    auto& ride = curveTable->rideOf (lane.id);

                    /*  THE DEVICE'S NEWEST REPORT of this address, heard since the
                        pass began (O.8) - never a read-back sweep's, which may
                        be asking after what Go.dot itself sent. */
                    if (mounts != nullptr)
                        if (const auto* said = mounts->observedOf (target.address))
                        {
                            const auto when = mounts->heardAtTick (target.address);

                            /*  NOT GO.DOT'S OWN VALUE COMING BACK (O.10, mine): a
                                device that reports every change - a motor fader,
                                a server that pushes to every listener - sends the
                                curve's value straight back while it plays, and
                                that is no hand. Before a curve is latched, a
                                report of exactly what was last sent is passed
                                over; once latched, every report counts. */
                            const auto echo = ! ride.latched && lane.arg < target.written.size()
                                                && lane.arg < said->size() && (*said)[lane.arg].isNumber()
                                                && target.written[lane.arg].isNumber()
                                                && std::abs ((*said)[lane.arg].asDouble() - target.written[lane.arg].asDouble())
                                                     <= 1.0e-6 * std::max (1.0, std::abs (target.written[lane.arg].asDouble()));

                            if (echo && when > ride.sampledTick)
                                ride.sampledTick = when;

                            if (! echo && when >= curveTable->startTick && when > ride.sampledTick
                                  && lane.arg < said->size() && (*said)[lane.arg].isNumber())
                            {
                                ride.value = (*said)[lane.arg].asDouble();
                                ride.source = "heard";
                                ride.sampledTick = when;
                                ride.latched = true;
                            }
                        }

                    //  A HAND'S RIDE, since the last sample (O.11's `curve.ride`).
                    if (ride.source == "hand" && ride.lastTick > ride.sampledTick)
                    {
                        ride.sampledTick = ride.lastTick;
                        ride.latched = true;
                    }

                    /*  UNTIL SOMETHING LATCHES IT, a curve rides its own drawing
                        where the clock is - what `ride` shows, and where a push
                        of the puck starts from. */
                    if (! ride.latched)
                    {
                        const auto drawn = valuesAt (target, place.seconds);

                        if (lane.arg < drawn.size() && drawn[lane.arg].isNumber())
                            ride.value = drawn[lane.arg].asDouble();
                    }

                    //  LATCHED: written every tick until the pass stops, the last value held (DH).
                    if (ride.latched)
                        appendRide (ride.segments, place.seconds, ride.value);
                }

            return;
        }

        if (! ending)
            return;

        curvePassEnded = curveTable->run;

        /*  THE END, ONE STEP OF UNDO (ZL): each armed curve that was moved,
            spliced into the curve it rode over, thinned to its own scale (ZG),
            judged - every curve or none - and written in one `node.setMany`. */
        if (document.isLocked())
        {
            engine.submit (origin::engine, "curve.stop", { osc::Value::string ("locked") });
            return;
        }

        std::vector<osc::Value> writes;
        std::string written;
        std::int32_t points = 0;
        auto refused = false;

        if (job != nullptr)
            for (const auto& target : job->targets)
                for (const auto& lane : target.lanes)
                {
                    const auto ridden = curveTable->rides.find (lane.id);

                    if (! curveTable->isArmed (lane.id) || ridden == curveTable->rides.end()
                          || ridden->second.segments.empty())
                        continue;

                    const auto base = "/godot/curve/" + lane.id + "/";
                    std::string problem;
                    const auto range = doc::readLaneRange (document.getAttribute (base + "range")
                                                             .value_or (std::string {}), problem);

                    /*  THE SCALE: the curve's range, else the device's for this
                        value, else what was ridden. */
                    auto span = 0.0;

                    if (range.has_value())
                        span = range->high - range->low;
                    else if (const auto* node = mounts != nullptr ? mounts->nodeAt (target.address) : nullptr;
                             node != nullptr && node->rangeOf (lane.arg).hasMinimum && node->rangeOf (lane.arg).hasMaximum)
                        span = node->rangeOf (lane.arg).maximum - node->rangeOf (lane.arg).minimum;
                    else
                    {
                        auto low = ridden->second.segments.front().front().levelDb, high = low;

                        for (const auto& segment : ridden->second.segments)
                            for (const auto& point : segment)
                            {
                                low = std::min (low, point.levelDb);
                                high = std::max (high, point.levelDb);
                            }

                        span = high - low;
                    }

                    const auto tolerance = curveTolerance (osc::parseDouble (document.getAttribute (base + "tolerance")
                                                                                 .value_or (std::string {})).value_or (0.0),
                                                           span);
                    const auto curve = spliceCurve (lane.points, ridden->second.segments, 0.05, tolerance);
                    const auto text = curveText (curve, curveStep (tolerance));
                    const auto judged = doc::readLane (text, range);

                    if (! judged.problem.empty())
                    {
                        refused = true;
                        continue;
                    }

                    writes.push_back (osc::Value::string (base + "points"));
                    writes.push_back (osc::Value::string (text));
                    points += static_cast<std::int32_t> (judged.points.size());
                    written += (written.empty() ? "" : " ") + lane.id;
                }

        if (refused)
            engine.submit (origin::engine, "curve.stop", { osc::Value::string ("dropped") });
        else if (writes.empty())
            engine.submit (origin::engine, "curve.stop", { osc::Value::string ("untouched") });
        else
        {
            engine.submit (origin::engine, "node.setMany", std::move (writes));
            engine.submit (origin::engine, "curve.stop",
                           { osc::Value::string ("kept"), osc::Value::int32 (points), osc::Value::string (written) });
        }

        //  A HAND THAT ASKED TO STOP stops the cue too, gracefully, as a lane's pass does.
        if (curveTable->stopping && run != nullptr && ! run->isFinished() && run->state != runState::stopping)
            engine.submit (origin::engine, "run.stop", one (curveTable->run));
    }

    std::string Runner::startLanePass (Engine& engine, std::int64_t tick, const std::string& cueId,
                                       double from, const std::string& runId)
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return {};

        /*  WHAT THE FIRE HANDS BACK IS NOT ALWAYS A NEW RUN (decision N): a
            cue already sounding is ignored and its run handed back as it is,
            wherever it has got to - and a sampler member muted at the bottom of
            its fader is sounding, since play-out mutes and does not stop. Until
            2026-10-05 a pass with no `from` (nought, and every pass from the
            D700's Rec) took that run as it found it, and recorded wherever the
            sample happened to be (namespace draft §30, item 3). So whatever
            was there before the fire is moved to where the pass starts. A run
            the fire has just made is armed at the cue's own start already, and
            is left to it unless another second was asked. */
        const auto existed = liveUntakenRunOf (cueId) != nullptr;
        const auto made = fire (engine, tick, cueId, runId);

        if (made.empty())
            return made;

        if (const auto* fired = runs.find (made); fired == nullptr || fired->kind != "media")
            return made;

        /*  FROM `from`, OR THE CUE'S OWN START - its start offset, or its first
            range's in-point - where DG puts the fader before a pass. The seek
            withdraws any stop asked of the run (HB): a run still on its way out
            from the last pass's stop - a graceful stop since K4, its tail
            ringing - is playing again, and the pass is not over on its first
            tick. A range's first pass, not the one it was on: the pass plays
            the cue from there, as a fire would. */
        if (existed || from > 0.0)
        {
            const auto ranges = rangesOf (cue);
            const auto start = from > 0.0       ? from
                             : ranges.empty()   ? numberOf (cue, "startOffset")
                                                : ranges.front().in;

            seekingRun (engine, tick, made);
            seekMedia (engine, tick, made, start, false);
        }

        /*  HEARD: a fader parked at the bottom plays the pass at -120 dB, and a
            ride nobody can hear is a ride nobody can judge. Lifted as a press
            lifts it, before anything is heard. */
        if (auto* fired = runs.find (made))
            liftParkedFader (*fired, cue);

        return made;
    }

    std::vector<std::string> Runner::seekGroup (Engine& engine, std::int64_t tick,
                                                const std::string& runId, double seconds,
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

        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished() || run->kind != "group")
            return used;

        const auto listId = listOfCue (run->cue);

        if (listId.empty())
            return used;

        /*  THE SCENE'S CUE, KEPT BY VALUE (2026-10-01, namespace draft §24): the
            seat below makes runs, and the run table is a vector - `run` points
            into memory that growth may move. Read through it after the seat,
            the step the scene re-dates was looked for under whatever lay there,
            and no step moved. */
        const auto sceneCue = run->cue;

        /*  WHAT THE PANES ARE TOLD (`run/seekable`) IS WHAT IS DONE (K9's
            review): a scene running its footer, or one Doh! took back, is left
            as it is whichever road it would take. */
        if (! seekableNow (*run))
            return used;

        const auto group = document.findById (sceneCue);

        std::vector<PlannedRun> wanted;
        auto sceneSeconds = seconds;

        /*  A SCENE THE WALK TIMES AS WRITTEN - a timeline or an automatic
            sequence that plays its members once, in order, with no header - is
            asked of the solver at that second, exactly as a jump asks it, in
            its own pre-wait too (HV). As written by the RUN (K9's review, MX):
            fired to play once, and playing the members in order. */
        if (seeksAsWritten (*run, group))
        {
            /*  Only the part under the scene is taken: what the solver found
                for the rest of the list is the jump's business, and a scrub on
                one group leaves everything beside it sounding as it was. */
            const auto plan = solve (document, durations, mounts, { listId, sceneCue, seconds });

            if (! plan.ok)
                return used;

            for (const auto& wants : plan.runs)
                if (wants.cue == run->cue
                     || std::find (wants.ancestors.begin(), wants.ancestors.end(), run->cue)
                          != wants.ancestors.end())
                    wanted.push_back (wants);

            /*  NOTHING PLACED UNDER IT, NOTHING DONE (2026-10-01, the J1 review,
                namespace draft §23.8, HX). The walk gives a scene's members their
                seconds only when nothing in the way of the arithmetic is unknown -
                one round, every member, in the written order, no header, and the
                machine pacing it (§13.8) - and for any other scene the solver
                places none of them: a scene that loops, shuffles or plays some of
                its members, a timeline with a header, a manual group. A seek there
                has nothing to seat, and it ended every member all the same and
                seated the scene alone, over a job with nothing left to wait for.
                With the seat keeping the round its scene is in (HV), a timeline in
                its last round - where one with a header that plays once always is -
                ran its footer there and then and ended where the hand had put it,
                and so did a manual group in its second round, whatever its operator
                still had to fire; before J1 both began their round again from the
                top. An automatic sequence awaited nothing for ever, either way.

                So it is applied and changes nothing - its members, its round, its
                step - as a seek on a run that is over is: the scene plays on where
                it was, and the head the hand dragged goes back to where the sound
                is. The solver reads the document and the media lengths, which a
                replay has from the log, so a replay takes the same road. */
            /*  (K9, 2026-10-02, §23.18:) the shapes it named are sought in their
                round below, a manual group still not; what is left here is a
                scene of the walk's shape whose members' lengths this build cannot
                read, which the solver cannot place either. */
            const auto placedUnder = std::any_of (wanted.begin(), wanted.end(),
                                                  [&sceneCue] (const PlannedRun& other)
                                                  {
                                                      return other.cue != sceneCue;
                                                  });

            if (! placedUnder)
                return used;
        }
        else
        {
            /*  ANY OTHER SCENE IS SOUGHT WHERE IT CAN SAY IT IS (K9, 2026-10-02,
                namespace draft §23.18; the author's ruling, which narrows HX). A running
                scene the walk cannot time knows what the walk did not: which round
                it is in and which members that round plays, in which order
                (`round`, from `run.round`'s handler), and when that round began
                (`roundStartedAtTick`, the same handler's). So a timeline or an
                automatic sequence that loops, shuffles or plays some of its
                members is sought WITHIN ITS CURRENT ROUND: the round solved as if
                it played once (`solveRound`), its members seated at the round's
                second, the scene keeping its round, its count and its seed (HV).
                The seconds asked are the scene's own, as its `position` reads,
                which spans its rounds: the round's second is that less where the
                round began, clamped to the round - before it is its start, past it
                its end, where every member is over and the scene goes on as it
                would there, to its next round or its footer. A timeline with a
                header is sought once its header is over, which is once it has
                begun a round; a second inside the header changes nothing, nor
                does a seek while the header still plays. A manual group and a
                sampler bank still change nothing (`seekableNow`). All of it read
                from handler state, so a replay takes the same road. */
            if (! seekableNow (*run) || run->iteration < 1 || run->round.empty())
                return used;

            const auto launchedAt = run->launchRequestedAtTick;
            const auto askedTicks = static_cast<std::int64_t> (ticksFor (seconds));
            const auto roundAt = std::max (std::int64_t { 0 }, run->roundStartedAtTick - launchedAt);

            if (! membersOf (group.getChildWithName ("Header")).empty()
                  && askedTicks < run->firstRoundAtTick - launchedAt)
                return used;

            const auto intoRound = static_cast<double> (askedTicks - roundAt)
                                     / static_cast<double> (TickClock::rateHz);

            const auto inRound = solveRound (document, durations, group, run->round,
                                             std::max (0.0, intoRound));

            if (inRound.runs.empty())
                return used;

            sceneSeconds = static_cast<double> (roundAt + ticksFor (inRound.at))
                             / static_cast<double> (TickClock::rateHz);

            /*  The scene's own groups, outermost first, which the round's runs
                hang under: what `seatPlan` reads is the last of them. */
            std::vector<std::string> above;

            for (auto parent = run->parent; ! parent.empty();)
            {
                const auto* up = runs.find (parent);

                if (up == nullptr)
                    break;

                above.insert (above.begin(), up->cue);
                parent = up->parent;
            }

            wanted.clear();

            PlannedRun scene;
            scene.cue = sceneCue;
            scene.ancestors = above;
            scene.offset = sceneSeconds;
            wanted.push_back (scene);

            for (auto member : inRound.runs)
            {
                member.ancestors.insert (member.ancestors.begin(), above.begin(), above.end());
                wanted.push_back (member);
            }
        }

        /*  WHAT IT HELD IS ENDED FIRST, the way a jump ends what it abandons:
            every descendant, no footer, its jobs retired and its voices and
            slots given back in this same drain. The scene's own run stands. */
        std::vector<std::string> below;

        const auto gather = [this, &below] (const std::string& parent, auto&& self) -> void
        {
            for (const auto* child : runs.childrenOf (parent))
            {
                below.push_back (child->id);
                self (child->id, self);
            }
        };

        gather (runId, gather);

        for (const auto& id : below)
        {
            if (auto* child = runs.find (id); child != nullptr && ! child->isFinished())
            {
                /*  GONE AT ONCE, not kept for reading: the seconds a finished
                    run stays published are for reading what happened, and
                    what happened here is the seek - a scrub that left five
                    rows of ended members behind at every step was a pane
                    nobody could read. */
                child->state = runState::done;
                child->endedAtTick = tick - retentionTicks - 1;
                child->prepare.clear();
                runs.releaseSlotsOf (child->id);

                if (child->track >= 0 && audio != nullptr)
                    audio->stop (child->track);
            }

            for (auto& job : scheduled)
                if (job.run == id)
                    job.retired = true;
        }

        for (auto& job : scheduled)
            if (job.run == runId)
                job.retired = true;

        //----------------------------------------------------------------------
        /*  THE GROUPS ABOVE IT ARE THE RUNS IT ALREADY HAS, and it is its own:
            `seatPlan` keeps what the map names and builds the rest under it. */
        std::map<std::string, std::string> runFor;
        runFor[run->cue] = runId;

        for (auto parent = run->parent; ! parent.empty();)
        {
            const auto* above = runs.find (parent);

            if (above == nullptr)
                break;

            runFor[above->cue] = above->id;
            parent = above->parent;
        }

        seatPlan (engine, tick, wanted, runFor, nextId);

        /*  AND WHAT ITS OLD JOB DECIDED IN THIS TICK IS NOBODY'S (K9's review,
            2026-10-03, MS). A seek drained in the tick a round ends finds the
            job's `run.round` and the next round's spawns on their way behind
            it, decided on the round the seek has just seated again: counted,
            the scene read the next round over this one's members and played
            both. They are applied and ignored, as the records a Doh hands back
            are (D2's `unadoptedAt`, the same mark and the same reading: the
            run was re-seated in this tick, and its hooks' decisions in it are
            on a state that is gone). */
        if (auto* seated = runs.find (runId))
            seated->unadoptedAt = tick;

        /*  The scene's step follows it: fired, as far as the history is now
            concerned, `seconds` ago. */
        lists.refired (listId, sceneCue, tick - ticksFor (sceneSeconds));

        return used;
    }

    void Runner::requestArmOn (Engine& engine, const juce::ValueTree& cue, Run& run,
                               double levelDb, int liveFirstInput, int liveWidth)
    {
        /*  Where it goes, resolved through the buses the show declares, so the
            audio side never has to know what a bus is. A rack channel's track
            is two channels wide whatever the voices are. */
        const auto live = liveFirstInput >= 0;
        std::string problem;
        const auto routing = resolveRouting (cue, live ? rackTrackChannels : audio->channelsPerTrack(),
                                             problem, chainChannelsOf (cue));

        if (! problem.empty())
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (run.id),
                             osc::Value::string (runError::badRoute) });
            return;
        }

        /*  A run holding a voice is `armed`, whatever it was before. A cue in
            its pre-wait keeps `waiting` - the operator's answer to "what is that
            cue doing" is the wait, not the plumbing underneath it. A run that
            is being SEEKED keeps `playing` for the same reason: it is sounding
            as far as anybody in the room is concerned. */
        if (! run.isWaiting() && run.state != runState::playing)
            run.state = runState::armed;

        ArmRequest request;
        request.runId = run.id;
        request.track = run.track;
        request.mediaFile = mediaPathOf (run.media);
        request.levelDb = levelDb;
        request.routing = routing;
        request.ranges = rangesOf (cue);
        request.stretch = run.stretch;

        /*  WHAT THE SLOTS WILL HOLD, kept on the run (namespace draft §33): the
            clips are written with these and never again while the cue sounds,
            so a move of a slice is measured from them and the playhead reads
            what the voice plays rather than what the document says now. */
        run.armedSlices.clear();

        for (const auto& range : request.ranges)
            run.armedSlices.push_back ({ range.in, range.out, range.pingPong });

        run.playingSlices = run.armedSlices;
        run.slicesRevision = document.showRevision();
        run.rearmEditedAt = -1;
        run.loopMove = 0;
        run.firstPassSamples = 0;
        run.passesBefore = 0;

        /*  THE CUE'S EQ RIDES THE ARM (Phase 9a), read through the schema
            here and applied on the far side while the voice is silent, as
            the routing is; and the run keeps the copy so `applyEq` can say
            what changed later. */
        request.eq = eqOf (cue);
        run.eq = request.eq;
        request.fx = fxOf (cue);
        run.fx = request.fx;

        /*  A LIVE INPUT (Phase 9b): no file, no ranges - the logical inputs its
            channel's stage takes. */
        request.live = live;
        request.firstInput = liveFirstInput;
        request.inputWidth = liveWidth;

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
        request.startOffset = run.startOffset > 0.0 ? run.startOffset
                                                    : numberOf (cue, "startOffset");

        /*  AND THE PLAYHEAD'S ORIGIN IS THAT SAME NUMBER, copied here because
            here is where it is decided. What `updatePositions` has to publish
            is the offset the voice was ACTUALLY armed with; a playhead that
            re-read the document per tick would draw itself wherever the cue was
            last edited to, which §4.10 says changes the NEXT run and not this
            one. A cue with ranges leaves it at nought and the launch overwrites
            it with the range's `in`, the document refusing an offset beside a
            range list for exactly that reason. */
        run.positionOrigin = request.startOffset;
        run.armedStartOffset = request.startOffset;

        /*  NO SLOT, and it is a refusal rather than a truncation. The graph is
            built with as many launcher slots as the show's widest cue has
            ranges, once, when the show loads (§3.25) - so a range added during
            the show has nowhere to be armed. Arming the first S of them would
            be a cue that plays most of what it says, which is worse than one
            that says it cannot. */
        if (static_cast<int> (request.ranges.size()) > audio->slotCount())
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (run.id),
                             osc::Value::string (runError::noSlot) });
            return;
        }

        /*  THE CUE'S LEVEL LANE (namespace draft §20.4), read here as the EQ
            is, for a voice with a file; a live input has no file to be bound
            to. Where the lane is read until the voice is launched is where the
            voice will START - the offset, or the in-point of the slice it
            enters - and a boundary pending from an earlier arm is forgotten,
            since a re-arm is a jump and the slice it was leaving is gone. */
        /*  THE SLICE IT ENTERS, AND HOW FAR INTO ITS LOOP (K8's review): a bed
            Esc paused inside a looping slice is armed at the same point of the
            loop - the passes it had played are the launch's to count, not the
            clip's. Every other arm enters its slice at the in-point. */
        if (! request.ranges.empty())
        {
            request.startSlot = std::clamp (run.startRange, 0, static_cast<int> (request.ranges.size()) - 1);

            const auto& slice = request.ranges[static_cast<std::size_t> (request.startSlot)];
            const auto length = slice.out - slice.in;

            if (run.sliceFrom > 0.0 && length > 0.0)
                request.sliceOffset = std::fmod (run.sliceFrom, length);
        }

        run.lane = live ? std::vector<doc::LanePoint> {}
                        : doc::readLevelLane (textOf (cue, "levelLane")).points;
        run.laneStart = request.ranges.empty()
                          ? request.startOffset
                          : request.ranges[static_cast<std::size_t> (request.startSlot)].in + request.sliceOffset;
        run.laneOutgoingAt = 0;
        run.laneDb = doc::laneLevelDb (run.lane, run.laneStart);

        /*  AND ITS SENDS' LANES (namespace draft §28), read at the same second
            and for the same reason: the arm's routing reaches the voice while
            it is silent, with no glide, so a send drawn up from silence starts
            silent rather than gliding down from where the send is written. */
        run.sendLanes = live ? std::map<std::string, std::vector<doc::LanePoint>> {} : sendLanesOf (cue);
        run.sendLaneDb = sendLaneOffsets (run.sendLanes, run.laneStart);

        if (! run.sendLaneDb.empty())
        {
            std::string lanesProblem;
            const auto withLanes = resolveRouting (cue, audio->channelsPerTrack(), lanesProblem,
                                                   chainChannelsOf (cue), nullptr, &run.sendLaneDb);

            if (lanesProblem.empty())
                request.routing = withLanes;
        }

        /*  THE CUE'S AUTHORED LEVEL IS THE RUN'S OWN, which is what a fade
            aimed at this cue moves and what a trim from a group above it is
            added TO. `level` itself is left for applyLevels to compute on the
            next tick, so there is one place that decides what a run is heard
            at rather than two that could disagree.

            AND THE VOICE IS SNAPPED WITH ITS LANE'S FIRST WORD IN IT (DC). The
            arm's level is set while the voice is silent, with no slew; a lane
            drawn up from silence would otherwise start its voice at the cue's
            level and slide down to where the lane says over the first tick -
            a burst at the top of a cue the designer drew as a fade-in. The
            lane is its own term, so the run's own level stays the cue's. */
        run.ownLevel = request.levelDb;
        request.levelDb += run.laneDb;
        run.level = request.levelDb;

        audio->requestArm (request);
    }

    std::vector<std::string> Runner::prepareStandby (Engine& engine, std::int64_t tick,
                                                     const juce::ValueTree& list,
                                                     const std::string& cueId,
                                                     const std::vector<std::string>& supplied)
    {
        /*  WHAT THE HORIZON MAKES IS NOBODY'S GO (2026-10-01, namespace draft
            §24, GY), whatever its parent carries - the next scene's block is
            made under a live act - and it says when it was made: after which
            GO. Doh! gives such a run back rather than taking it down. */
        struct Horizon
        {
            explicit Horizon (bool& flagToSet) : flag (flagToSet) { flag = true; }
            ~Horizon() { flag = false; }
            bool& flag;
        } horizon { makingForHorizon };

        /*  AND IT LEAVES OUT WHAT A DOH LEFT WITH A DEVICE'S OPERATOR on this
            list (HO): every cue any of the list's marks names. */
        std::set<std::string> leaveOut;

        if (const auto found = marks.find (list[idProperty].toString().toStdString()); found != marks.end())
            for (const auto& entry : found->second.left)
                leaveOut.insert (entry.second.begin(), entry.second.end());

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
            /*  NOT ONE ON ITS WAY OUT (2026-09-28): a group Esc is fading down
                is going, and preparing a member into it would hand the member
                to a group about to kill it. The horizon builds a fresh one,
                as it would once the old one has ended. */
            /*  NOR ONE INSIDE A SCENE ON ITS WAY OUT, nor a block made ready
                under one (2026-10-02, K3's review, §23.14, KU): since K3 a
                fade-and-stop keeps a scene `stopping` for as long as its fade,
                and a block reused under it was given back when its stop landed
                - its pre-send put back over whatever the GO in between had
                sent. The block under it is given back at once instead
                (`armStandby`), and a fresh one is made here. */
            if (const auto* live = runs.liveRunOf (groupId);
                live != nullptr && ! goingOut (*live))
            {
                parentRun = live->id;
                continue;
            }

            if (const auto* ready = runs.preparedRunOf (groupId);
                ready != nullptr && ! goingOut (*ready))
            {
                parentRun = ready->id;
                continue;
            }

            const auto id = nextId();

            createRun (id, groupId, "group", parentRun);

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
            const auto started = beginPreparation (engine, job, group, nextId, used, tick, leaveOut);

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
            /*  A CHILD DOH! TOOK BACK IS NOT ONE (2026-10-01, namespace draft
                §24): finished or fading, it is the GO that was taken back, and
                the cue is armed again under the block for the corrected one. */
            const auto children = runs.childrenOf (parentRun);

            if (std::any_of (children.begin(), children.end(),
                             [&id] (const Run* child) { return child->cue == id && ! child->takenBack; }))
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

            /*  ITS OLD VOICE IS STILL FADING OUT under a Doh: the cue waits for
                it (§24, the block path and the member path alike), remembered so
                the standby asks again once it has gone - an arm now would take
                a second voice, or queue behind its own channel. */
            if (standing != nullptr && standing->takenBack)
            {
                waitingForVoice = standing->id;
                continue;
            }

            const auto adoptable = standing != nullptr
                                     && standing->parent.empty()
                                     && standing->state == runState::armed
                                     && ! standing->launchRequested
                                     && ! claimedByAJob (standing->id);

            if (standing != nullptr && ! adoptable)
                continue;

            const auto made = spawnChild (engine, parentRun, id,
                                          adoptable ? std::string {} : nextId(), tick);

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
                that started then.

                NOT A GROUP ON ITS WAY OUT, though (2026-09-28). Esc fades a
                scene down before it stops it, so a group can be `stopping` for
                as long as the show's panic fade - and a member spawned into it
                would be killed by it on the next tick, a GO that made no sound.
                The GO starts the scene again instead, which is what it did
                when the group was gone a tick after Esc. */
            /*  NOR ONE INSIDE A SCENE ON ITS WAY OUT (2026-10-02, K3's review,
                §23.14, KU): the level above was built afresh, and the old run
                of this level is going with its parent. */
            if (const auto* live = runs.liveRunOf (groupId);
                live != nullptr && ! goingOut (*live))
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
            if (const auto* ready = runs.preparedRunOf (groupId);
                ready != nullptr && ! goingOut (*ready))
            {
                const auto adopted = ready->id;

                adoptPrepared (adopted,
                               level + 1 < ancestors.size()
                                 ? ancestors[level + 1][idProperty].toString().toStdString()
                                 : cueId,
                               parentRun.empty(), tick);

                parentRun = adopted;
                createdGroup = true;
                continue;
            }

            const auto id = nextId();

            createRun (id, groupId, "group", parentRun);

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

        /*  A SCENE THE HORIZON MADE READY UNDER THE ACT IS ADOPTED, not
            entered cold beside it (2026-10-01, namespace draft §23.9, J2).

            The pointer stands on the row of a scene that plays itself - a
            timeline, an automatic sequence - inside an act already running, and
            the horizon has prepared that scene under the act: its header run
            ahead, its first sounds armed. This road reached none of the doors
            that adopt a block, so it spawned a second run of the scene beside
            it, which entered cold - its sounds armed again with the hand
            already down, its header sent a second time - while the block held
            its voices and its pre-sends for as long as the act ran.

            ADOPTED THE WAY THE DESCENT ADOPTS A BLOCK UNDER A RUNNING GROUP:
            told where the pointer entered - nowhere inside it, so at its first
            member - stamped with this GO, and left standing for the act's job
            to launch on the next tick, as it launches every member; the launch
            lets the block's job out of its hold (`fireKind`, the third door).
            One launcher, as for any member: entered here as well, the act's
            own launch would arrive at a scene already running, and put one
            with a pre-wait back into `waiting` over its running header.

            NOTHING IS DRAWN, so the record carries nothing for it: a replay
            reaches the same block by the same road, the `run.prepare` that made
            it being in the log above this record. And only the block under THIS
            act - one prepared anywhere else is not the scene this act would
            play, and the GO enters the scene cold as it always has.

            AND WHEN THIS GO ENTERED THE SCENE'S PARENT AS WELL (2026-10-01,
            namespace draft §23.9, IC), by adopting the block the horizon made
            of it - an act's first scene, a manual scene's first line inside a
            running act - which is why this comes before the return below. The
            parent was told to enter at this scene, and the scene's block was
            left marked for the parent's job to ask for when its members begin.
            With nothing in the parent's header they begin on the next tick, in
            time; with a line in it, or with the parent itself waiting for a
            running act to launch it, they begin later, and the pointer this GO
            moved on found the block first - a scene nobody had asked for - and
            gave it back with its sounds and its desk values. The parent's
            members then began on a revoked run, and the scene the operator
            GO'd never played. Adopted here, it is this GO's
            from the press, and the parent's job takes it when its turn comes,
            as it takes any member it was told to enter at. A parent this GO
            CREATED has no block under it, so nothing changes for that one. */
        if (const auto* ready = runs.preparedRunOf (cueId);
            ready != nullptr && ready->parent == parentRun)
        {
            const auto adopted = ready->id;
            adoptPrepared (adopted, {}, false, tick);
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
        if (const auto* ahead = liveUntakenRunOf (cueId))
            if (ahead->state == runState::armed && ! ahead->prepare.empty())
            {
                const auto reachedId = ahead->id;

                /*  As it was, first (D2, namespace draft §24.12): the arm, and the act whose job
                    takes it. */
                snapshotAdoption (reachedId, parentRun);
                askedFor (reachedId);

                /*  AND IT IS THIS GO'S (2026-10-01, namespace draft §24). */
                stampSubtree (reachedId, currentGo);
                return used;
            }

        /*  Decision N, 2026-09-06: a media cue that is already sounding is
            ignored - the GO is applied and logged, the pointer has advanced, and
            the playing instance carries on. Not one Doh! took back, which is
            on its way out and is no longer the cue playing. */
        if (liveUntakenRunOf (cueId) != nullptr && kindOfCue (cue) == "media")
            return used;

        /*  A member of a manual group is spawned INTO it, so the group waits for
            it, its footer runs after it, and killing the group takes it with it. */
        /*  SPAWNED AND NOT LAUNCHED. The group's job starts it on the next
            tick, because one launcher is better than two: `run.launch` begins a
            pre-wait, and a member started from both here and there would begin
            its wait twice. */
        const auto id = spawnChild (engine, parentRun, cueId, nextId(), tick);

        if (id.empty())
            used.pop_back();

        return used;
    }

    //==============================================================================
    void Runner::fireNow (Engine& engine, std::int64_t tick, const std::string& runId)
    {
        auto* run = runs.find (runId);

        /*  NOR ONE DOH! TOOK BACK (2026-10-01): it fires nothing more. */
        /*  NOR ONE A KILL HAS REACHED (2026-10-02, H4, namespace draft §23.10).
            A killed scene ends its members a tick after the press, from its own
            job, and a member's fire can drain after the press in between: its
            pre-wait running out the tick after, or its launch asked for by the
            scene's job in the press's own tick. It fired - a MIDI note-on after
            the note-offs, ringing with nothing to end it; a value queued after
            the queue was emptied. "Drops all actions" reaches what was still to
            be asked for. Handler state, so a replay fires the same nothing. */
        if (run == nullptr || run->isFinished() || run->takenBack || beingKilled (*run))
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
        /*  WHEN IT STARTED, FOR EVERY KIND, and HERE because this is the one
            place all of them pass: a cue with a pre-wait arrives through
            `fireNow`, and a cue without one is fired straight from the GO
            without going near it.

            `launchRequestedAtTick` was stamped on the media path alone,
            because that is where lateness is measured and lateness is
            something only a PLACED launch can have - so a fade, an OSC cue or
            a memo carried a nought for ever, and anything drawing a playhead
            over one had nothing to measure from. A fade is the case that
            showed it: its bar sat at the start however long the fade was,
            which reads as a duration that does not take (author, 2026-09-18).

            Guarded, so the arming paths keep the stamp they already made:
            theirs is the tick lateness is measured against, and it is the same
            tick anyway. */
        if (auto* starting = runs.find (runId);
            starting != nullptr && starting->launchRequestedAtTick <= 0)
        {
            starting->launchRequestedAtTick = tick;
        }

        if (kind == "fade")
        {
            fireFade (cue, runId, tick);
            return;
        }

        if (kind == "transport")
        {
            fireStop (cue, runId, tick);
            return;
        }

        /*  WHERE IT IS SENT, DECIDED AT THE SEND (2026-10-01, namespace draft
            §24, HQ): the device the document routes the cue to now, declared
            and with its `tx` on - nothing could leave otherwise - and nothing
            for a run that sends nothing by construction. Every road to a send
            passes here, so a Doh later reads what left, and where, from the
            run: a device edited or removed in between does not move it. */
        if (kind == "osc" || kind == "midi")
            if (auto* firing = runs.find (runId))
                firing->sentTo = firing->sendsLeft ? std::string {}
                                                   : sendTargetOf (document, kind, firing->cue);

        if (kind == "osc")
        {
            fireOsc (cue, runId, tick);
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

                    /*  AND A SAMPLER GROUP TAKES OVER AS IT ARMS, here as in
                        the other two roads in (2026-10-01, namespace draft
                        §23.9): a bank the horizon made ready on an act's row,
                        adopted by the GO there, is launched by the act through
                        this door alone - and so is one an act nobody had
                        entered launches as its first row. Let out of the hold
                        and nothing more, a bank that takes over the whole desk
                        armed beside the bank it should have closed. Only ever
                        after a GO: a block is launched by its parent's members,
                        and not by its parent's preparation (IE). */
                    if (textOf (cue, "mode") == "sampler")
                        takeOverFrom (runId, cue);
                }

                return;
            }

            /*  A SAMPLER GROUP ARMING TAKES OVER (PRD §3.27): with
                `takeover=group` every other armed sampler group is closed, in
                this handler, so a replay closes the same ones. */
            if (textOf (cue, "mode") == "sampler")
                takeOverFrom (runId, cue);

            GroupJob job;
            job.run = runId;
            job.enterAt = run->enterAt;
            scheduled.push_back (job);
            return;
        }

        if (kind == "start")
        {
            /*  A START CUE PRESSES A BUTTON (2026-09-19): it names a cue and
                fires it as a surface would, by name, standby untouched. The
                fire itself is a record of its own - `cue.fire`, submitted by
                the next tick's hook and never from here - so the target's
                runs carry their identifiers in a record a replay re-supplies,
                and the start cue is a memo in every other respect: playing
                now, done the next tick, its waits its own. */
            if (auto* run = runs.find (runId))
                run->state = runState::playing;

            /*  AND WHICH GO IT FIRES UNDER (2026-10-01, namespace draft §24):
                the start cue's own, so the target's runs carry it and a Doh of
                that GO takes them back with it. */
            if (const auto target = textOf (cue, "target"); ! target.empty())
                startsToFire.push_back ({ target, goOfRun (runId) });

            finishing.push_back (runId);
            return;
        }

        /*  A MIC CUE (Phase 9b, namespace draft §18.5): a launch asked for, as
            a media cue's is, and the arm for one fired cold - its channel
            claimed, or waited for, and its plugins set as the cue says. */
        if (kind == "mic")
        {
            auto* run = runs.find (runId);

            if (run == nullptr)
                return;

            run->launchRequested = true;
            run->launchRequestedAtTick = tick;
            run->prepare.clear();

            if (run->track < 0)
                armMic (engine, cue, runId);

            /*  ITS TAKE, AS ITS ROW SAYS (decision CF): now if the channel is
                the cue's already - claimed at GO, or ahead in standby - and when
                the claim lands otherwise, which the release says. */
            applyTakeOnGo (cue, runId);
            return;
        }

        /*  A VIDEO CUE (Phase 8a, namespace draft 35): a layer on a canvas,
            brought up by the hook a launch horizon ahead, as a media cue's
            clip is launched - and then it HOLDS until something stops it, as a
            mic cue does (VJ): Esc, a stop cue, a kill. What it is, read here
            at GO and carried as a value: the far side never reads the
            document (VM). */
        if (kind == "video")
        {
            VideoJob job;
            job.self = runId;
            job.spec.id = runId;
            job.spec.canvas = textOf (cue, "canvas");
            job.spec.layer = static_cast<int> (std::lround (numberOf (cue, "layer")));
            job.spec.order = ++videoOrder;
            job.spec.source = textOf (cue, "source");
            job.spec.blend = textOf (cue, "blend");

            const auto paint = textOf (cue, "paint");
            job.spec.paint = video::paintFromText (paint.data(), paint.size());

            /*  A PICTURE'S FILE AS A WHOLE PATH, resolved here as a sound's is
                (VX), and the geometry as the cue is written (§36.3). */
            if (job.spec.source == "picture" || job.spec.source == "movie")
                job.spec.file = mediaPathOf (textOf (cue, "file"));

            //  A CAPTURE'S VIDEO INPUT, by identifier (namespace draft §44, YC).
            if (job.spec.source == "capture")
                job.spec.input = textOf (cue, "videoInput");

            //  AND THE INSERT ITS PICTURE GOES THROUGH, if any (§44, YE).
            job.spec.insert = textOf (cue, "videoInsert");

            /*  A MOVIE'S PLAYHEAD (§37): from its start offset at its speed,
                or from its first Range's in point (WL). */
            if (job.spec.source == "movie")
            {
                job.movie = true;
                job.movieFile = textOf (cue, "file");
                job.cue = cue[idProperty].toString().toStdString();
                job.moviePosition = std::max (0.0, numberOf (cue, "startOffset"));
                job.rate = std::clamp (numberOf (cue, "rate"), -20.0, 20.0);   // below nought, backwards (§41)

                job.pieceStart = job.moviePosition;

                if (const auto ranges = rangesOf (cue); ! ranges.empty())
                {
                    job.rangeId = ranges.front().id;
                    job.moviePosition = std::max (0.0, job.rate < 0.0 ? ranges.front().out : ranges.front().in);
                }
                else if (job.rate < 0.0)
                {
                    /*  BACKWARDS FROM THE END (namespace draft §41): its piece
                        the other way, from the file's end - when the show knows
                        it; not known, it waits at its start offset. */
                    if (const auto* known = handlerDurations())
                        if (const auto found = known->find (job.movieFile); found != known->end() && found->second > job.moviePosition)
                            job.moviePosition = found->second;
                }
            }

            job.spec.fit = textOf (cue, "fit");
            job.spec.scale = numberOf (cue, "scale");
            job.spec.offsetX = numberOf (cue, "offsetX");
            job.spec.offsetY = numberOf (cue, "offsetY");
            job.spec.rotation = numberOf (cue, "rotation");
            job.spec.flipH = textOf (cue, "flipH") == "true";
            job.spec.flipV = textOf (cue, "flipV") == "true";

            /*  THE GRADE (§36, VP, VU), the curves baked here, once, so the
                far side reads tables and never text (VM). */
            job.spec.grade.contrast = numberOf (cue, "contrast");
            job.spec.grade.saturation = numberOf (cue, "saturation");
            job.spec.grade.gamma = numberOf (cue, "gamma");
            job.spec.grade.hue = numberOf (cue, "hue");

            const auto curveOf = [this, &cue] (const char* row)
            {
                std::vector<double> numbers;

                for (const auto& word : juce::StringArray::fromTokens (juce::String (textOf (cue, row)), " ", ""))
                    if (const auto value = osc::parseDouble (word.toStdString()); value.has_value())
                        numbers.push_back (*value);

                return video::curveFrom (numbers);
            };

            video::bakeCurves (job.spec.grade, curveOf ("curveLuma"),
                               { curveOf ("curveRed"), curveOf ("curveGreen"), curveOf ("curveBlue") });

            /*  A MASK'S OUTLINE (UY, VF): x, y pairs, at most `maxPoints`. */
            {
                std::vector<double> corners;

                for (const auto& word : juce::StringArray::fromTokens (juce::String (textOf (cue, "shape")), " ", ""))
                    if (const auto value = osc::parseDouble (word.toStdString()); value.has_value())
                        corners.push_back (*value);

                const auto count = std::min<std::size_t> (corners.size() / 2, video::mask::maxPoints);

                for (std::size_t n = 0; n < count; ++n)
                {
                    job.spec.shape.x[n] = static_cast<float> (std::clamp (corners[2 * n], -1.0, 2.0));
                    job.spec.shape.y[n] = static_cast<float> (std::clamp (corners[2 * n + 1], -1.0, 2.0));
                }

                job.spec.shape.count = static_cast<int> (count);
                job.spec.shape.feather = static_cast<float> (std::max (0.0, numberOf (cue, "feather")));
                job.spec.shape.invert = textOf (cue, "invert") == "true";
            }

            /*  IN %, the author's unit (VR): the renderer's opacity is 0..1. */
            job.opacity = std::clamp (numberOf (cue, "opacity") / 100.0, 0.0, 1.0);
            job.fadeInSeconds = std::max (0.0, numberOf (cue, "fadeIn"));

            const auto movieCue = job.movie ? job.cue : std::string {};

            std::erase_if (showing, [&runId] (const VideoJob& held) { return held.self == runId; });
            showing.push_back (std::move (job));

            /*  ITS OWN SOUNDS, fired with it in this tick (namespace draft
                37.5, WJ): launched where it is placed, a horizon ahead. */
            if (! movieCue.empty())
                fireLockedSounds (engine, tick, movieCue, runId);

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

        /*  A MIC CUE'S CHANNEL IS A CLAIM (Phase 9b, decision BX): held alone,
            and queued behind another run that holds it - a processor input's
            policy and not an insert's, because a live voice played dry without
            the plugins it was given is not the cue somebody fired. A shared
            channel is refused at the arm, in words. */
        if (cue.hasType ("Mic"))
        {
            const auto channelId = textOf (cue, "channel");
            const auto channel = channelId.empty() ? juce::ValueTree() : document.findById (channelId);

            if (channel.isValid() && channel.hasType ("Channel")
                  && channel.getProperty ("access", "exclusive").toString() != "shared")
            {
                const auto* holder = runs.holderOf (channelId);
                const auto queued = std::find (run->pending.begin(), run->pending.end(), channelId)
                                      != run->pending.end();

                if (holder != run && ! queued)
                {
                    if (holder == nullptr)
                        run->claims.push_back (channelId);
                    else
                        run->pending.push_back (channelId);
                }
            }
        }

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

            /*  A SLOT THIS RUN ALREADY HOLDS, OR ALREADY WAITS FOR, IS NOT
                ASKED FOR AGAIN. With no audio side a run is armed twice - a
                pre-wait arms it on entry, and the fire path arms it again when
                the wait is over because its track is still -1 - and the second
                arm used to find the run itself holding the slot and read that
                as busy: a Feed queued behind its own claim, and an Insert
                warned `no-channel` against itself. That is the configuration
                `wfg replay` runs in and every serve without `--hosted`, and a
                hosted session, which reserves a track at the first arm, never
                reaches it - so the two disagreed about what the slot rows said.
                Suspected in PR 5.6 (namespace §14.5) and confirmed by a test
                before this line was written. */
            if (holder == run
                  || std::find (run->pending.begin(), run->pending.end(), slotId) != run->pending.end())
                continue;

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

        /*  THE FILE, beside the claim and above the same return, for the same
            reason: which file a cue names is the document's business, not the
            audio side's. Copied below it, `Run::media` would be empty on every
            replay and on every `wfg serve` without `--hosted`, and the case it
            exists for - a run that goes on sounding after an undo took its cue
            away, and must still say what it plays - is one a replay reproduces
            (namespace draft §14.5).

            ONCE, AT THE FIRST ARM. With an audio side the first arm reserves a
            track and the guard above returns on every later one, so a hosted
            run keeps the file it was armed with. Without one the track stays -1
            and a run can be armed twice - a cue with a pre-wait, armed and then
            fired; a sequence member, spawned and armed and launched minutes
            later - and a second read would pick up an edit the hosted session
            never plays. So the copy is taken only while the run has none, which
            makes every configuration agree with the one that sounds. *Corrected
            in PR 5.6's review (2026-09-14).* The one edge left is a cue that
            named no file at its first arm, whose later arm may still read one. */
        if (run->media.empty())
            run->media = textOf (cue, "file");

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

        /*  The run's copy rather than a second read of the document, so the
            file resolved and played below is the file the run says it plays. */
        const auto named = run->media;

        /*  RESOLVED AGAINST THE BUNDLE, and checked here rather than three
            layers down. A cue naming a file the bundle does not have fails its
            RUN and never the load - a show with one missing sound is still a
            show somebody has to run tonight - and finding out at the arm rather
            than at the launch means the failure is reported while the operator
            is still reading the next line. */
        const auto file = mediaPathOf (named);

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

        /*  A SAMPLER MEMBER WAITS FOR A VOICE (decision Z, 2026-09-23). Every
            armed member holds a track so a press sounds at once, and a bank
            larger than the tracks left is not a failure: the member says
            `voice` in its pending list - in words, never colour alone - and the
            scheduler hands it the next track that frees. A cue fired by GO
            still fails at entry, visibly, as it always has. */
        if (track < 0 && run->sampler)
        {
            if (std::find (run->pending.begin(), run->pending.end(), "voice") == run->pending.end())
                run->pending.push_back ("voice");

            return;
        }

        if (track < 0)
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (runId),
                             osc::Value::string (runError::noTrack) });
            return;
        }

        /*  Reserved from here, so a second arm on the same tick cannot pick the
            same voice. The audio side confirms with audio.armed once the graph
            and the disk are ready; until then the run is armed and silent. */
        run->track = track;

        /*  THE SPEED AND ITS MODE (namespace draft §22), read here because the
            arm is where the mode has to be - it rebuilds Tracktion's graph - and
            where a fresh run's speed begins. A seek arms again through
            requestArmOn and keeps both (DV). Read with one as the fallback, never
            nought: an unreadable number read as nought would be a stopped tape.
            A stretched run is held to the stretcher's limit at this rate, so the
            run's clock and the voice's never disagree. */
        run->stretch = textOf (cue, "rateMode") == "timestretch";
        run->rateSeen = osc::parseDouble (textOf (cue, "rate")).value_or (1.0);
        const auto stretchLimit = audio != nullptr ? audio->stretchSpeedLimit() : 0.0;
        run->ownRate = run->stretch && stretchLimit > 0.0
                         ? std::copysign (std::min (std::abs (run->rateSeen), stretchLimit), run->rateSeen)
                         : run->rateSeen;
        run->ratePlaced = run->ownRate;
        run->rateNow = run->ownRate;

        /*  BACKWARDS FROM THE START (namespace draft §41): fresh at every arm,
            and Go.dot's own stop cleared with it. */
        run->direction = run->ownRate < 0.0 ? -1 : 1;
        run->turned = false;
        run->bent = false;
        run->turnEndPlaced = 0;

        /*  A RUN THAT ARRIVES OVER A DE-CLICK IS ARMED AT SILENCE (K8's review;
            namespace draft §24, GQ): the level it comes up to is kept, and the
            scheduler starts the ramp once its launch is placed
            (`deClickLaunched`). Its own level is the silence until then, so
            nothing - `applyLevels`, a seek's re-arm - puts it at the cue's
            level ahead of the ramp. */
        auto level = numberOf (cue, "level");

        /*  AND ONE DOH! CARRIES ON comes up to the level it had when the Doh
            paused it, not to its cue's (D2): a fade had it somewhere else. */
        if (run->deClick)
        {
            run->deClickTo = run->arrivalDb.value_or (level);
            run->deClickOwed = true;
            level = silenceDb;
        }

        requestArmOn (engine, cue, *run, level);
    }

    void Runner::armMic (Engine& engine, const juce::ValueTree& cue, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->track >= 0)
            return;

        /*  THE CHANNEL'S CLAIM FIRST, above the return below, for armMedia's
            reason: which channel a cue holds is a fact about the document and
            the run table, so a replay takes the same one (§13.4). */
        claimSlotsFor (cue, runId);

        /*  A MIC CUE DOH! CARRIES ON opens over the de-click (D2, namespace draft §24.12): its
            gate is its arrival, and a tenth of a second is the author's. */
        /*  AND OVER THE PANIC FADE, one the GO had stopped (D3). */
        run->fadeIn = run->resumes ? static_cast<double> (run->arrivalTicks > 0 ? run->arrivalTicks : deClickTicks)
                                       / static_cast<double> (TickClock::rateHz)
                                   : std::max (0.0, numberOf (cue, "fadeIn"));

        if (audio == nullptr)
            return;

        /*  WHAT WOULD FAIL IT, each in its own word, and `wfg validate` says
            the first three before the show. Reported from below the return,
            as every run report is, so a replay has the log's copy only. */
        const auto fail = [&engine, &runId] (const char* why)
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (runId), osc::Value::string (why) });
        };

        const auto inputId = textOf (cue, "input");
        const auto channelId = textOf (cue, "channel");
        const auto input = inputId.empty() ? juce::ValueTree() : document.findById (inputId);
        const auto channel = channelId.empty() ? juce::ValueTree() : document.findById (channelId);

        if (! input.isValid() || ! input.hasType ("Input"))
            return fail (runError::noInput);

        if (! channel.isValid() || ! channel.hasType ("Channel")
              || channel.getProperty ("access", "exclusive").toString() == "shared")
            return fail (runError::badChannel);

        /*  WHAT A CLASS TAKES IN, the first half of its name: mono and
            mono-to-stereo take one channel, stereo two (PRD §3.9e). */
        const auto width = sourceChannelsOf (cue);
        const auto classTakes = channel.getProperty ("class", "mono").toString() == "stereo" ? 2 : 1;

        if (width != classTakes)
            return fail (runError::badWidth);

        /*  WAITING FOR ITS CHANNEL, armed with no track (decision CM): a run
            holding a track would receive every live push the Runner makes and
            move the sounding cue's matrix, EQ and plugins. `armWaitingMics`
            asks again when the claim lands; the row says `pending` meanwhile. */
        if (std::find (run->pending.begin(), run->pending.end(), channelId) != run->pending.end())
        {
            run->waitsForChannel = true;
            return;
        }

        /*  A CHANNEL THE GRAPH WAS BUILT WITHOUT - declared since the show
            opened - until Load now builds it. */
        const auto track = audio->rackTrackOf (channelId);

        if (track < 0)
            return fail (runError::notBuilt);

        run->track = track;

        static const Reader schema;
        const auto first = static_cast<int> (schema.integer (input, "input", "firstChannel"));

        requestArmOn (engine, cue, *run, numberOf (cue, "level"), first, width);
    }

    void Runner::takeOnGo (const std::string& runId, const std::string& channelId)
    {
        const auto* run = runs.find (runId);

        if (run == nullptr || takes == nullptr)
            return;

        /*  WAIT MAKES NO SOUND NOBODY ASKED FOR; loop plays the take it finds,
            and with none waits for Rec; clear empties the channel for a fresh
            one (§19.6). */
        const auto word = textOf (document.findById (run->cue), "onGo");
        const auto channel = samplingChannelOf (document, channelId);
        const auto before = takes->of (channelId);

        if (word == "loop")
            takes->press (channelId, TakeVerb::loop, channel.layers);
        else if (word == "clear")
            takes->press (channelId, TakeVerb::clear, channel.layers);

        /*  A PRESS OF THE GO'S (Doh! D3): undone by its inverse, or said. */
        if (word == "loop" || word == "clear")
            noteTake (runId, channelId, word == "loop" ? TakeVerb::loop : TakeVerb::clear, before);
    }

    void Runner::applyTakeOnGo (const juce::ValueTree& cue, const std::string& runId)
    {
        if (takes == nullptr)
            return;

        const auto channelId = textOf (cue, "channel");

        if (! samplingChannelOf (document, channelId).samples())
            return;

        if (auto* run = runs.find (runId))
        {
            if (std::find (run->claims.begin(), run->claims.end(), channelId) != run->claims.end())
                takeOnGo (runId, channelId);
            else
                run->takeOnGoPending = true;
        }
    }

    void Runner::takeReleased (const std::string& slotId, const std::string& toRun)
    {
        if (takes == nullptr || ! samplingChannelOf (document, slotId).samples())
            return;

        /*  THE TAKE OUTLIVES ITS CUE (§19.3): held silent, a first pass or a
            layer closed as it stood - never lost. The audio side held it at the
            stop already; this is the account catching up, in a place a replay
            reaches too. */
        takes->release (slotId);

        //  AND THE CUE THAT WAS WAITING FOR IT, if its GO has come.
        if (auto* next = runs.find (toRun); next != nullptr && next->takeOnGoPending)
        {
            next->takeOnGoPending = false;
            takeOnGo (toRun, slotId);
        }
    }

    void Runner::serviceTakes (Engine& engine)
    {
        if (takes == nullptr)
            return;

        /*  THE PRESSES, in the order they were asked for, each placed where a
            launch asked for now would be (§19.6), so a press and a cue land on
            a sample the log can name. */
        const auto at = audio->samplesElapsed()
                          + static_cast<std::int64_t> (latencyTicks()) * static_cast<std::int64_t> (samplesPerTick);

        for (const auto& press : takes->takePresses())
            audio->postTake (press.channel, press.verb, at, press.in, press.out);

        /*  KEEP (§19.8): each asked for handed to the writer beside the takes
            - a channel the audio side holds no take for answered at once, as
            the writer would answer it - and what the writer has finished, for
            the log, which is where a replay learns the file's name. */
        for (const auto& request : takes->keepRequests())
            if (! audio->keepTake (request.channel, request.stem, mediaFolder))
                engine.submit (origin::engine, "take.kept",
                               { osc::Value::string (request.channel), osc::Value::string (std::string {}),
                                 osc::Value::string ("there is no take on this channel") });

        for (const auto& kept : audio->keptTakes())
            engine.submit (origin::engine, "take.kept",
                           { osc::Value::string (kept.channel), osc::Value::string (kept.file),
                             osc::Value::string (kept.error) });

        /*  WHAT THE AUDIO THREAD DID BY ITSELF - a take closed, and at what
            length - for the log, and every channel the account knows is asked,
            emptied ones too: a report left unread would reach a take recorded
            later. */
        const auto channels = takes->channels();

        for (const auto& report : audio->takeReports (channels))
            engine.submit (origin::engine, "take.closed",
                           { osc::Value::string (report.channel), osc::Value::float64 (report.seconds),
                             osc::Value::string (report.how) });

        //  WHERE EACH LOOP IS PLAYING, for the picture; not in the log.
        for (const auto& channel : channels)
            takes->setPlayhead (channel, audio->takePlayhead (channel));

        /*  AND WHETHER EACH CHANNEL SOUNDS ITS INPUT AS WELL (§19.3): as the
            mic cue holding it says, told when it changes. */
        for (const auto& snapshot : runs.all())
        {
            if (snapshot.kind != "mic" || snapshot.isFinished() || snapshot.claims.empty())
                continue;

            const auto cue = document.findById (snapshot.cue);
            const auto channelId = textOf (cue, "channel");
            const auto through = textOf (cue, "through") == "true" ? 1 : 0;

            if (snapshot.throughSent == through || ! samplingChannelOf (document, channelId).samples())
                continue;

            audio->setTakeThrough (channelId, through != 0);

            if (auto* run = runs.find (snapshot.id))
                run->throughSent = through;
        }
    }

    void Runner::armWaitingMics (Engine& engine)
    {
        for (const auto& snapshot : runs.all())
        {
            if (! snapshot.waitsForChannel || snapshot.isFinished() || snapshot.track >= 0
                  || ! snapshot.pending.empty())
                continue;

            if (auto* run = runs.find (snapshot.id))
                run->waitsForChannel = false;

            engine.submit (origin::engine, "run.arm", one (snapshot.id));
        }
    }

    std::string Runner::statePathOf (const std::string& named) const
    {
        if (named.empty() || pluginsFolder.empty())
            return {};

        /*  NOTHING OUTSIDE plugins/: `fx.capture` refuses such a name, but a
            script may write the row whole, and a state is bytes a plugin is
            made to believe. */
        const auto climbs = named.find ("..") != std::string::npos || named.find (':') != std::string::npos
                              || named.front() == '/' || named.front() == '\\';

        return juce::File (juce::String (pluginsFolder))
                   .getChildFile (climbs ? juce::String ("not-a-state-in-this-bundle") : juce::String (named))
                   .getFullPathName().toStdString();
    }

    std::string Runner::mediaPathOf (const std::string& named) const
    {
        /*  RESOLVED AGAINST THE BUNDLE. A run's copy of the file name is
            bundle-relative, which is how the document writes it and the key
            the analyser files its records under; the audio side wants the
            path on disk. */
        if (named.empty())
            return named;

        //  The one resolver: own media/, then the show's around it (MediaInfo.h).
        return audio::resolveMediaPath (mediaFolder, named);
    }

    //==============================================================================
    std::vector<Coefficient> Runner::resolveRouting (const juce::ValueTree& cue,
                                                     int trackChannels,
                                                     std::string& problem,
                                                     int chainChannels,
                                                     const std::map<std::string, double>* moved,
                                                     const std::map<std::string, double>* laneOffsets) const
    {
        std::vector<Coefficient> out;
        problem.clear();

        /*  READ THROUGH THE SCHEMA AND NEVER OFF THE TREE. The canonical
            writer omits an attribute that equals its default - a slot one
            channel wide is written with no width at all - and the document's
            own getter gives the default back, which is the round trip that
            omission depends on. Read raw, an absent width is nought, and a
            show that had been SAVED and reopened refused every feed in it as
            `bad-route` (author, 2026-09-18: "when the standby pointer lands
            on 2 it shows in the active cues as an error bad route"). It was
            found on a RECOVERED show, which is the same writer; every cue
            attribute here already went through `numberOf`, and these four
            were the only reads that did not.

            Built once: the defaults are the parameter table's and do not
            change under a running show. */
        static const Reader schema;

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

            const auto written = static_cast<int> (gains.size()) / width;

            if (written > trackChannels)
            {
                problem = "the cue is wider than a track";
                return false;
            }

            /*  A CUE ITS INSERTS MADE WIDER THAN THE ROWS WRITTEN FOR IT
                (2026-09-26): a mono cue through a stereo reverb, routed by a
                one-row matrix. The route has room for one side, so the two
                are summed into it - side `i` takes row `i mod rows`, at
                rows / sides - and the cue is never narrower than it was
                written for. As many rows as sides, and it is as written. */
            const auto inputs = chainChannels > written && written > 0 ? std::min (chainChannels, trackChannels) : written;
            const auto share = inputs > written ? static_cast<double> (written) / static_cast<double> (inputs) : 1.0;

            for (int input = 0; input < inputs; ++input)
                for (int channel = 0; channel < width; ++channel)
                {
                    const auto gain = gains[static_cast<std::size_t> ((input % written) * width + channel)] * share;

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

        /*  HOW A CUE'S CHANNELS SPREAD ACROSS A DESTINATION, for the two
            places that do not carry a written matrix: the direct out and every
            send. A Route and a Feed say it themselves, coefficient by
            coefficient, because a designer placing a source among twelve
            processor inputs means something no rule could guess. A direct out
            and a mix channel are the ordinary cases, and the rule for them is
            short enough to be read:

              - stereo onto one channel: both channels at half, folded
              - a mono cue: its one channel onto everything, at full
              - as wide as the destination, or narrower: channel to channel
              - wider than the destination otherwise: refused, and the run says so

            THE FOLD ASKS NOBODY (2026-10-08, the author: "Can we have mono to
            stereo and stereo to mono direct outs?"). It used to wait for the
            cue's `stereoToMono`, and a stereo cue aimed at a mono out without
            it failed the run - a switch whose only other answer was an error.
            A bus is a summing point and its width does not matter (PRD 3.9b's
            own table), so two sides onto one channel is folded the way one side
            onto two is spread. Wider files stay refused: six channels onto two
            is a layout somebody has to say, and no rule here knows it.

            `what` is the destination in words, so the refusal names the thing
            the designer was looking at rather than a kind they never chose. */
        const auto spreadOf = [&] (const juce::ValueTree& mediaCue, int width,
                                   double gain, const char* what,
                                   std::string& why) -> std::vector<double>
        {
            const auto fileChannels = sourceChannelsOf (mediaCue);

            if (fileChannels <= 0)
            {
                why = "the cue's channel count is not known";
                return { 0.0 };   // a non-empty list, so `emit` reports rather than passes
            }

            /*  A CUE ITS INSERTS MADE WIDER (2026-09-26, the author's decision:
                a plugin can make a mono cue stereo). Where the destination has
                room for every side, side to channel; where it has fewer, side
                `i` onto channel `i mod width` at width / sides - two sides onto
                a mono speaker at a half each. Never refused for its width: the
                cue is no wider than it was, only than its file. */
            const auto widened = chainChannels > fileChannels;

            if (widened)
            {
                const auto sides = chainChannels;
                std::vector<double> gains (static_cast<std::size_t> (sides) * static_cast<std::size_t> (width), 0.0);
                const auto share = sides > width ? static_cast<double> (width) / static_cast<double> (sides) : 1.0;

                for (auto input = 0; input < sides; ++input)
                {
                    if (sides <= width)
                        gains[static_cast<std::size_t> (input * width + input)] = gain;
                    else
                        gains[static_cast<std::size_t> (input * width + input % width)] = gain * share;
                }

                return gains;
            }

            const auto channels = fileChannels;
            const auto fold = channels == 2 && width == 1;

            if (! fold && channels > width)
            {
                why = std::string ("the cue is wider than its ") + what;
                return { 0.0 };
            }

            /*  A FOLD KEEPS BOTH ROWS, and it has to: folding is SUMMING the
                two sides, so each one needs its own row into the destination
                at half. One row of half would be the left channel alone at
                -6 dB, which is not a fold, and `emit` reads the row count back
                out of the length so it would never notice. */
            std::vector<double> gains (static_cast<std::size_t> (channels)
                                         * static_cast<std::size_t> (width), 0.0);

            for (auto input = 0; input < channels; ++input)
                for (auto channel = 0; channel < width; ++channel)
                {
                    const auto at = static_cast<std::size_t> (input * width + channel);

                    if (fold)
                        gains[at] = gain * 0.5;
                    else if (channels == 1 || input == channel)
                        gains[at] = gain;
                }

            return gains;
        };

        /*  A SEND INTO A MIX CHANNEL: the spread a direct out would get,
            scaled by the send's level - shared by the show's sends and the
            ones a locked show made live. False when the run cannot be routed,
            with `problem` saying why. */
        const auto sendInto = [&] (const juce::ValueTree& bus, double level, bool on) -> bool
        {
            /*  SILENCE CONTRIBUTES NOTHING AT ALL rather than a very small
                number: -120 dB is how this document spells silence
                everywhere else, and a track with nothing to add should
                cost nothing to mix. `emit` drops exact zeroes anyway; this
                saves building the matrix. */
            if (level <= -120.0)
                return true;

            /*  AND A SEND SWITCHED OFF is out of the mix the same way, its
                level kept for when it comes back on (send/on). */
            if (! on)
                return true;

            std::string why;
            const auto width = schema.integer (bus, "bus", "width");
            const auto gain = juce::Decibels::decibelsToGain (level, -120.0);
            const auto gains = spreadOf (cue, width, gain, "mix channel", why);

            /*  ASKED HERE rather than left to `emit`, which cannot always
                tell: a refused spread answers with one zero, and against a
                mono destination that is a legal matrix of one silent
                coefficient - so the send would be dropped quietly instead
                of failing the run with a reason somebody can read. */
            if (! why.empty())
            {
                problem = why;
                return false;
            }

            return emit (schema.integer (bus, "bus", "firstChannel"), width, gains);
        };

        /*  THE DIRECT OUT, where this cue's own channels land. Empty is every
            cue until somebody chooses one, and is silent rather than wrong -
            the same reading `emit` gives an empty gains list. */
        if (const auto directOut = schema.text (cue, "sound", "directOut"); ! directOut.empty())
        {
            const auto bus = busNamed (directOut);

            if (! bus.isValid())
            {
                problem = "the cue's direct out is no bus of this show";
                return {};
            }

            std::string why;
            const auto width = schema.integer (bus, "bus", "width");
            const auto gains = spreadOf (cue, width, 1.0, "direct out", why);

            if (! why.empty())
            {
                problem = why;
                return {};
            }

            if (! emit (schema.integer (bus, "bus", "firstChannel"), width, gains))
                return {};
        }

        /*  The mixes this cue sends into, so a fade's send into one it does not
            is told apart (namespace draft §26). */
        std::vector<std::string> sentTo;

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

                if (! emit (schema.integer (bus, "bus", "firstChannel"),
                            schema.integer (bus, "bus", "width"),
                            gainsOf (destination)))
                    return {};

                continue;
            }

            if (element == "Send")
            {
                /*  A SEND IS A DESTINATION WITH A LEVEL, which is the whole
                    difference between it and a Route above. A Route carries a
                    matrix, because a designer placing a source among twelve
                    processor inputs means something no rule could guess; a
                    send carries one number, because a mix channel is somewhere
                    many cues arrive and what anybody wants to say about it is
                    how loud. The matrix it becomes is the same shape the
                    direct out gets, scaled.

                    AND IT IS BELOW THE CUE'S LEVEL, not beside it. The
                    coefficients here are summed by `CueMatrix` and the cue's
                    own level multiplies that sum, once, after all of them - so
                    a fade on the cue moves the direct out and every send
                    together, which is what a designer means by fading a cue
                    and what a console calls a DCA. Nothing here arranges that;
                    it falls out of where the level is applied. */
                const auto bus = busNamed (destination[juce::Identifier ("bus")]
                                             .toString().toStdString());

                if (! bus.isValid())
                {
                    problem = "send names no bus of this show";
                    return {};
                }

                /*  A LOCKED SHOW RIDES ITS SENDS LIVE (2026-09-25): a level or
                    a switch held in the layer is the one heard. */
                const auto sendId = destination.getProperty ("id").toString().toStdString();
                const auto* riding = liveLayer != nullptr ? liveLayer->sendOf (sendId) : nullptr;

                auto level = schema.number (destination, "send", "level");
                auto on = schema.flag (destination, "send", "on");

                if (riding != nullptr && riding->level.has_value())
                    level = osc::parseDouble (*riding->level).value_or (level);

                if (riding != nullptr && riding->on.has_value())
                    on = *riding->on == "true";

                /*  AND WHAT A FADE MOVED IT TO TONIGHT (namespace draft §26,
                    OZ): newer than the lock's ride, and the run's alone. Its
                    switch is the cue's still - a fade moves numbers (PB). */
                const auto busId = destination[juce::Identifier ("bus")].toString().toStdString();

                if (moved != nullptr)
                    if (const auto held = moved->find ("send/" + busId); held != moved->end())
                        level = held->second;

                /*  AND WHAT ITS LANE ASKS FOR AT THIS SECOND OF THE FILE
                    (namespace draft §28, PY): an offset on all of the above,
                    as the level lane is a term beside a fade and a trim - so
                    nought is the send as written, and the sum stays inside
                    the send's own range. */
                if (laneOffsets != nullptr)
                    if (const auto lane = laneOffsets->find (busId); lane != laneOffsets->end())
                        level = std::clamp (level + lane->second, silenceDb, 12.0);

                sentTo.push_back (busId);

                if (! sendInto (bus, level, on))
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

                const auto slotFirst = schema.integer (slot, "processorInput", "firstChannel");
                const auto slotWidth = schema.integer (slot, "processorInput", "width");
                const auto busFirst = schema.integer (bus, "bus", "firstChannel");
                const auto busWidth = schema.integer (bus, "bus", "width");

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

        /*  AND THE SENDS A LOCKED SHOW MADE LIVE (2026-09-25): mix channels
            this cue did not send to, turned up under the lock. A bus deleted
            since is simply not there - a live send is a ride, and a ride on a
            fader that has gone fails nothing. */
        if (liveLayer != nullptr)
        {
            const auto cueId = cue.getProperty ("id").toString().toStdString();

            for (const auto& sendId : liveLayer->createdSendsOf (cueId))
            {
                const auto* send = liveLayer->sendOf (sendId);
                const auto bus = send != nullptr ? busNamed (send->bus) : juce::ValueTree {};

                if (! bus.isValid())
                    continue;

                auto level = osc::parseDouble (send->level.value_or ("0")).value_or (0.0);
                const auto on = send->on.value_or ("true") == "true";

                if (moved != nullptr)
                    if (const auto held = moved->find ("send/" + send->bus); held != moved->end())
                        level = held->second;

                sentTo.push_back (send->bus);

                if (! sendInto (bus, level, on))
                    return {};
            }
        }

        /*  AND A MIX RIDDEN IN A PASS THAT THE CUE DOES NOT SEND TO (namespace
            draft §34, UQ): heard as the send the pass will give it - at nought,
            its lane the hand's - a send of the run alone until then. Only a
            hand puts a lane's term on a bus with no send. */
        if (laneOffsets != nullptr)
            for (const auto& [busId, offset] : *laneOffsets)
            {
                if (std::find (sentTo.begin(), sentTo.end(), busId) != sentTo.end())
                    continue;

                const auto bus = busNamed (busId);

                if (! bus.isValid())
                    continue;

                sentTo.push_back (busId);

                if (! sendInto (bus, std::clamp (offset, silenceDb, 12.0), true))
                    return {};
            }

        /*  AND THE MIXES A FADE BROUGHT IN FROM SILENCE (namespace draft §26,
            PB): a bus the cue has no send into, faded up tonight - a send of
            the run alone, which the cue never holds. A bus that has gone is
            not there, as a live send's is not. */
        if (moved != nullptr)
            for (const auto& [entry, level] : *moved)
            {
                if (entry.rfind ("send/", 0) != 0)
                    continue;

                const auto busId = entry.substr (5);

                if (std::find (sentTo.begin(), sentTo.end(), busId) != sentTo.end())
                    continue;

                const auto bus = busNamed (busId);

                if (bus.isValid() && ! sendInto (bus, level, true))
                    return {};
            }

        return out;
    }

    //==============================================================================
    void Runner::fireFade (const juce::ValueTree& cue, const std::string& runId, std::int64_t tick)
    {
        /*  A FADE ON A PICTURE (Phase 8a, namespace draft 36, VS) moves what
            its `video` pairs name, and nothing of a sound's. */
        if (fireVideoFade (cue, runId, tick))
            return;

        /*  A DRAWN CURVE, when there is one, is the whole of the shape: its last
            breakpoint is where the fade ends, and `level` and `curve` are not
            read (§14.6). Not combined - two shapes multiplied together are a
            third shape nobody drew.

            A list the write door and the loader both refuse cannot be here. One
            that got here anyway would come back with no points, and the fade
            would play as its two words say rather than do nothing on a show
            night - the reason fadeCurveFrom reads an unknown word as linear. */
        auto drawn = doc::readFadePoints (textOf (cue, "points"));

        const auto toDb = drawn.points.empty() ? numberOf (cue, "level")
                                               : drawn.points.back().levelDb;

        /*  WHAT IT MOVES (namespace draft §22.6): the level unless `levelOn`
            says not, the speed when `rateOn` says so - one fade, either or both,
            QLab's shape. A fade that moves neither is a fade that moves
            nothing: its run ends at once, and `wfg validate` says why. So is a
            fade on a DCA with its level switch off: a DCA has no speed (EC),
            and its `target` is not read. */
        const auto levelOn = textOf (cue, "levelOn") != "false";
        const auto rateOn = textOf (cue, "rateOn") == "true";

        /*  AND WHAT ELSE OF THE TARGET'S IT MOVES (namespace draft §26): its
            sends, EQ numbers and plugin values, each a job of its own under
            this fade's one report (PC). A DCA has none of them. */
        const auto moves = textOf (cue, "dca").empty()
                             ? readFadeMoves (textOf (cue, "sends"), textOf (cue, "eq"), textOf (cue, "fx"))
                             : std::vector<FadeMove> {};

        if (! levelOn && ((! rateOn && moves.empty()) || ! textOf (cue, "dca").empty()))
        {
            if (auto* selfRun = runs.find (runId))
                selfRun->state = runState::playing;

            FadeJob nothing;
            nothing.self = runId;
            running.push_back (nothing);
            return;
        }

        /*  A FADE THAT NAMES A DCA MOVES THE DCA (Phase 6, `fade/dca`), and
            `target` is not read: a DCA has no run to find, and a fade that
            named both would otherwise have to choose which of two levels it
            meant. `wfg validate` says so when both are written. A DCA has no
            speed, so `rateOn` there moves nothing (EC). */
        if (const auto dcaId = textOf (cue, "dca"); ! dcaId.empty())
        {
            beginDcaFade (tick, dcaId, runId, toDb, numberOf (cue, "duration"),
                          fadeCurveFrom (textOf (cue, "curve")), std::move (drawn.points));
            return;
        }

        /*  AND WHETHER ARRIVING IS STOPPING (author, 2026-09-18: "a tick box
            to stop a media file once a fade has completed"). Until then a
            fade never stopped anything, even at silence: the run played on,
            silently, until it ended by itself - right for a fade that will
            come back up, and a surprise for the common fade-out. `false`
            reads as the default the writer omits, so an unsaid box is off. */
        const auto stopWhenDone = textOf (cue, "stopWhenDone") == "true";

        /*  THE SPEED FIRST, so the level knows whether to wait for it: a
            speed is heard a horizon after the tick that moved it, so a fade
            that moves both and stops at the end stops once the speed has
            arrived too (ED). The level's job carries that stop; a fade that
            moves only the speed carries its own. Read with one as the
            fallback, never nought, for the reason the cue's own speed is. */
        const auto speedMoves = rateOn
                                  && beginRateFade (tick, textOf (cue, "target"), runId,
                                                    osc::parseDouble (textOf (cue, "rate")).value_or (1.0),
                                                    numberOf (cue, "duration"),
                                                    fadeCurveFrom (textOf (cue, "curve")),
                                                    stopWhenDone && ! levelOn,
                                                    ! levelOn && moves.empty());

        /*  THE TARGET'S OWN NUMBERS (§26), carrying the stop only when
            neither the level nor the speed does; and saying "nothing to move"
            only when nothing else here moves either. */
        if (! moves.empty())
            beginMoveFades (tick, textOf (cue, "target"), runId, moves,
                            numberOf (cue, "duration"), fadeCurveFrom (textOf (cue, "curve")),
                            stopWhenDone && ! levelOn && ! speedMoves,
                            ! levelOn && ! speedMoves);

        if (levelOn)
            beginFade (tick, cue[idProperty].toString().toStdString(),
                              textOf (cue, "target"),
                              runId, "fade",
                              toDb,
                              numberOf (cue, "duration"),
                              fadeCurveFrom (textOf (cue, "curve")),
                              stopWhenDone,
                              std::move (drawn.points),
                              speedMoves ? latencyTicks() + 1 : 0);
    }

    int Runner::advanceTargetOf (const juce::ValueTree& cue, const Run& run) const
    {
        /*  WHICH SLICE `transport/range` NAMES, as a place in the playlist -
            and -1, meaning the next one, for every transport cue written
            before there was anything to name and for every one that names
            nothing.

            REFUSED RATHER THAN GUESSED, in three ways, and each leaves the
            advance doing what it always did rather than doing nothing: a slice
            of some OTHER cue is a reference somebody has to fix and the run
            still has somewhere to go; the slice it is already on is a request
            with no boundary in it; and a slice past the graph's slots has no
            clip to launch, which is the `no-slot` refusal one step earlier and
            quieter. */
        const auto wanted = textOf (cue, "range");

        if (wanted.empty())
            return -1;

        const auto target = document.findById (run.cue);

        if (! target.isValid())
            return -1;

        auto at = 0;

        for (const auto& child : target)
        {
            if (child.getType().toString() != "Range")
                continue;

            if (child[idProperty].toString().toStdString() == wanted)
            {
                if (at == run.range)
                    return -1;          // already there, so the next one is the answer

                return audio != nullptr && at >= audio->slotCount() ? -1 : at;
            }

            ++at;
        }

        return -1;                      // a slice of some other cue, or of none
    }

    void Runner::fireStop (const juce::ValueTree& cue, const std::string& runId, std::int64_t tick)
    {
        const auto verb = textOf (cue, "verb");

        /*  THREE ARE NOT STOPS EITHER (2026-10-05, namespace draft §27).
            Enable and disable switch their target for this run, now, and stop
            nothing (PQ). Jump moves standby, on the next tick, through
            `standby.jump` - the start cue's road, so a replay takes the record
            the session logged and fires nothing of its own. The cue's own run
            is over either way. */
        if (verb == "enable" || verb == "disable")
        {
            switchOverride (cue, runId);
            finishing.push_back (runId);
            return;
        }

        if (verb == "jump")
        {
            const auto target = textOf (cue, "target");
            const auto cueId = cue[idProperty].toString().toStdString();
            const auto andGo = textOf (cue, "andGo") == "true";

            /*  A JUMP-AND-GO AIMED AT ITSELF fires itself for ever, a tick
                apart, and is applied and does nothing (PU). A jump back to an
                earlier cue that comes round to this one again is the designer's
                loop, and is let be. */
            if (! target.empty() && ! (andGo && target == cueId))
                jumpsToMake.push_back ({ listOfCue (cueId), target, andGo, goOfRun (runId) });

            finishing.push_back (runId);
            return;
        }

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
                        /*  AS IT WAS, for Doh! (D3): withdrawn while its
                            boundary is still to be placed. */
                        noteFlag (*run, runId);

                        run->advanceRequested = true;
                        run->advanceTo = advanceTargetOf (cue, *run);
                        finishing.push_back (runId);
                        return;
                    }

                    /*  Nothing to advance out of. A hard stop, for the reason
                        `afterMember` is one against a cue that is not a group:
                        there is no boundary to wait for, and a request quietly
                        ignored is worse than one honoured plainly. */
                    noteHardStop (*run, runId, tick);
                    run = runs.find (target->id);
                    run->askStop (goOfRun (runId));
                }

            finishing.push_back (runId);
            return;
        }

        /*  FOUR ARE NOT STOPS AT ALL (Phase 9c, namespace draft §19.6):
            record, loop, overdub and clear are the press of that name on the
            take of the sampling channel the target is sounding through. A
            target that is not a sounding mic cue on a sampling channel is the
            transport cue's own rule for a target that is not running: applied,
            and nothing done - and so is a layer with every one in use. */
        if (auto press = TakeVerb::record; verb != "undo" && takeVerbOf (verb, press))
        {
            const auto* target = runs.liveRunOf (textOf (cue, "target"));

            if (target != nullptr && takes != nullptr && target->kind == "mic"
                  && target->state == runState::playing)
            {
                const auto channelId = textOf (document.findById (target->cue), "channel");
                const auto channel = samplingChannelOf (document, channelId);
                const auto full = takes->wouldStartLayer (channelId, press)
                                    && takes->of (channelId).layers >= channel.layers;

                if (channel.samples() && ! full)
                {
                    const auto before = takes->of (channelId);
                    takes->press (channelId, press, channel.layers);

                    /*  A PRESS OF THE GO'S, for Doh! to undo by its inverse (D3). */
                    noteTake (runId, channelId, press, before);
                }
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
                        noteFlag (*run, runId);

                        run->stopAfter = verb == "afterMember" ? "member" : "iteration";
                        finishing.push_back (runId);
                        return;
                    }

                    noteHardStop (*run, runId, tick);
                    run = runs.find (target->id);
                    run->askStop (goOfRun (runId));
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

        beginFade (tick, cue[idProperty].toString().toStdString(),
                          textOf (cue, "target"),
                          runId, "transport",
                          silenceDb, seconds,
                          fadeCurveFrom (textOf (cue, "curve")),
                          true, {});
    }

    Runner::Takeover Runner::resolveTakeover (const std::string& targetId)
    {
        /*  WHAT THE JOBS ALREADY ON THIS TARGET MEANT, and what of it survives.

            Two separate things come out of this, and keeping them apart is why
            it is a function: which RUNS are over (their work belongs to somebody
            else now) and which SCHEDULE is inherited (a stop that was already
            coming). The first is about lifetime; the second is about time. */
        Takeover out;
        std::vector<std::string> supersededSelves;

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
            supersededSelves.push_back (superseded.self);

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

        /*  A FADE CUE THAT MOVES TWO THINGS ENDS WHEN BOTH ARE TAKEN (namespace
            draft §22.6): a level fade over one that also moves the speed takes
            the level and leaves the speed moving, and the older fade's run goes
            on until that job is done too. */
        for (const auto& self : supersededSelves)
            if (std::none_of (running.begin(), running.end(),
                              [&self] (const FadeJob& job) { return job.self == self; }))
                supersededRuns.push_back (self);

        return out;
    }

    void Runner::beginFade (std::int64_t tick,
                            const std::string& selfCueId,
                            const std::string& targetCueId,
                            const std::string& selfRunId, const std::string& kind,
                            double toDb, double seconds, FadeCurve curve,
                            bool stopWhenDone, std::vector<doc::FadePoint> points,
                            int stopLagTicks)
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

        /*  A FADE OF THE GO ON SOMETHING THE GO DID NOT START (2026-10-03, Doh!
            D3, namespace draft §24.13): what Doh! brings the level back from -
            where it stood, and the fade that was moving it, copied before the
            takeover below erases it. Hook-consumed, the level and the job; the
            stop's account below is the handler's. */
        const auto touched = ofTheOpenGo (self) && goOfRun (targetId) != goRecord.serial;
        std::optional<FadeJob> movingBefore;

        if (touched)
            for (const auto& before : running)
                if (before.target == targetId && ! before.retired && goOfRun (before.self) != goRecord.serial)
                    movingBefore = before;

        const auto wasStopping = target->stopAsked;
        const auto wasWaiting = target->state == runState::waiting;
        const auto wasPostWait = target->state == runState::postWait;
        const auto dueBefore = target->dueTick;
        const auto targetKind = target->kind;
        const auto targetParent = target->parent;

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
        job.points = std::move (points);
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
        /*  AND A STOP OVER A STOP LANDS AT THE SOONER OF THE TWO (2026-10-02,
            K3's review, namespace draft §23.14, KS). The takeover keeps a stop
            already coming from being called off by a FADE; it was never meant
            to put off a second stop that is due first. A `hard` stop over a
            ten-second fade-out took the level to silence at once and left the
            cue - since K3, a whole scene - playing silently to the tenth
            second. */
        if (takeover.keepStopping)
        {
            job.stopWhenDone = true;
            job.stopsAtTick = stopWhenDone
                                ? std::min (takeover.stopsAtTick, tick + job.ticksTotal + stopLagTicks)
                                : takeover.stopsAtTick;
        }
        else if (stopWhenDone)
        {
            /*  A stop of its own, landing when its own fade arrives. The two
                are the same number here and diverge only when somebody fades
                over the top of it - or when the same fade moves the speed,
                which is heard later (ED). */
            job.stopsAtTick = tick + job.ticksTotal + stopLagTicks;
        }

        /*  A MIC CUE FADED OUT IS ITS INPUT FADED (Phase 9b, namespace draft
            §18.5): the level moves into the chain rather than out of it, so
            the channel's reverb rings on at the cue's level after the voice
            has gone. The output stays where it is - here and on a replay, which
            publishes the same level - and the stop at the end lets the tail
            ring out. */
        if (stopWhenDone && target->kind == "mic")
        {
            job.toDb = fromDb;

            if (audio != nullptr && target->track >= 0)
                audio->shutLive (target->track, seconds);
        }

        /*  A cue on its way out says so from the moment it is asked, not when
            the sound goes. `done` here would publish a silence that has not
            happened yet. ASKED, and by whom (2026-10-01, namespace draft §24):
            the GO of the cue doing the stopping, so a Doh can tell its own
            GO's stop from another hand's.

            A GROUP SAYS SO TOO, and its scene plays on regardless (2026-10-02,
            K3, namespace draft §23.14): this job holds the group to the fade's
            end as `enforceStops` holds a voice, and the group's own job waits
            for it before stopping any member (`fadingToItsStop`). Not an abort,
            so no `stopEndsWait` on the group (K2, JX). */
        if (stopWhenDone)
            if (auto* stopping = runs.find (targetId))
                stopping->askStop (goOfRun (self));

        if (touched)
        {
            noteLevelTouch (targetId, targetId, {}, false, fromDb, job.toDb, tick, movingBefore);

            if (stopWhenDone)
                noteGoStop ({ targetId, self, targetKind, tick, tick + std::max (job.ticksTotal, 1),
                              job.stopsAtTick, wasStopping, wasWaiting, dueBefore, fromDb,
                              wasPostWait, job.ticksTotal > 0 },
                            targetParent);
        }

        running.push_back (job);
    }

    bool Runner::beginRateFade (std::int64_t tick,
                                const std::string& targetCueId, const std::string& selfRunId,
                                double toRate, double seconds, FadeCurve curve,
                                bool stopWhenDone, bool alone)
    {
        auto* selfRun = runs.find (selfRunId);

        if (selfRun == nullptr)
            return false;

        selfRun->state = runState::playing;

        /*  NOTHING TO MOVE, said once: by this job when the fade moves only the
            speed, by the level's job when it moves both. */
        const auto nothing = [&] (std::string failure)
        {
            if (! alone)
                return false;

            FadeJob orphan;
            orphan.self = selfRunId;
            orphan.failure = std::move (failure);
            running.push_back (orphan);
            return false;
        };

        if (! targetCueId.empty() && ! document.findById (targetCueId).isValid())
            return nothing (runError::badTarget);

        /*  A MEDIA CUE'S, and one that is running (§3.8's silent no-op). A
            group, a mic cue, a memo: nothing there has a speed to move (EC). */
        const auto* live = runs.liveRunOf (targetCueId);

        if (live == nullptr || live->kind != "media")
            return nothing ({});

        const auto targetId = live->id;

        /*  A SPEED THE GO MOVED (Doh! D3), as a level is noted in `beginFade`. */
        const auto touched = ofTheOpenGo (selfRunId) && goOfRun (targetId) != goRecord.serial;
        std::optional<FadeJob> movingBefore;

        if (touched)
            for (const auto& before : running)
                if (before.target == "rate:" + targetId && ! before.retired
                      && goOfRun (before.self) != goRecord.serial)
                    movingBefore = before;

        const auto wasStopping = live->stopAsked;
        const auto wasWaiting = live->state == runState::waiting;
        const auto wasPostWait = live->state == runState::postWait;
        const auto dueBefore = live->dueTick;
        const auto levelBefore = live->ownLevel;
        const auto targetParent = live->parent;

        const auto takeover = resolveTakeover ("rate:" + targetId);
        auto* target = runs.find (targetId);

        if (target == nullptr)
            return false;

        FadeJob job;
        job.target = "rate:" + targetId;
        job.self = selfRunId;
        job.movesRate = true;
        job.fromRate = target->ownRate;

        /*  Nought to twenty, as the row is, whatever reached here - and a
            stretched cue held to the stretcher's limit, as its own speed is,
            so its clock and its voice agree. */
        const auto stretchLimit = audio != nullptr ? audio->stretchSpeedLimit() : 0.0;
        const auto wanted = std::isfinite (toRate) ? std::clamp (toRate, -20.0, 20.0) : 1.0;   // through nought: backwards (§41)
        job.toRate = target->stretch && stretchLimit > 0.0
                       ? std::copysign (std::min (std::abs (wanted), stretchLimit), wanted)
                       : wanted;
        job.ticksTotal = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        job.curve = curve;
        job.stopWhenDone = stopWhenDone;

        /*  A STOP ONCE THE SPEED HAS ARRIVED (ED). The last speed is placed a
            horizon ahead of the tick that wrote it and reached over one more,
            and a stop is heard at the next block: stopped with the fade's last
            tick, the voice would never play the end of the ramp. A stop this
            job inherited keeps its own tick, as a level's does - unless this
            job stops too, and sooner (KS). */
        if (takeover.keepStopping)
        {
            job.stopWhenDone = true;
            job.stopsAtTick = stopWhenDone
                                ? std::min (takeover.stopsAtTick, tick + job.ticksTotal + latencyTicks() + 1)
                                : takeover.stopsAtTick;
        }
        else if (stopWhenDone)
        {
            job.stopsAtTick = tick + job.ticksTotal + latencyTicks() + 1;
        }

        if (job.stopWhenDone)
            target->askStop (goOfRun (selfRunId));

        if (touched)
        {
            noteLevelTouch ("rate:" + targetId, targetId, {}, true, job.fromRate, job.toRate, tick, movingBefore);

            if (stopWhenDone)
                noteGoStop ({ targetId, selfRunId, "media", tick, tick + std::max (job.ticksTotal, 1),
                              job.stopsAtTick, wasStopping, wasWaiting, dueBefore, levelBefore,
                              wasPostWait, job.ticksTotal > 0 },
                            targetParent);
        }

        running.push_back (job);
        return true;
    }

    void Runner::beginDcaFade (std::int64_t tick,
                               const std::string& dcaId, const std::string& selfRunId,
                               double toDb, double seconds, FadeCurve curve,
                               std::vector<doc::FadePoint> points)
    {
        auto* selfRun = runs.find (selfRunId);

        if (selfRun == nullptr)
            return;

        /*  Running from the tick it starts, for the reason a cue fade is: a
            fade has nothing to arm. */
        selfRun->state = runState::playing;

        /*  A DCA THIS SHOW DOES NOT DECLARE is a pointer at nothing, the
            `bad-target` a cue fade gives for a cue that is not there - said
            out loud from the tick hook, as every report is. */
        const auto declared = document.findById (dcaId);

        if (! declared.isValid() || declared.getType().toString() != "Dca")
        {
            FadeJob orphan;
            orphan.self = selfRunId;
            orphan.failure = runError::badTarget;
            running.push_back (orphan);
            return;
        }

        /*  THE TAKEOVER KEY IS THE DCA, spelled so no run identifier can be
            it: a fade over a fade on one DCA begins from where the first had
            got to, exactly as a fade over a fade on one cue does. */
        const auto key = "dca:" + dcaId;

        /*  A TRIM THE GO MOVED (Doh! D3), noted before the takeover erases the
            fade that was moving it. */
        const auto touched = ofTheOpenGo (selfRunId);
        std::optional<FadeJob> movingBefore;

        if (touched)
            for (const auto& before : running)
                if (before.target == key && ! before.retired && goOfRun (before.self) != goRecord.serial)
                    movingBefore = before;

        resolveTakeover (key);

        FadeJob job;
        job.target = key;
        job.dca = dcaId;
        job.self = selfRunId;
        job.fromDb = dcas != nullptr ? dcas->trimOf (dcaId) : 0.0;
        job.handSerial = dcas != nullptr ? dcas->handSerialOf (dcaId) : 0;
        job.toDb = toDb;
        job.ticksTotal = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        job.curve = curve;
        job.points = std::move (points);

        if (touched)
            noteLevelTouch (key, {}, dcaId, false, job.fromDb, job.toDb, tick, movingBefore);

        running.push_back (job);
    }

    bool Runner::beginMoveFades (std::int64_t tick,
                                 const std::string& targetCueId, const std::string& selfRunId,
                                 const std::vector<FadeMove>& moves, double seconds, FadeCurve curve,
                                 bool stopWhenDone, bool alone)
    {
        auto* selfRun = runs.find (selfRunId);

        if (selfRun == nullptr)
            return false;

        selfRun->state = runState::playing;

        //  Nothing to move, said once - by this when nothing else here moves.
        const auto nothing = [&] (std::string failure)
        {
            if (! alone)
                return false;

            FadeJob orphan;
            orphan.self = selfRunId;
            orphan.failure = std::move (failure);
            running.push_back (orphan);
            return false;
        };

        if (! targetCueId.empty() && ! document.findById (targetCueId).isValid())
            return nothing (runError::badTarget);

        /*  A MEDIA OR A MIC RUN, sounding (§3.8's silent no-op otherwise): a
            group has no sends, EQ or inserts of its own (§4.12), and a memo
            has no sound. */
        const auto* live = runs.liveRunOf (targetCueId);

        if (live == nullptr || (live->kind != "media" && live->kind != "mic"))
            return nothing ({});

        const auto targetId = live->id;
        const auto touched = ofTheOpenGo (selfRunId) && goOfRun (targetId) != goRecord.serial;
        const auto ticks = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        auto first = true;

        for (const auto& move : moves)
        {
            const auto key = "move:" + targetId + ":" + move.entry;
            std::optional<FadeJob> movingBefore;

            if (touched)
                for (const auto& before : running)
                    if (before.target == key && ! before.retired && goOfRun (before.self) != goRecord.serial)
                        movingBefore = before;

            /*  FROM WHERE IT HAS GOT TO, read before the takeover lets the job
                moving it go - the value it holds is the run's either way. */
            auto* target = runs.find (targetId);

            if (target == nullptr)
                return ! first;

            const auto from = movedValueNow (*target, move.entry);
            const auto takeover = resolveTakeover (key);

            target = runs.find (targetId);

            if (target == nullptr)
                return ! first;

            FadeJob job;
            job.target = key;
            job.self = selfRunId;
            job.move = move.entry;
            job.moveRun = targetId;
            job.moveDomain = move.domain;
            job.fromValue = std::isfinite (from) ? from : move.value;
            job.toValue = move.value;
            job.ticksTotal = ticks;
            job.curve = curve;

            /*  THE STOP, on the first job only, and one a takeover inherited
                keeps its tick (KS), as a level's does. */
            const auto stopsHere = stopWhenDone && first;

            if (takeover.keepStopping)
            {
                job.stopWhenDone = true;
                job.stopsAtTick = stopsHere ? std::min (takeover.stopsAtTick, tick + ticks)
                                            : takeover.stopsAtTick;
            }
            else if (stopsHere)
            {
                job.stopWhenDone = true;
                job.stopsAtTick = tick + ticks;
            }

            if (job.stopWhenDone)
                target->askStop (goOfRun (selfRunId));

            /*  A VALUE THE GO MOVED (Doh! D3, namespace draft §26 PE), as a
                level is noted in `beginFade`: put back with it. */
            if (touched)
            {
                noteLevelTouch (key, targetId, {}, false, job.fromValue, job.toValue, tick, movingBefore);

                for (auto& note : goChanges.levels)
                    if (note.key == key)
                    {
                        note.move = move.entry;
                        note.moveDomain = move.domain;
                    }
            }

            running.push_back (job);
            first = false;
        }

        return ! first;
    }

    double Runner::movedValueNow (const Run& run, const std::string& entry) const
    {
        if (const auto held = run.moved.find (entry); held != run.moved.end())
            return held->second;

        static const Reader schema;
        const auto cue = document.findById (run.cue);
        const auto cueId = run.cue;

        std::string kind, name;
        int index = -1;

        if (! cue.isValid() || ! splitMoveEntry (entry, kind, name, index))
            return std::numeric_limits<double>::quiet_NaN();

        if (kind == "send")
        {
            for (const auto& child : cue)
            {
                if (! child.hasType ("Send") || child.getProperty ("bus").toString().toStdString() != name)
                    continue;

                const auto sendId = child.getProperty ("id").toString().toStdString();

                if (liveLayer != nullptr)
                    if (const auto* riding = liveLayer->sendOf (sendId); riding != nullptr && riding->level.has_value())
                        if (const auto level = osc::parseDouble (*riding->level))
                            return *level;

                return schema.number (child, "send", "level");
            }

            if (liveLayer != nullptr)
                for (const auto& sendId : liveLayer->createdSendsOf (cueId))
                    if (const auto* send = liveLayer->sendOf (sendId); send != nullptr && send->bus == name)
                        return osc::parseDouble (send->level.value_or ("0")).value_or (0.0);

            //  A mix the cue does not send into is silence, and fades up from it (PB).
            return -120.0;
        }

        if (kind == "eq")
        {
            if (liveLayer != nullptr)
                if (const auto* riding = liveLayer->rowOf (cueId, name))
                    if (const auto value = osc::parseDouble (*riding))
                        return *value;

            return schema.number (cue, "sound", name.c_str());
        }

        //  A plugin value: the cue's insert of that entry, then what it rides.
        for (const auto& child : cue)
        {
            if (! child.hasType ("Fx") || child.getProperty ("plugin").toString().toStdString() != name)
                continue;

            const auto fxId = child.getProperty ("id").toString().toStdString();

            if (liveLayer != nullptr)
                if (const auto* riding = liveLayer->fxValuesOf (fxId))
                    if (const auto value = riding->find (index); value != riding->end())
                        return value->second;

            const auto values = parseFxValues (schema.text (child, "fx", "values"));

            if (const auto value = values.find (index); value != values.end())
                return value->second;

            break;
        }

        /*  A PARAMETER THE CUE NEVER SET rests at the entry's preset, which
            only the plugin knows; the catalogue's default is the nearest this
            side can say. With no catalogue it is not known at all, and the
            fade starts where it ends. */
        if (catalogues != nullptr)
        {
            const auto setEntry = document.findById (name);

            if (setEntry.isValid() && setEntry.hasType ("Plugin"))
                if (const auto catalogue = catalogues->find (setEntry.getProperty ("identifier").toString().toStdString()))
                    if (index >= 0 && index < static_cast<int> (catalogue->params.size()))
                        return static_cast<double> (catalogue->params[static_cast<std::size_t> (index)].defaultValue);
        }

        return std::numeric_limits<double>::quiet_NaN();
    }

    void Runner::releaseMovesOf (const std::string& runId)
    {
        std::vector<std::string> keys;
        const auto head = "move:" + runId + ":";

        for (const auto& job : running)
            if (job.target.rfind (head, 0) == 0
                  && std::find (keys.begin(), keys.end(), job.target) == keys.end())
                keys.push_back (job.target);

        for (const auto& key : keys)
            resolveTakeover (key);
    }

    std::string Runner::moveWords (const std::string& entry) const
    {
        std::string kind, name;
        int index = -1;

        if (! splitMoveEntry (entry, kind, name, index))
            return entry;

        if (kind == "send")
            return "send to " + deviceLabel ("bus", name);

        if (kind == "fx")
            return deviceLabel ("plugin", name) + " parameter " + std::to_string (index + 1);

        //  eqB2Gain -> "EQ band 2 gain", eqHpfFreq -> "EQ high-pass frequency".
        if (name == "eqHpfFreq")
            return "EQ high-pass frequency";

        if (name == "eqLpfFreq")
            return "EQ low-pass frequency";

        const auto suffix = name.substr (4);
        return std::string ("EQ band ") + name[3] + " "
                 + (suffix == "Gain" ? "gain" : suffix == "Q" ? "width" : "frequency");
    }

    const std::vector<std::string>& Runner::dcaChainOf (const std::string& cueId)
    {
        /*  READ ONCE PER SHOW REVISION, because it is a walk of the document
            and this is asked for every run on every tick. A mark or a nesting
            is an edit to the show, and an edit moves the revision; nothing
            else can change the answer. */
        if (! dcaChainsRead || dcaChainsRevision != document.showRevision())
        {
            dcaChains.clear();
            dcaChainsRead = true;
            dcaChainsRevision = document.showRevision();

            const auto root = document.root();
            const juce::Identifier dcaProperty { "dca" };
            const auto chainFrom = [this] (std::string first) { return dcaNestingFrom (std::move (first)); };

            std::function<void (const juce::ValueTree&)> visit;
            visit = [this, &visit, &chainFrom, &dcaProperty] (const juce::ValueTree& node)
            {
                const auto element = node.getType().toString();

                /*  ONLY MEDIA AND GROUPS CARRY A MARK (PRD §3.28): audio and
                    video cues, and groups. A fade's `dca` is what it moves and
                    a strip's is what it rides - neither is membership. */
                if (element == "Media" || element == "Mic" || element == "Video" || element == "Group")
                    if (const auto mark = node[dcaProperty].toString().toStdString(); ! mark.empty())
                        if (auto chain = chainFrom (mark); ! chain.empty())
                            dcaChains[node[idProperty].toString().toStdString()] = std::move (chain);

                for (const auto& child : node)
                    visit (child);
            };

            if (const auto showLists = root.getChildWithName ("Lists"); showLists.isValid())
                visit (showLists);
        }

        static const std::vector<std::string> none;
        const auto found = dcaChains.find (cueId);
        return found != dcaChains.end() ? found->second : none;
    }

    std::vector<std::string> Runner::dcaNestingFrom (std::string first) const
    {
        /*  UP THE NESTING, bounded by how many DCAs there are: the door
            refuses a circle and so does the reader, but a sum walked here
            must not be the thing that finds out it did not. */
        const auto dcaList = document.root().getChildWithName ("Dcas");
        const juce::Identifier dcaProperty { "dca" };

        std::vector<std::string> chain;
        const auto bound = dcaList.isValid() ? dcaList.getNumChildren() : 0;

        for (int steps = 0; steps < bound && ! first.empty(); ++steps)
        {
            const auto node = dcaList.getChildWithProperty (idProperty, juce::String (first));

            if (! node.isValid())
                break;

            chain.push_back (first);
            first = node[dcaProperty].toString().toStdString();
        }

        return chain;
    }

    void Runner::applyOutputLevels()
    {
        if (audio == nullptr)
            return;

        /*  READ ONCE PER SHOW REVISION, as a cue's chain is: an output's
            channels, its trim and its mark change only by an edit. */
        if (! outputGainsRead || outputGainsRevision != document.showRevision())
        {
            outputGainSources.clear();
            outputGainsRead = true;
            outputGainsRevision = document.showRevision();

            if (const auto audioNode = document.root().getChildWithName ("Audio"); audioNode.isValid())
            {
                for (const auto& bus : audioNode)
                {
                    if (! bus.hasType ("Bus"))
                        continue;

                    OutputGainSource source;
                    source.firstChannel = static_cast<int> (bus.getProperty ("firstChannel", 0));
                    source.width = static_cast<int> (bus.getProperty ("width", 1));
                    source.trimDb = static_cast<double> (bus.getProperty ("trim", 0.0));
                    source.dcaChain = dcaNestingFrom (bus.getProperty ("dca").toString().toStdString());
                    outputGainSources.push_back (std::move (source));
                }
            }
        }

        for (const auto& source : outputGainSources)
        {
            auto gainDb = source.trimDb;

            if (dcas != nullptr)
                for (const auto& dcaId : source.dcaChain)
                    gainDb += dcas->trimOf (dcaId);

            audio->setOutputGainDb (source.firstChannel, source.width, gainDb);
        }
    }

    //==============================================================================
    double Runner::levelForByte (int byte, double floor) noexcept
    {
        if (byte >= 127)
            return 0.0;

        if (byte <= 1)
            return floor;

        return floor + (0.0 - floor) * static_cast<double> (byte - 1) / 126.0;
    }

    const std::vector<std::string>& Runner::samplerStrips()
    {
        /*  READ ONCE PER SHOW REVISION: a strip's role and a surface's order
            are edits to the show, and an edit moves the revision. */
        if (! rosterRead || rosterRevision != document.showRevision())
        {
            rosterRead = true;
            rosterRevision = document.showRevision();
            samplerRoster.clear();
            gateStrips.clear();

            for (const auto& box : document.root().getChildWithName ("Surfaces"))
            {
                if (box.getType().toString() != "Surface")
                    continue;

                const auto surfaceId = box[idProperty].toString().toStdString();
                const auto profile = document.getAttribute ("/godot/surface/" + surfaceId + "/profile")
                                         .value_or (std::string {});

                for (const auto& strip : box)
                {
                    if (strip.getType().toString() != "Strip")
                        continue;

                    const auto stripId = strip[idProperty].toString().toStdString();

                    if (document.getAttribute ("/godot/slot/" + stripId + "/role")
                          .value_or (std::string {}) != "sampler")
                        continue;

                    samplerRoster.push_back (stripId);

                    if (profile == "midiPads")
                        gateStrips.insert (stripId);
                }
            }
        }

        return samplerRoster;
    }

    std::string Runner::stripForMember (const juce::ValueTree& group, const std::string& cueId)
    {
        /*  A MEMBER'S OWN STRIP FIRST, THEN POSITIONAL (plan decision 3, and
            the pin the author asked for on 2026-09-25): `placeMembers` is the
            one rule, shared with the re-arm below and with the tree's
            `stripNow`, so the strip a menu says is the strip that is armed.
            Counted over media members only, because a sampler's members are
            clips (§3.27) and a memo among them would otherwise take a strip
            nothing could be played from. */
        for (const auto& member : placeMembers (document, group, samplerStrips()))
            if (member.cue == cueId)
                return member.strip;

        return {};
    }

    void Runner::claimStripFor (Run& run, const std::string& stripId)
    {
        run.strip = stripId;

        if (stripId.empty())
            return;

        const auto* holder = runs.holderOf (stripId);

        if (holder == nullptr)
        {
            run.claims.push_back (stripId);
            return;
        }

        if (holder->id == run.id)
            return;

        /*  BUSY: WAIT FOR IT, and the group holding it has lost it (PRD §3.27,
            `takeover=strip`). The claim is the slot table's own waiting claim
            (§3.9e): it lands when the holder's run ends, however it ends, and
            meanwhile this member shows pending. Under `takeover=group` the
            holding group is already closing, so marking the strip lost changes
            nothing it was going to do. A member of the SAME group never takes
            from itself - a re-arm finds its strip free or its own. */
        run.pending.push_back (stripId);

        if (auto* holderGroup = runs.find (holder->parent);
            holderGroup != nullptr && holderGroup->id != run.parent)
            if (std::find (holderGroup->lostStrips.begin(), holderGroup->lostStrips.end(), stripId)
                  == holderGroup->lostStrips.end())
            {
                holderGroup->lostStrips.push_back (stripId);

                /*  TAKEN FROM ANOTHER BANK BY A MEMBER OF THE GO'S (Doh! D3):
                    given back when the Doh takes the GO back. */
                if (ofTheOpenGo (run.id) && goChanges.lostStrips.size() < changesKept)
                    goChanges.lostStrips.emplace_back (holderGroup->id, stripId);
            }
    }

    void Runner::takeOverFrom (const std::string& groupRunId, const juce::ValueTree& group)
    {
        /*  `takeover=group` CLOSES EVERY OTHER ARMED SAMPLER GROUP (PRD §3.27):
            each launches nothing new and plays out what is playing - the
            close of §3.9e, not a kill. `strip` closes nothing here: it takes
            strips one claim at a time, in `claimStripFor`. */
        if (textOf (group, "takeover") != "group")
            return;

        for (const auto& other : runs.all())
        {
            if (other.id == groupRunId || other.isFinished() || ! other.isGroup()
                 || other.state == runState::preparing)
                continue;

            const auto otherCue = document.findById (other.cue);

            if (! otherCue.isValid() || textOf (otherCue, "mode") != "sampler")
                continue;

            if (auto* closing = runs.find (other.id))
            {
                /*  CLOSED BY THE GO (Doh! D3): opened again by the Doh, or fired
                    again when it has finished by then. */
                if (! closing->closing && ofTheOpenGo (groupRunId) && goChanges.closedSamplers.size() < changesKept)
                    goChanges.closedSamplers.push_back (other.id);

                closing->closing = true;
            }
        }
    }

    void Runner::samplerTick (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                              const Run& groupRun)
    {
        /*  IDLE IS ARMED AND NOT ASKED FOR: nothing has been launched, so
            ending it loses no sound and hands its strip over at once. */
        const auto idle = [] (const Run& child)
        {
            return child.state == runState::armed && ! child.launchRequested;
        };

        const auto lost = [&groupRun] (const std::string& stripId)
        {
            return std::find (groupRun.lostStrips.begin(), groupRun.lostStrips.end(), stripId)
                     != groupRun.lostStrips.end();
        };

        std::map<std::string, const Run*> live;

        for (const auto* child : runs.childrenOf (job.run))
            if (! child->isFinished() && child->sampler)
                live[child->cue] = child;

        /*  TAKEN OVER: A CLOSE, NOT A KILL (§3.9e). Nothing new is armed; an
            idle member is ended so its strip goes to the group that took it at
            once; a clip that is sounding plays out, and its strip changes
            hands when its run ends. The group completes - its footer, then
            its end - when its last member has. */
        if (groupRun.closing)
        {
            for (const auto& entry : live)
                if (idle (*entry.second))
                    engine.submit (origin::engine, "run.kill", one (entry.second->id));

            if (live.empty())
                finishPhase (engine, job, group);

            return;
        }

        /*  A STRIP ANOTHER GROUP TOOK, under `takeover=strip`: the idle member
            on it is ended the same way, and nothing is armed there again. */
        for (const auto& entry : live)
            if (lost (entry.second->strip) && idle (*entry.second))
                engine.submit (origin::engine, "run.kill", one (entry.second->id));

        /*  FIRST, A VOICE FOR EACH MEMBER THAT HAS ITS STRIP AND NO TRACK, as
            tracks free - in the order the runs were made, and no more of them
            in one tick than there are tracks free, so a bank waiting on one
            voice does not queue eight asks for it. BEFORE the re-arms below,
            and the order is the fairness: a member that has been waiting takes
            a freed track ahead of one whose clip only just ended. With no audio
            side there are no tracks to hand out, and nothing is asked. */
        if (audio != nullptr)
        {
            int freeTracks = 0;

            for (int track = 0; track < audio->trackCount(); ++track)
                if (! runs.isTrackBusy (track))
                    ++freeTracks;

            for (const auto* child : runs.childrenOf (job.run))
            {
                if (freeTracks <= 0)
                    break;

                if (child->isFinished() || ! child->sampler || child->track >= 0
                     || child->state != runState::armed)
                    continue;

                if (std::find (child->claims.begin(), child->claims.end(), child->strip)
                      == child->claims.end())
                    continue;

                engine.submit (origin::engine, "run.arm", one (child->id));
                --freeTracks;
            }
        }

        /*  EVERY MEMBER WITH NO RUN, ARMED ONTO ITS STRIP - at the GO, and
            again the tick after a member's run ends, however it ended. That
            is what makes a clip playable any number of times: the strip frees
            when the run ends, and the member takes it back armed and ready.
            A member past the last strip is left unarmed; the group is then
            partially armed, and the row says so. The strip each member is on
            is `placeMembers`' answer - its own pin, or the next one free. */
        bool anyStrip = false;

        for (const auto& member : placeMembers (document, group, samplerStrips()))
        {
            if (member.strip.empty() || lost (member.strip))
                continue;

            anyStrip = true;

            if (live.count (member.cue) == 0)
                engine.submit (origin::engine, "run.spawn",
                               { osc::Value::string (job.run), osc::Value::string (member.cue) });
        }

        /*  A GROUP WITH NO STRIP LEFT AND NOTHING SOUNDING COMPLETES (PRD
            §3.27): it has nothing left to offer, as an emptied round completes
            a loop (§3.6). The same for a group armed on a show with no sampler
            strips at all - it could never play anything. */
        if (! anyStrip && live.empty())
            finishPhase (engine, job, group);
    }

    void Runner::samplerEdges (Engine& engine)
    {
        /*  TOUCH-START AND FADER-STOP, AS RULES OVER A TOUCH AND A TRIM.

            Every surface, the virtual panel and the page ride the run under a
            fader strip through `node.set` on its trim, and hold it with
            `node.touch` - so the edges are read here, off the touch table and
            the trim, once, rather than by each thing that moves a fader.

            START (author, 2026-09-23): the member's fader has flown to its
            `initialLevel` and waits there; a NEW touch on it - the hand landing
            - starts the clip at wherever the fader is. One touch starts one
            clip, and a touch on a clip already playing is a ride and nothing
            else, so leaning on a fader to bring a sound down never restarts
            it. A hand that was on the fader when the strip changed hands holds
            the OLD node, never the new one - the virtual panel keeps its grab
            for the whole ride, and a surface lets go at the handover
            (SurfaceBridge) - so a resting hand cannot start the clip that has
            just arrived under it; only a hand landing again can.

            STOP: a hold clip whose trim is at the bottom with nobody touching
            it, where a hand was a tick ago or the fader came down from above -
            the release at -inf, §3.9a's fader-stop. A play-out clip is not
            stopped: silence on its fader is a mute.

            Submitted as `strip.press` and `strip.release` from the engine, so
            the log carries each edge and a replay - which runs no hooks - has
            them as records. */
        const auto& strips = samplerStrips();

        for (const auto& stripId : strips)
        {
            if (gateStrips.count (stripId) > 0)
                continue;

            const auto* holder = runs.holderOf (stripId);

            if (holder == nullptr || ! holder->sampler)
            {
                stripEdges.erase (stripId);
                continue;
            }

            auto& edge = stripEdges[stripId];

            if (edge.holder != holder->id)
            {
                edge = StripEdgeState {};
                edge.holder = holder->id;
                edge.lastTrim = holder->trim;
            }

            const auto touched = touches != nullptr
                                   && ! touches->holdersOf ("/godot/run/" + holder->id + "/trim").empty();

            edge.touchedTicks = touched ? edge.touchedTicks + 1 : 0;

            const auto launched = holder->launchRequested
                                    || holder->state == runState::playing
                                    || holder->state == runState::waiting;

            const auto hold = textOf (document.findById (holder->cue), "release") == "hold";

            if (! touched)
                edge.fired = false;

            if (! launched && holder->state == runState::armed && touched && ! edge.fired
                 && edge.touchedTicks > FaderEdge::touchDwellTicks)
            {
                engine.submit (origin::engine, "strip.press", one (stripId));
                edge.fired = true;
            }
            else if (launched && hold && holder->trim <= FaderEdge::parkedDb && ! touched
                      && (edge.wasTouched || edge.lastTrim > FaderEdge::parkedDb))
            {
                engine.submit (origin::engine, "strip.release", one (stripId));
            }

            /*  A TOUCH THAT FOUND THE CLIP ALREADY GOING started nothing, and
                is spent: the hand is riding, and letting go of a sound it
                pressed and touching it again is what starts the next one. */
            if (touched && launched)
                edge.fired = true;

            edge.wasTouched = touched;
            edge.lastTrim = holder->trim;
        }
    }

    void Runner::beginReleaseFade (const std::string& runId, double seconds, std::int64_t tick)
    {
        auto* target = runs.find (runId);

        if (target == nullptr || target->isFinished())
            return;

        /*  A FADE OVER A FADE takes over from where the level has got to, as
            every fade does; the release is simply the last one. */
        resolveTakeover (runId);

        FadeJob job;
        job.target = runId;
        job.reportsSelf = false;
        job.fromDb = target->ownLevel;
        job.toDb = silenceDb;
        job.ticksTotal = std::max (0, static_cast<int> (std::lround (seconds * 50.0)));
        job.curve = FadeCurve::linear;
        job.stopWhenDone = true;
        job.stopsAtTick = tick + job.ticksTotal;

        target->askStop (0);
        running.push_back (job);
    }

    void Runner::beginPanicFade (std::int64_t tick)
    {
        /*  THE SHOW'S NUMBER, read at the press: an edit to it lands at the next
            Esc and never under one already fading. A show that cannot say -
            no Audio element, a value that will not parse - gets the schema's
            second rather than a cut nobody chose. */
        const auto seconds = std::max (0.0, osc::parseDouble (document.getAttribute ("/godot/audio/panicFade")
                                                                .value_or ("1")).value_or (1.0));
        const auto ticks = static_cast<int> (std::lround (seconds * TickClock::rateHz));

        /*  NOUGHT IS THE CUT, and nothing is started for it: `enforceStops`
            stops every run the roots' stop reaches - once no stop cue's fade is
            holding it back. Left holding, a fade-and-stop whose own run the
            roots' stop reaches next hands its target back its level (the rule
            for a stop cue killed on its own), and the cue played on after Esc.
            Found by this change's tests, and older than it. */
        if (ticks <= 0)
        {
            dropStopFades();
            return;
        }

        const auto stopsAt = tick + ticks;

        /*  AND WHAT IS SHOWING (Phase 8a, namespace draft 35.5): a picture is
            taken down to black over the same second, from wherever its
            opacity is, then stopped - its footers run, as a sound's do. The
            job owns the run's ending until then, so the sweep in
            `advanceWaits` does not end it on the spot; the hook places the
            points. A layer still to come up comes up and goes down with it. */
        for (const auto& job : showing)
        {
            auto* run = runs.find (job.self);

            if (run == nullptr || run->isFinished() || run->stopIssued || job.removed)
                continue;

            fadeOutVideo (job.self, tick, ticks);
            run->askStop (0);
            run->stopEndsWait = true;
        }

        /*  WHAT IS SOUNDING, collected before anything is changed, since a fade
            begun here takes over the jobs the next one would be asked about. */
        std::vector<std::string> sounding;

        for (const auto& run : runs.all())
        {
            if (run.isFinished() || run.track < 0 || run.stopIssued)
                continue;

            if (run.state == runState::playing)
            {
                sounding.push_back (run.id);
                continue;
            }

            /*  ALREADY ON ITS WAY OUT, by a stop cue's fade or a clip's release.
                The stop that lands first wins: a ten-second fade-and-stop is
                brought down with everything else, and a stop due sooner than
                the panic's is left to land where it was going to. A `stopping`
                run no job holds is a hard stop `enforceStops` is about to
                issue, and is left to it. */
            if (run.state == runState::stopping)
            {
                /*  The soonest of them: a level's stop and a speed's can hold
                    one voice (§22.6), and the first to land is the one that
                    counts. */
                auto soonest = std::numeric_limits<std::int64_t>::max();

                for (const auto& held : running)
                    if (held.stopWhenDone && held.heldRun() == run.id)
                        soonest = std::min (soonest, held.stopsAtTick);

                if (soonest != std::numeric_limits<std::int64_t>::max() && soonest > stopsAt)
                    sounding.push_back (run.id);
            }

            /*  ARMED AND NOT SOUNDING - a standby made ready, a clip waiting for
                its launch, a cue in its pre-wait - has nothing to fade, and the
                roots' stop ends it at once as it always did. */
        }

        /*  A SOONER STOP ON A VOICE LANDS, LET GO OF BY THE STOP CUE THAT
            STARTED IT. The roots' stop is about to reach that stop cue's own
            run, and a fade whose run is stopped gives its target back its level
            - the rule for a stop cue killed on its own - so the cue went back to
            `playing` and played on after Esc. Found by this change's tests, and
            older than it. Held by nobody's run, the job lands its stop, and the
            stop cue's run, stopped like every other and owned by no job, ends
            with the rest. A stop on a group is left as it was: the group is a
            root the same press stops, and it came down without this. */
        for (auto& job : running)
        {
            if (! job.stopWhenDone || ! job.dca.empty()
                  || std::find (sounding.begin(), sounding.end(), job.heldRun()) != sounding.end())
                continue;

            const auto* held = runs.find (job.heldRun());

            if (held == nullptr || held->track < 0)
                continue;

            job.self.clear();
            job.reportsSelf = false;
        }

        for (const auto& id : sounding)
        {
            panicShapedFade (id, tick, ticks, seconds);

            /*  AN ABORT, and so no post-wait after it (2026-10-02, K2, namespace
                draft §23.13) - a footer's sound Esc finds playing included. */
            if (auto* target = runs.find (id))
            {
                target->askStop (0);
                target->stopEndsWait = true;
            }
        }
    }

    void Runner::panicShapedFade (const std::string& runId, std::int64_t tick, int ticks, double seconds)
    {
        auto* target = runs.find (runId);

        if (target == nullptr || ticks <= 0)
            return;

        const auto stopsAt = tick + ticks;

        /*  WHICH VOICES IS THE CALLER'S QUESTION, and not this one's (namespace
            draft §24, the review of 2026-10-01). Esc's caller has already left
            out what a sooner stop holds, and takes over everything else - a
            voice back to `playing` under a fade-and-stop that a seek left on
            it included, as Esc always did. Doh!'s leaves out what a sooner
            stop holds itself. A filter here, as the first build had, let such
            a sooner stop stand under Esc still held by the stop cue's own run,
            which Esc then stops - and a fade whose run is stopped hands its
            target back to `playing`: the cue played on after Esc. */

        /*  A FADE ALREADY ON IT GIVES WAY, from wherever its level has got
            to - and the stop it may have carried with it lands later than
            this one, or it would not be here. So does a fade on its SPEED,
            which Esc does not move: the speed stays where it has got to
            (EB), and a speed fade's stop, due later, goes with it. */
        resolveTakeover (runId);
        resolveTakeover ("rate:" + runId);

        /*  AND WHAT A FADE WAS MOVING OF ITS OWN (namespace draft §26, PD):
            its sends, EQ and plugin values stay where they have got to, as its
            speed does - Esc's fade is the level's. */
        releaseMovesOf (runId);

        target = runs.find (runId);

        if (target == nullptr)
            return;

        FadeJob job;
        job.target = runId;
        job.reportsSelf = false;
        job.fromDb = target->ownLevel;
        job.toDb = silenceDb;
        job.ticksTotal = ticks;
        job.curve = FadeCurve::linear;
        job.stopWhenDone = true;
        job.stopsAtTick = stopsAt;

        /*  A MIC CUE IS FADED AT ITS INPUT, as a stop cue's fade fades one
            (namespace draft §18.5): the voice goes and the channel's reverb
            rings on after it, which is what Esc has always let a mic cue
            do. The output stays where it is. */
        if (target->kind == "mic")
        {
            job.toDb = job.fromDb;

            if (audio != nullptr && target->track >= 0)
                audio->shutLive (target->track, seconds);
        }

        running.push_back (job);
    }

    void Runner::dropStopFades()
    {
        /*  ONLY THE JOBS THAT HOLD A STOP. A plain fade's own run is marked by
            the double Esc like every other root and `advanceFades` retires it;
            it holds no voice, so nothing waits on it. A job that holds a stop
            is the one thing `enforceStops` defers to, and deferring is exactly
            what a double Esc does not do. */
        running.erase (std::remove_if (running.begin(), running.end(),
                                       [] (const FadeJob& job)
                                       {
                                           return job.stopWhenDone && job.dca.empty();
                                       }),
                       running.end());
    }

    void Runner::resetEffects()
    {
        if (audio == nullptr)
            return;

        /*  WHAT THE PRESS LEAVES ARMED KEEPS ITS LEVEL (namespace draft §23.3
            and §23.6). The standby's arm and every member its horizon armed -
            a run only made ready, which every road to a launch unmarks - are
            spared by `run.killAll`, and the GO after it launches them with no
            arm in between: the level their arm set is the only one they will
            have, and a sweep that silenced it made that GO play nothing. One
            in a block somebody reached into is in this list all the same, and
            the press stops the block: what was asked for in it is ended, then
            the rest is given back by the block's revocation - an armed run
            never launched, which holds nothing for a sweep to empty, and whose
            voice the next arm on it sets afresh. */
        std::vector<int> ready;

        for (const auto& run : runs.all())
            if (! run.isFinished() && run.track >= 0 && run.onlyPrepared())
                ready.push_back (run.track);

        audio->resetEffects (ready);
    }

    void Runner::dropOutputs (std::int64_t tick)
    {
        /*  WHAT THE PRESS'S OWN DRAIN QUEUED NEVER LEFT (namespace draft §24,
            HQ, L31): the sender flushes once a tick, after the drain, so an osc
            run launched earlier in this drain - and killed by this press - had
            its message in the queue emptied below. It is marked so that
            Doh! does not count it as sent. Decided from handler state - the
            kind, the launch tick, the kill marks the run table's half has just
            written - and never from whether there is a sender, so a replay,
            which has none, marks the same runs. A message a rate cap held from
            an earlier tick, a pre-send this tick's hook wrote, and a MIDI
            message - which goes to its own thread at once and may have left -
            are dropped where no handler can see them, and are not marked
            (L31). */
        for (const auto& snapshot : runs.all())
        {
            if (snapshot.kind != "osc" || snapshot.isFinished() || snapshot.launchRequestedAtTick <= 0
                  || snapshot.launchRequestedAtTick != tick || ! beingKilled (snapshot))
                continue;

            if (auto* dropped = runs.find (snapshot.id))
                dropped->sendDropped = true;
        }

        /*  EVERY PICTURE, AT ONCE (Phase 8a, namespace draft 35.5): the canvases
            black and the outputs left open on black - what Go.dot originates
            is the picture, not the projector (PRD §4.4). Every job lets go of
            its run, so the sweep ends the killed runs as it ends the rest; a
            fade-out Esc began is cut with them. A replay has no sink and
            drops the same jobs. */
        if (videoSink != nullptr && ! showing.empty())
            videoSink->clear();

        for (auto& job : showing)
        {
            job.removed = true;
            job.fadeOutTicks = -1;
            job.endsAtTick = -1;
        }

        /*  THE NETWORK SENDER'S QUEUE, a value a rate cap holds back included,
            but for the pre-sends of what the press leaves ready: the standby's
            scene keeps its readiness through a double Esc (§23.3), and the GO
            after it counts those pre-sends as done - dropped, the desk would
            never get a value the scene believes it holds. */
        if (sender_ != nullptr)
            sender_->dropQueued ([this] (const std::string& owner) { return sparedByThePress (owner); });

        /*  THE MIDI QUEUE: the cues' messages dropped, the surfaces' kept, and
            a note-off for each note a cue started that nothing has ended. */
        if (midiOut != nullptr)
            midiOut->dropQueued();

        /*  A START CUE'S FIRE IS SUBMITTED BY THE NEXT TICK'S HOOK, so one fired
            earlier in this drain would fire its target a tick after the press.
            A hook's list: a replay never had the record, and does not now. And
            one the hook of this very tick submitted, draining behind the press,
            is answered by `cue.fire` itself (`killedInDrain`). A jump cue's
            move is an action like any other, and goes with them (§27). */
        startsToFire.clear();
        jumpsToMake.clear();
        killedAtTick = tick;

        /*  THE PERSISTENT PASS A GO BEFORE THE PRESS OPENED IS TAKEN BACK (the
            review of H4, 2026-10-02): PRD §3.29 has the NEXT GO restore the
            section after a double Esc, and a pass owed to an earlier step - not
            yet run, or waiting for the desks' answers - put the beds back right
            after the press. The step is counted as asserted, so only a step
            taken after the press opens a pass. Hook state, as the start cues'
            list: a replay takes `run.assert` from the log, and the log now has
            none.

            AND A BED AN ESC BEFORE IT PAUSED IS FORGOTTEN (K8, the author's
            ruling): a double Esc on a persistent cue is what it was - stopped at
            once, back from the top at the next GO - and the second press drops
            the first's pause with everything else. Handler state, like the
            pause itself; what a pass owed goes with it. */
        assertedFor = lists.stepsTaken();
        assertDue = -1;
        paused.clear();
        owed.clear();

        /*  AND THE PUT-BACKS A CLOCK MOVE OR A SETTINGS OPERATION STILL OWES
            (2026-10-02, K5's review, namespace draft §23.16, LP): PRD §4.4's
            "drops all actions", as the queued output above is dropped - the
            desk keeps what the pre-sends left, the price of an emergency. The
            standby is still made ready again once a write is let in (LC):
            `waitingForWrites` stays, and the settle is let go. Hook state. */
        owedPutBacks.clear();
        prepareAfterPutBack = -1;
    }

    bool Runner::goTooSoon (std::int64_t tick) const
    {
        if (lastGoTick < 0)
            return false;

        /*  The schema's half second when the show cannot say, as the panic
            fade falls back to its own default rather than to nothing. */
        const auto seconds = osc::parseDouble (document.getAttribute ("/godot/list/goDebounce")
                                                 .value_or ("0.5")).value_or (0.5);
        const auto window = static_cast<std::int64_t> (std::llround (std::max (0.0, seconds)
                                                                      * TickClock::rateHz));

        return window > 0 && tick >= lastGoTick && tick - lastGoTick < window;
    }

    //==============================================================================
    /*  DOH! - TAKING BACK THE LAST GO (PRD §3.32, namespace draft §24; D1,
        2026-10-01).

        THE REPLAY RULE, which every function below keeps: a decision that
        changes a run, the run table, an identifier, the document or the history
        is taken in a HANDLER, from handler state and logged records - the
        document, run fields only handlers and records write (`goSerial`,
        `causedBy`, `preparedAfterGo`, `takenBack`, `startedAtTick`, `stopAsked`,
        launch evidence, `endedAtTick`, `sentTo`...), and `state` only for
        finished, waiting, post-wait and preparing, which no hook writes. Never
        `track`, `stopIssued`, a job's progress, a desk's value or the clock: a
        replay has none of those, and must reach the same answer. */
    bool Runner::hasLaunchEvidence (const Run& run) noexcept
    {
        return run.launchRequestedAtTick > 0 || run.isWaiting() || run.startedAtTick >= 0;
    }

    std::uint64_t Runner::goOfRun (const std::string& runId) const
    {
        const auto* run = runs.find (runId);

        if (run == nullptr)
            return 0;

        /*  WHAT THE GO CAUSED BEFORE WHAT MADE IT: a footer an older act ran
            because of the GO carries the act's own serial - the GO that entered
            the act, long ago - and `causedBy` says which GO it is really
            answering to. */
        return run->causedBy != 0 ? run->causedBy : run->goSerial;
    }

    const Run* Runner::liveUntakenRunOf (const std::string& cueId) const
    {
        const Run* newest = nullptr;

        for (const auto& run : runs.all())
            if (run.cue == cueId && ! run.isFinished() && run.state != runState::preparing
                  && ! run.takenBack)
                newest = &run;

        return newest;
    }

    const Run* Runner::liveOtherRunOf (const std::string& cueId, const std::string& except) const
    {
        const Run* newest = nullptr;

        for (const auto& run : runs.all())
            if (run.cue == cueId && run.id != except && ! run.isFinished() && run.state != runState::preparing
                  && ! run.takenBack)
                newest = &run;

        return newest;
    }

    void Runner::claimSendsLeft (Run& run)
    {
        const auto serial = run.causedBy != 0 ? run.causedBy : run.goSerial;

        if (serial == 0)
            return;

        const auto found = leftByGo.find (serial);

        if (found == leftByGo.end() || found->second.cues.erase (run.cue) == 0)
            return;

        /*  ONCE PER CUE: the first run this GO makes of it sends nothing, and
            the entry for it goes, so a scene that loops sends it in its later
            rounds (namespace draft §24, L32). */
        run.sendsLeft = true;

        /*  AND THE LIST'S MARK LETS GO OF IT NOW (D3's review, OA): a GO that
            only made the scene that would spawn the cue left the mark's entry
            standing (`endGo`), so an Esc before the spawn loses nothing; the
            run that claims it is the end of it. */
        if (const auto mark = marks.find (found->second.list); mark != marks.end())
        {
            auto& left = mark->second.left;

            for (auto entry = left.begin(); entry != left.end();)
            {
                entry->second.erase (std::remove (entry->second.begin(), entry->second.end(), run.cue),
                                     entry->second.end());

                if (entry->second.empty())
                    entry = left.erase (entry);
                else
                    ++entry;
            }

            if (left.empty() && ! mark->second.root.has_value())
                marks.erase (mark);
        }

        if (found->second.cues.empty())
            leftByGo.erase (found);
    }

    bool Runner::isFooterCueOf (const Run& group, const std::string& cueId) const
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return false;

        const auto holder = cue.getParent();

        return holder.isValid() && holder.getType().toString() == "Footer"
                 && holder.getParent()[idProperty].toString().toStdString() == group.cue;
    }

    bool Runner::isReached (const Run& group) const
    {
        /*  AN OLDER ACT'S REACTION TO THE GO IS THE GO'S (namespace draft §24,
            HE): the release a manual group runs because the GO fired its last
            member. Followed through manual groups that play once, and that no
            other hand is stopping; anything else is said, not undone. */
        if (goRecord.serial == 0 || ! group.isGroup() || group.iterations != 1)
            return false;

        if (group.stopAsked && group.stopAskedBy != goRecord.serial)
            return false;

        if (const auto* above = runs.find (group.parent);
            above != nullptr && ! (above->isGroup() && isManualGroup (document.findById (above->cue))))
            return false;

        /*  REACHED: one the GO's root sits in, or a manual group whose reached
            child has ended since the GO - the act above an act that ended. */
        std::function<bool (const Run&, int)> reached = [this, &reached] (const Run& candidate, int depth)
        {
            if (depth > 64)
                return false;

            if (std::find (goRecord.touched.begin(), goRecord.touched.end(), candidate.id) != goRecord.touched.end())
                return true;

            if (! candidate.isGroup() || ! isManualGroup (document.findById (candidate.cue)))
                return false;

            for (const auto* child : runs.childrenOf (candidate.id))
                if (child->isGroup() && child->isFinished() && child->endedAtTick >= goRecord.tick
                      && reached (*child, depth + 1))
                    return true;

            return false;
        };

        return reached (group, 0);
    }

    void Runner::createRun (const std::string& id, const std::string& cueId,
                            const std::string& kind, const std::string& parentRun)
    {
        runs.create (id, cueId, kind, parentRun);

        auto* run = runs.find (id);

        if (run == nullptr)
            return;

        /*  Found after the create, which can move the table. */
        const auto* parent = parentRun.empty() ? nullptr : runs.find (parentRun);

        /*  THE HORIZON'S WORK IS NOBODY'S GO (§24, GY), whatever its parent
            carries, and it says after which GO it was made. */
        if (makingForHorizon)
        {
            run->goSerial = 0;
            run->preparedAfterGo = static_cast<std::int64_t> (goCount);
            return;
        }

        /*  AND NOR IS A DOH'S PUT-BACK (2026-10-03, D3): a cue the GO stopped,
            made again; a bank it closed, fired again. Nobody's GO, whatever its
            parent carries; what is made under it later inherits that. */
        if (makingForPutBack)
        {
            run->goSerial = 0;
            return;
        }

        /*  THE GO IN HAND, or the parent's: a member a group spawns minutes
            later is still the GO that entered the group. */
        run->goSerial = currentGo != 0 ? currentGo : (parent != nullptr ? parent->goSerial : 0);

        /*  A RELEASE THE GO SET OFF, while it is still the GO a Doh would take
            back: a footer an older act runs because the GO fired its last
            member. A run made under one inherits it. */
        if (parent != nullptr && parent->causedBy != 0)
            run->causedBy = parent->causedBy;
        else if (goRecord.serial != 0 && parent != nullptr && parent->isGroup()
                   && isFooterCueOf (*parent, cueId) && isReached (*parent))
            run->causedBy = goRecord.serial;

        claimSendsLeft (*run);
    }

    void Runner::stampSubtree (const std::string& runId, std::uint64_t serial)
    {
        if (serial == 0)
            return;

        std::vector<std::string> subtree { runId };

        for (const auto* below : runs.descendantsOf (runId))
            subtree.push_back (below->id);

        for (const auto& id : subtree)
            if (auto* run = runs.find (id))
            {
                run->goSerial = serial;

                /*  What it has not launched yet it will launch for this GO, so
                    a cue a Doh left with a device's operator sends nothing. */
                if (! hasLaunchEvidence (*run))
                    claimSendsLeft (*run);
            }
    }

    bool Runner::heardUnder (const std::string& rootId, std::uint64_t serial) const
    {
        /*  §24's ONE PREDICATE, read from logged records and the document: a
            media or mic run with `run.started`, or a MIDI run launched to a port
            that plays sound, its tx on (the author, 2026-10-01). The Doh!
            setting has no say in it. Only the GO's own runs are asked - the
            horizon's work under the root is not the GO's. */
        const auto heard = [this] (const Run& run)
        {
            if ((run.kind == "media" || run.kind == "mic") && run.startedAtTick >= 0)
                return true;

            /*  SEEN COUNTS AS HEARD (Phase 8a, namespace draft 35, VK): a
                picture up on a canvas has reached the room as surely as a
                sound has. */
            if (run.kind == "video" && run.startedAtTick >= 0)
                return true;

            return run.kind == "midi" && hasLaunchEvidence (run) && playsSound (document, run.cue);
        };

        const auto ofTheGo = [serial] (const Run& run)
        {
            return (run.causedBy != 0 ? run.causedBy : run.goSerial) == serial;
        };

        if (const auto* root = runs.find (rootId); root != nullptr && heard (*root))
            return true;

        for (const auto* below : runs.descendantsOf (rootId))
            if (ofTheGo (*below) && heard (*below))
                return true;

        return false;
    }

    void Runner::endHere (const std::string& runId, std::int64_t tick)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished())
            return;

        /*  ENDED THE WAY A JUMP ENDS WHAT IT ABANDONS: done, on the model, with
            no footer and no post-wait, its slots let go in this drain - and
            everything that would otherwise report on it later let go too, so no
            late `run.ended` lands over the tick it ended on. A launch placed for
            a sample still to come is cancelled by the stop. */
        run->state = runState::done;
        run->endedAtTick = tick;
        run->prepare.clear();

        const auto track = run->track;
        runs.releaseSlotsOf (runId);

        if (audio != nullptr && track >= 0)
            audio->stop (track);

        for (auto& job : scheduled)
            if (job.run == runId)
                job.retired = true;

        finishing.erase (std::remove (finishing.begin(), finishing.end(), runId), finishing.end());
        supersededRuns.erase (std::remove (supersededRuns.begin(), supersededRuns.end(), runId),
                              supersededRuns.end());
        running.erase (std::remove_if (running.begin(), running.end(),
                                       [&runId] (const FadeJob& job) { return job.self == runId; }),
                       running.end());

        for (auto& job : sending)
            if (job.self == runId)
                job.finished = true;
    }

    bool Runner::countsAsSent (const Run& run) const
    {
        /*  WHAT LEFT IS DECIDED AT THE SEND (§24, HQ): a device the run was
            routed to, a launch, and nothing that says nothing was written - a
            double Esc's drop of its own drain among them (`sendDropped`, since
            H4, §23.10). */
        if (run.sentTo.empty() || ! hasLaunchEvidence (run) || run.sendDropped)
            return false;

        if (run.state == runState::failed)
            for (const auto* nothingWritten : { reason::badAddress, reason::readOnly, reason::typeMismatch,
                                                runError::noPort, runError::badMessage, runError::sendFailed })
                if (run.error == nothingWritten)
                    return false;

        /*  A PRE-SEND ASKS BEFORE IT WRITES, so its launch says only that it
            was asked for: it counts once its write is known - done, or the desk
            answering with another value. */
        if (run.preparedAfterGo >= 0)
            return run.state == runState::done
                     || (run.state == runState::failed && run.error == oscError::disagreed);

        return true;
    }

    bool Runner::dohTooSoon (std::int64_t tick) const
    {
        if (lastDohTick < 0)
            return false;

        /*  THE GO DEBOUNCE'S OWN NUMBER: a bounced press, a held key, a second
            hand would otherwise be taken for the deliberate second press. */
        const auto seconds = osc::parseDouble (document.getAttribute ("/godot/list/goDebounce")
                                                 .value_or ("0.5")).value_or (0.5);
        const auto window = static_cast<std::int64_t> (std::llround (std::max (0.0, seconds)
                                                                      * TickClock::rateHz));

        return window > 0 && tick >= lastDohTick && tick - lastDohTick < window;
    }

    bool Runner::dohTooLate (std::int64_t tick) const
    {
        /*  THE SHOW'S WINDOW, ten seconds when it cannot say; nought is off,
            and then every press is too late. The GO debounce's convention: at
            exactly the window's length the press is outside it. */
        const auto seconds = osc::parseDouble (document.getAttribute ("/godot/list/dohWindow")
                                                 .value_or ("10")).value_or (10.0);
        const auto window = static_cast<std::int64_t> (std::llround (std::max (0.0, seconds)
                                                                      * TickClock::rateHz));

        return window <= 0 || tick - goRecord.tick >= window;
    }

    bool Runner::isInside (const std::string& cueId, const std::string& ancestorId) const
    {
        if (ancestorId.empty())
            return false;

        for (auto node = document.findById (cueId).getParent(); node.isValid(); node = node.getParent())
        {
            if (node.getType().toString() == "List")
                return false;

            if (node[idProperty].toString().toStdString() == ancestorId)
                return true;
        }

        return false;
    }

    bool Runner::reaches (const std::string& standby, const std::string& marked) const
    {
        if (standby.empty() || marked.empty())
            return false;

        if (standby == marked || isInside (standby, marked))
            return true;

        /*  OR A GROUP THE MACHINE RUNS THAT HOLDS IT: an automatic sequence or a
            timeline will spawn the marked cue under this GO, through nothing
            but groups of its own kind. A manual group waits for a GO of its
            own, so it does not reach through. */
        const auto automatic = [this] (const juce::ValueTree& node)
        {
            return node.isValid() && node.getType().toString() == "Group" && ! isManualGroup (node)
                     && textOf (node, "mode") != "sampler";
        };

        if (! automatic (document.findById (standby)))
            return false;

        for (auto node = document.findById (marked).getParent(); node.isValid(); node = node.getParent())
        {
            const auto element = node.getType().toString();

            if (element == "List")
                return false;

            if (node[idProperty].toString().toStdString() == standby)
                return true;

            if (element == "Header" || element == "Footer")
                continue;

            if (! automatic (node))
                return false;
        }

        return false;
    }

    bool Runner::isPast (const std::string& listId, const std::string& fired, const std::string& marked) const
    {
        if (fired.empty() || fired == marked || isInside (fired, marked))
            return false;

        /*  THE LIST'S ROW ORDER: its cues walked depth first, as the persistent
            solver places them - a group before what it holds. */
        std::vector<std::string> order;

        std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& node)
        {
            for (const auto& child : node)
            {
                const auto element = child.getType().toString().toStdString();

                if (element == "Persistent")
                    continue;

                if (element == "Header" || element == "Footer")
                {
                    walk (child);
                    continue;
                }

                if (doc::ShowDocument::ownerForElement (element) != "cue")
                    continue;

                order.push_back (child[idProperty].toString().toStdString());
                walk (child);
            }
        };

        walk (document.findById (listId));

        const auto at = [&order] (const std::string& id) -> std::ptrdiff_t
        {
            const auto found = std::find (order.begin(), order.end(), id);
            return found == order.end() ? -1 : found - order.begin();
        };

        const auto firedAt = at (fired);
        const auto markedAt = at (marked);

        return firedAt >= 0 && markedAt >= 0 && firedAt > markedAt;
    }

    void Runner::beginGo (std::int64_t tick, const std::string& listId, const std::string& standby,
                          bool finishedBefore)
    {
        GoRecord record;
        record.serial = ++goCount;
        record.tick = tick;
        record.list = listId;
        record.cue = standby;
        record.finishedBefore = finishedBefore;

        /*  THE DEBOUNCE, noted here as `noteGo` noted it before this record,
            with where it stood before: the corrected GO after a Doh is measured
            from the GO before this one. */
        record.lastGoTickBefore = lastGoTick;
        lastGoTick = tick;

        if (const auto found = marks.find (listId); found != marks.end())
            record.markBefore = found->second;

        currentGo = record.serial;

        /*  A GO IS NOT A BOUNCE OF THE DOH BEFORE IT: a Doh after a GO takes
            back that GO. */
        lastDohTick = -1;

        /*  WHAT A DOH LEFT, FILED UNDER THIS GO for every marked cue it reaches
            (§24, HP): the runs it makes of those cues send nothing. */
        if (record.markBefore.has_value())
            for (const auto& [marked, cues] : record.markBefore->left)
                if (reaches (standby, marked))
                {
                    auto& filed = leftByGo[record.serial];
                    filed.list = listId;
                    filed.cues.insert (cues.begin(), cues.end());
                    record.filed[marked] = cues;
                }

        goRecord = std::move (record);
        goChanges = {};
        lists.setDohOffer ({ listId, standby, tick });

        /*  AND THE LAST DOH'S REPORT IS RETIRED (D4's review, OM): the show has
            moved on, and a sentence about a GO two GOs ago only misleads.
            Handler state, as the readout is - a replay retires it at the same
            GO. */
        lists.setDohReport ({});

        /*  AND A SCENE A DOH PUT OFF UNTIL IT HAD ENDED is not brought back
            over this GO (D3, namespace draft §24.13): most often the corrected
            GO, whose own stop stops that scene again - a relaunch landing after
            it would undo the very GO the operator meant. */
        pendingRelaunches.erase (std::remove_if (pendingRelaunches.begin(), pendingRelaunches.end(),
                                                 [&listId] (const PendingRelaunch& pending) { return pending.list == listId; }),
                                 pendingRelaunches.end());

        /*  BOUNDED, belt and braces: the oldest entries go first. */
        while (leftByGo.size() > 16)
            leftByGo.erase (leftByGo.begin());
    }

    void Runner::endGo (Engine& engine, std::int64_t tick, const std::vector<std::string>& made,
                        const std::string& carriedOn)
    {
        const auto serial = goRecord.serial;
        goRecord.made = made;

        /*  THE OLDER ACTS IT REACHED (§24, HE): the unfinished manual group a
            root of this GO sits in, when that group is not this GO's own. */
        for (const auto& run : runs.all())
        {
            if (run.goSerial != serial)
                continue;

            const auto* parent = runs.find (run.parent);

            if (parent == nullptr || parent->goSerial == serial || parent->isFinished() || ! parent->isGroup()
                  || ! isManualGroup (document.findById (parent->cue)))
                continue;

            if (std::find (goRecord.touched.begin(), goRecord.touched.end(), parent->id) == goRecord.touched.end())
                goRecord.touched.push_back (parent->id);
        }

        /*  THE RESUME GOES WITH ANY GO ON ITS LIST (2026-10-02, D2, GN, namespace draft §24.12):
            spent on the run that carried it on, or dropped with its arm at the
            point revoked - the operator went another way, and a later GO on the
            cue starts it from its top. What was left with devices' operators
            lives by its own rule, below. */
        dropRoot (engine, tick, goRecord.list, carriedOn);

        /*  AND THE LIST'S MARK, SETTLED (§24's table): an entry this GO filed is
            consumed - unless the GO stood on the marked cue and made no run of
            it (decision N's ignore), when the filing is undone and the entry
            stays; an entry it did not file stays for a GO before its cue, and
            goes for a GO past it. */
        if (const auto found = marks.find (goRecord.list); found != marks.end())
        {
            auto& left = found->second.left;

            for (auto entry = left.begin(); entry != left.end();)
            {
                const auto& marked = entry->first;

                if (const auto filed = goRecord.filed.find (marked); filed != goRecord.filed.end())
                {
                    const auto stoodOn = goRecord.cue == marked || isInside (goRecord.cue, marked);

                    /*  A RUN OF THE CUE, OR OF A SCENE THAT WILL SPAWN IT
                        (2026-10-03, D3): a GO that enters an act afresh makes the
                        act, and the act's job spawns the member after its header,
                        a tick or more later - which is no ignore of decision N's.
                        Undone there, the filing let the act's footer, left to a
                        device's operator, go out again (D3's test 23). */
                    const auto madeIt = std::any_of (runs.all().begin(), runs.all().end(),
                                                     [&marked, serial] (const Run& run)
                                                     {
                                                         return run.cue == marked
                                                                  && (run.goSerial == serial || run.causedBy == serial);
                                                     });

                    const auto madeAbove = ! madeIt
                                             && std::any_of (runs.all().begin(), runs.all().end(),
                                                             [this, &marked, serial] (const Run& run)
                                                             {
                                                                 return isInside (marked, run.cue)
                                                                          && (run.goSerial == serial || run.causedBy == serial);
                                                             });

                    const auto madeOne = madeIt || madeAbove;

                    /*  ONLY THE SCENE THAT WILL SPAWN IT, so far (D3's review,
                        OA): the entry stays on the mark until the run of the cue
                        claims it (`claimSendsLeft`) - an Esc before the spawn
                        would otherwise have consumed it with nothing sent
                        nothing, and the next GO on the cue sent it again. */
                    if (madeAbove)
                    {
                        ++entry;
                        continue;
                    }

                    if (stoodOn && ! madeOne)
                    {
                        if (const auto held = leftByGo.find (serial); held != leftByGo.end())
                        {
                            for (const auto& cue : filed->second)
                                held->second.cues.erase (cue);

                            if (held->second.cues.empty())
                                leftByGo.erase (held);
                        }

                        ++entry;
                        continue;
                    }

                    entry = left.erase (entry);
                    continue;
                }

                if (isPast (goRecord.list, goRecord.cue, marked))
                    entry = left.erase (entry);
                else
                    ++entry;
            }

            if (left.empty())
                marks.erase (found);
        }

        currentGo = 0;
    }

    void Runner::noteFireOnList (const std::string& cueId, const std::string& from, std::uint64_t cause)
    {
        if (goRecord.serial == 0)
            return;

        /*  THE GO'S OWN START CUE is the GO, not a trigger after it. */
        if (from == origin::engine && cause == goRecord.serial)
            return;

        if (listOfCue (cueId) != goRecord.list)
            return;

        goRecord.firedAfter = true;
        lists.setDohOffer ({});
    }

    void Runner::noteEscape() noexcept
    {
        if (goRecord.serial != 0)
            goRecord.escapedAfter = true;

        /*  AND NO SCENE A DOH PUT OFF comes back after an Esc (D3, GV): Esc
            stopped everything, and a Doh never undoes an Esc. */
        pendingRelaunches.clear();
    }

    void Runner::markFire (Engine& engine, std::int64_t tick, const std::string& cueId,
                           const std::string& from, std::uint64_t cause)
    {
        const auto listId = listOfCue (cueId);

        /*  A SCENE A DOH PUT OFF UNTIL IT HAD ENDED, fired by name, by a trigger
            or by a start cue meanwhile (D3's review, OB): the hand has played
            it, and the put-back would seat a second copy. */
        pendingRelaunches.erase (std::remove_if (pendingRelaunches.begin(), pendingRelaunches.end(),
                                                 [this, &cueId] (const PendingRelaunch& pending)
                                                 {
                                                     const auto* scene = runs.find (pending.groupRun);
                                                     return scene != nullptr && scene->cue == cueId;
                                                 }),
                                 pendingRelaunches.end());

        /*  A FIRE OF THE CUE A DOH WOULD CARRY ON (2026-10-02, D2, GN, namespace draft §24.12) - by
            name, by a trigger, by a start cue - starts it from its top: the
            resume is dropped first, its arm at the point revoked, so the fire
            finds no arm waiting silent at the point and makes the cue afresh. */
        /*  AND A FIRE OF A CUE INSIDE A PAUSED SCENE (D2's review, NA): the
            hand has played part of the scene, and a re-seat at the next GO
            would sound that cue a second time beside it. The scene starts from
            its top instead, as after any fire of it by name. */
        if (const auto held = marks.find (listId);
            held != marks.end() && held->second.root.has_value()
              && (held->second.cue == cueId || held->second.root->cue == cueId
                    || (held->second.root->kind == DohRoot::Kind::tree && isInside (cueId, held->second.root->cue))))
            dropRoot (engine, tick, listId);

        const auto found = marks.find (listId);

        if (found == marks.end())
            return;

        auto& left = found->second.left;
        const auto entry = left.find (cueId);

        if (entry == left.end())
            return;

        /*  A START CUE OF A GO FIRING THE MARKED CUE is that GO reaching it:
            filed under the GO, so the target sends nothing. By name or by a
            trigger it is a deliberate send, and sends everything (§24, L36). */
        if (from == origin::engine && cause != 0)
        {
            auto& filed = leftByGo[cause];
            filed.list = listId;
            filed.cues.insert (entry->second.begin(), entry->second.end());
        }

        left.erase (entry);

        if (left.empty())
            marks.erase (found);

        while (leftByGo.size() > 16)
            leftByGo.erase (leftByGo.begin());
    }

    void Runner::forgetGoOnJump (Engine& engine, std::int64_t tick, const std::string& listId)
    {
        if (goRecord.serial != 0 && goRecord.list == listId)
        {
            goRecord = {};
            goChanges = {};
            lists.setDohOffer ({});
        }

        /*  AND A SCENE PUT OFF UNTIL IT HAD ENDED (D3): the jump has placed
            the world, that scene with it. */
        pendingRelaunches.erase (std::remove_if (pendingRelaunches.begin(), pendingRelaunches.end(),
                                                 [&listId] (const PendingRelaunch& pending) { return pending.list == listId; }),
                                 pendingRelaunches.end());

        /*  THE WHOLE MARK, its resume revoked (D2): the jump re-solves the
            world, and nothing paused carries on into it. */
        dropMark (engine, tick, listId);

        for (auto entry = leftByGo.begin(); entry != leftByGo.end();)
        {
            if (entry->second.list == listId)
                entry = leftByGo.erase (entry);
            else
                ++entry;
        }
    }

    //==============================================================================
    /*  CARRYING ON WHAT A DOH PAUSED (2026-10-02, D2, PRD §3.32, namespace draft
        §24). Every function below keeps the replay rule (§24.1): what changes a
        run, the run table, an identifier or the history is decided from handler
        state and logged records - the playhead included, which reaches a
        handler only through the `go.dohPlayhead` record the hook submits. */
    const Runner::DohMark* Runner::markFor (const std::string& listId, const std::string& standby) const
    {
        const auto found = marks.find (listId);

        if (found == marks.end() || ! found->second.root.has_value() || found->second.cue != standby)
            return nullptr;

        return &found->second;
    }

    const Runner::DohRoot* Runner::soundRootOf (const std::string& cueId) const
    {
        for (const auto& entry : marks)
        {
            const auto& mark = entry.second;

            if (mark.root.has_value() && mark.root->kind != DohRoot::Kind::tree && mark.root->cue == cueId)
                return &*mark.root;
        }

        return nullptr;
    }

    bool Runner::isResumeArm (const Run& run, const DohRoot& root) const
    {
        /*  WHAT A RESUME ARM IS, wherever a rule names one (namespace draft §24.12): made at the
            point, and nobody has asked for it yet - launch evidence, never the
            `prepare` mark, which a group's job clears from a hook. */
        return ! run.isFinished() && run.resumes && ! hasLaunchEvidence (run)
                 && run.cue == root.cue && run.parent == root.underRun;
    }

    void Runner::armAtRoot (Run& run, const DohRoot& root, const juce::ValueTree& cue) const
    {
        /*  NO PRE-WAIT: the cue is carried on, not started; its wait was spent
            before the Doh paused it. */
        run.preWaitTicks = 0;
        run.resumes = true;

        if (root.kind != DohRoot::Kind::media)
            return;

        /*  ARMED SILENT, AND ARRIVING OVER THE DE-CLICK (K8's `deClick`, the
            author's tenth of a second) at the level it had at the press. */
        run.deClick = true;
        run.arrivalDb = root.levelDb;

        const auto ranges = rangesOf (cue);
        const auto slices = static_cast<int> (ranges.size());

        /*  INSIDE THE SLICE IT WAS IN, at the same point and in the same pass
            (K8's LR, the road a paused bed takes): a loop count lowered since
            keeps the point and gives it the slice's last pass. */
        if (root.range >= 0 && slices > 0)
        {
            run.startRange = std::min (root.range, slices - 1);
            run.startOffset = 0.0;

            if (root.range < slices && root.sliceFrom > 0.0)
            {
                const auto& slice = ranges[static_cast<std::size_t> (run.startRange)];
                const auto length = slice.out - slice.in;
                auto from = root.sliceFrom;

                if (length > 0.0 && slice.loops > 0 && from >= static_cast<double> (slice.loops) * length)
                    from = static_cast<double> (slice.loops - 1) * length + std::fmod (from, length);

                run.sliceFrom = length > 0.0 ? from : 0.0;
            }
        }
        else if (slices == 0)
        {
            /*  A second of the file, and never nought, which on a run means
                "where the cue says". */
            run.startOffset = std::max (root.offset, 0.001);
        }

        run.armedOrigin = run.startOffset > 0.0 ? run.startOffset : numberOf (cue, "startOffset");
    }

    bool Runner::notOver (const juce::ValueTree& cue, int range, double at) const
    {
        /*  A SLICE ALWAYS HAS SOMEWHERE TO CARRY ON: its loop, or the slice
            after it (L1, K8's LR). A FILE, by the length the log knows - half
            a second of the clock from its end is the file's half second times
            its speed - and as it is when the length is not known (L19). File
            seconds against file seconds (GM). */
        if (range >= 0)
            return true;

        const auto* lengths = handlerDurations();

        if (lengths == nullptr)
            return true;

        const auto found = lengths->find (textOf (cue, "file"));

        if (found == lengths->end() || ! (found->second > 0.0))
            return true;

        return at < found->second - 0.5 * documentSpeedOf (cue);
    }

    Runner::ResumePoint Runner::countedPoint (const Run& run, std::int64_t tick) const
    {
        /*  K8'S COUNT, for a session with no playhead to read (LS): where the
            run's arm began, plus the seconds since `run.started` at the cue's
            speed as the document says it; or the slice it is in, and the point
            it was armed at in the one it was armed into. The hook's playhead
            replaces it on the next tick (`go.dohPlayhead`). */
        ResumePoint at;
        const auto cue = document.findById (run.cue);

        if (! rangesOf (cue).empty())
        {
            at.range = std::max (run.range >= 0 ? run.range : run.startRange, 0);
            at.from = at.range == run.startRange ? run.sliceFrom : 0.0;
            return at;
        }

        const auto elapsed = run.startedAtTick >= 0
                               ? static_cast<double> (tick - run.startedAtTick) / static_cast<double> (TickClock::rateHz)
                               : 0.0;

        at.from = run.armedOrigin + std::max (elapsed, 0.0) * documentSpeedOf (cue);
        return at;
    }

    void Runner::publishResume (const std::string& listId)
    {
        const auto found = marks.find (listId);

        if (found == marks.end() || ! found->second.root.has_value())
        {
            lists.setResume (listId, {});
            return;
        }

        /*  THE CUE, AND THE SECOND IT CARRIES ON FROM: a media cue's second of
            its file, a scene's own second; a mic has no position, and says only
            its cue. */
        const auto& root = *found->second.root;
        auto text = found->second.cue;

        if (root.kind == DohRoot::Kind::media)
        {
            auto second = root.offset;

            if (root.range >= 0)
            {
                const auto ranges = rangesOf (document.findById (root.cue));

                if (root.range < static_cast<int> (ranges.size()))
                {
                    const auto& slice = ranges[static_cast<std::size_t> (root.range)];
                    const auto length = slice.out - slice.in;
                    second = slice.in + (length > 0.0 ? std::fmod (root.sliceFrom, length) : 0.0);
                }
            }

            text += " " + osc::formatDouble (second);
        }
        else if (root.kind == DohRoot::Kind::tree)
        {
            for (const auto& wants : root.plan)
                if (wants.cue == root.cue)
                {
                    text += " " + osc::formatDouble (wants.offset);
                    break;
                }
        }

        lists.setResume (listId, text);
    }

    void Runner::dropRoot (Engine& engine, std::int64_t tick, const std::string& listId,
                           const std::string& carriedOn)
    {
        const auto found = marks.find (listId);

        if (found == marks.end() || ! found->second.root.has_value())
            return;

        const auto root = *found->second.root;
        const auto markedCue = found->second.cue;
        found->second.root.reset();

        /*  A MARK LIVES WHILE IT HAS A ROOT OR AN ENTRY (GN, namespace draft §24.12). */
        if (found->second.left.empty())
            marks.erase (found);

        lists.setResume (listId, {});

        /*  NO ARM AT THE POINT OUTLIVES ITS ROOT (GN, red team B2): silent,
            with no pre-wait, a GO later would launch it as if the resume stood.
            Revoked - voice and slots back; it pre-sent nothing - but for the
            run that spent the resume. */
        std::vector<std::string> arms;

        for (const auto& run : runs.all())
            if (run.id != carriedOn && isResumeArm (run, root))
                arms.push_back (run.id);

        for (const auto& id : arms)
            revokePrepared (engine, tick, id);

        /*  AND THE STANDBY MAKES THE CUE READY AFRESH when it stands on it -
            at its own offset and level, with its pre-wait. */
        if (const auto list = document.findById (listId);
            list.isValid() && list[juce::Identifier ("standby")].toString().toStdString() == markedCue)
            resetAudioPreparation();
    }

    void Runner::dropMark (Engine& engine, std::int64_t tick, const std::string& listId)
    {
        dropRoot (engine, tick, listId);
        marks.erase (listId);
        lists.setResume (listId, {});
    }

    void Runner::setMark (Engine& engine, std::int64_t tick, const std::string& listId, DohMark mark)
    {
        dropMark (engine, tick, listId);

        if (mark.root.has_value() || ! mark.left.empty())
            marks[listId] = std::move (mark);

        publishResume (listId);
    }

    bool Runner::handedBackIn (const std::string& runId, std::int64_t tick) const
    {
        const auto* run = runs.find (runId);
        return run != nullptr && run->unadoptedAt >= 0 && run->unadoptedAt == tick;
    }

    void Runner::seekingRun (Engine& engine, std::int64_t tick, const std::string& runId)
    {
        const auto* run = runs.find (runId);

        if (run == nullptr || ! run->resumes || hasLaunchEvidence (*run))
            return;

        std::string listId;

        for (const auto& entry : marks)
            if (entry.second.root.has_value() && isResumeArm (*run, *entry.second.root))
            {
                listId = entry.first;
                break;
            }

        /*  SPENT ON THE RUN THE HAND PUT SOMEWHERE, NOT REVOKED (GN, namespace draft §24.12): it plays
            where it was sought, arriving through its own de-click, and a later
            GO on the cue is decision N's "already sounding" - never a second
            copy at the point. */
        if (! listId.empty())
            dropRoot (engine, tick, listId, runId);
    }

    void Runner::snapshotAdoption (const std::string& runId, const std::string& parentGroup)
    {
        /*  ONLY FOR THE GO THE RECORD IS OPEN FOR: an engine fire whose cause is
            an older GO adopts for that GO, which no Doh can take back now. And
            once per block: a block nested in one already noted is in it. */
        if (currentGo == 0 || goRecord.serial != currentGo)
            return;

        for (const auto& adoption : goRecord.adoptions)
            for (const auto& before : adoption.runs)
                if (before.id == runId)
                    return;

        Adoption adoption;
        adoption.root = runId;
        adoption.parentGroup = parentGroup;

        std::vector<std::string> inBlock { runId };

        for (const auto* below : runs.descendantsOf (runId))
            inBlock.push_back (below->id);

        for (const auto& id : inBlock)
            if (const auto* run = runs.find (id))
            {
                RunBefore before;
                before.id = run->id;
                before.state = run->state;
                before.prepare = run->prepare;
                before.parent = run->parent;
                before.enterAt = run->enterAt;
                before.launchRequested = run->launchRequested;
                before.launchRequestedAtTick = run->launchRequestedAtTick;
                before.dueTick = run->dueTick;
                before.round = run->round;
                before.iteration = run->iteration;
                before.seed = run->seed;
                before.roundStartedAtTick = run->roundStartedAtTick;
                before.firstRoundAtTick = run->firstRoundAtTick;
                adoption.runs.push_back (std::move (before));
            }

        /*  AND THE JOBS OF THE GROUPS IN IT, copied: hook-consumed, and what
            the scheduler carries the block on from once it is handed back. */
        for (const auto& job : scheduled)
            if (! job.retired && std::find (inBlock.begin(), inBlock.end(), job.run) != inBlock.end())
                adoption.jobs.push_back (job);

        goRecord.adoptions.push_back (std::move (adoption));
    }

    void Runner::unadopt (Engine& engine, std::int64_t tick, const Adoption& adoption, std::uint64_t serial)
    {
        std::set<std::string> before;

        for (const auto& was : adoption.runs)
            before.insert (was.id);

        /*  WHAT THE GO MADE UNDER IT SINCE - its header's remainder, the members
            its entry spawned - ended here, and taken back: none of it was
            heard, and the corrected GO makes it again. The horizon's own work
            under it carries no serial, and is given back by its own road. */
        std::vector<std::string> madeSince;

        for (const auto* below : runs.descendantsOf (adoption.root))
            if (below->goSerial == serial && before.count (below->id) == 0)
                madeSince.push_back (below->id);

        for (const auto& id : madeSince)
        {
            /*  BUT A FADE OR A STOP IT FIRED IS LEFT TO RUN, as the take-down
                leaves one (D1): its target is not the GO's, and what it moved is
                D3's to put back. */
            if (const auto* run = runs.find (id);
                run != nullptr && ! ((run->kind == "fade" || run->kind == "transport")
                                       && hasLaunchEvidence (*run) && ! run->isWaiting()))
                endHere (id, tick);

            if (auto* run = runs.find (id))
                run->takenBack = true;
        }

        /*  EVERY RUN IT HELD, AS IT WAS: its state and its mark, its parent, its
            launch fields and its round; nobody's GO again, with no stop on its
            account; and stamped with this tick, so a record the hooks decided
            in it on the adopted state - a pre-wait's end, a launch placed, a
            member spawned or launched - is applied and ignored (GZ). A voice
            whose launch was placed is stopped and asked for again on its track
            - live only, as a seek does; a replay has no voice. */
        for (const auto& was : adoption.runs)
        {
            auto* run = runs.find (was.id);

            if (run == nullptr || run->isFinished())
                continue;

            auto armAgain = false;

            if (run->launchedAtSample > 0)
            {
                if (audio != nullptr && run->track >= 0)
                {
                    if (run->kind == "mic")
                        audio->shutLive (run->track, 0.0);
                    else
                        audio->stop (run->track);

                    armAgain = run->kind == "media";
                }

                run->launchedAtSample = 0;
                run->sawPlaying = false;
                run->stopIssued = false;
                run->killIssued = false;

                if (armAgain)
                    run->armConfirmed = false;
            }

            run->state = was.state;
            run->prepare = was.prepare;
            run->enterAt = was.enterAt;
            run->launchRequested = was.launchRequested;
            run->launchRequestedAtTick = was.launchRequestedAtTick;
            run->dueTick = was.dueTick;
            run->round = was.round;
            run->iteration = was.iteration;
            run->seed = was.seed;
            run->roundStartedAtTick = was.roundStartedAtTick;
            run->firstRoundAtTick = was.firstRoundAtTick;

            if (run->parent != was.parent)
            {
                const auto now = run->parent;

                if (auto* holder = runs.find (now))
                    holder->children.erase (std::remove (holder->children.begin(), holder->children.end(), was.id),
                                            holder->children.end());

                if (auto* holder = runs.find (was.parent))
                    if (std::find (holder->children.begin(), holder->children.end(), was.id) == holder->children.end())
                        holder->children.push_back (was.id);

                run = runs.find (was.id);
                run->parent = was.parent;
            }

            run->goSerial = 0;
            run->stopAsked = false;
            run->stopAskedBy = 0;
            run->askedAgain = false;
            run->stopEndsWait = false;
            run->postWaitBegan = -1;
            run->sendsLeft = false;
            run->takenBack = false;
            run->unadoptedAt = tick;

            if (armAgain)
            {
                const auto cue = document.findById (run->cue);
                auto level = numberOf (cue, "level");

                if (run->deClick)
                {
                    level = silenceDb;
                    run->deClickOwed = true;
                }

                if (cue.isValid())
                    requestArmOn (engine, cue, *run, level);
            }
        }

        /*  ITS JOBS AS THEY WERE, the adopted ones let go of - and each has
            TAKEN what the GO made under its group since: those runs stay its
            children, over, and a phase finding a finished run of one of its
            cues that no job has taken walks past the cue as already run, or
            counts it as a member played. Taken, the corrected GO's entry makes
            each cue afresh. */
        for (const auto& copy : adoption.jobs)
        {
            for (auto& job : scheduled)
                if (job.run == copy.run)
                    job.retired = true;

            auto again = copy;
            again.retired = false;

            for (const auto& id : madeSince)
                if (const auto* run = runs.find (id);
                    run != nullptr && run->parent == again.run && ! again.hasTaken (id))
                    again.taken.push_back (id);

            scheduled.push_back (again);
        }

        /*  AND OUT OF THE RUNNING ACT THAT TOOK IT: not one of its members until
            a GO asks for it again, which the act's job then takes as ever. */
        if (! adoption.parentGroup.empty())
            for (auto& job : scheduled)
            {
                if (job.run != adoption.parentGroup || job.retired)
                    continue;

                job.taken.erase (std::remove (job.taken.begin(), job.taken.end(), adoption.root), job.taken.end());

                if (const auto at = std::find (job.phaseRuns.begin(), job.phaseRuns.end(), adoption.root);
                    at != job.phaseRuns.end())
                {
                    const auto index = static_cast<std::size_t> (at - job.phaseRuns.begin());
                    job.phaseRuns.erase (at);

                    if (index < job.launched)
                        --job.launched;
                }

                if (job.awaiting == adoption.root)
                    job.awaiting.clear();
            }
    }

    void Runner::adoptIntoParentJob (const std::string& groupRun, const std::string& runId)
    {
        /*  A MEMBER IT HAS LAUNCHED: taken, and among the runs of its phase
            before `launched`, which then counts it - so the job waits for it,
            runs its footer after it, and does not launch it again. */
        for (auto& job : scheduled)
        {
            if (job.run != groupRun || job.retired)
                continue;

            if (! job.hasTaken (runId))
                job.taken.push_back (runId);

            if (std::find (job.phaseRuns.begin(), job.phaseRuns.end(), runId) == job.phaseRuns.end())
            {
                const auto at = std::min (job.launched, job.phaseRuns.size());
                job.phaseRuns.insert (job.phaseRuns.begin() + static_cast<std::ptrdiff_t> (at), runId);
                job.launched = at + 1;
            }
        }
    }

    void Runner::noteDohPlayheads (Engine& engine)
    {
        /*  ON THE FIRST TICK AFTER THE PRESS, AND BEFORE `updatePositions`
            MOVES IT ON (K8's road, LQ): what each readout holds now is what the
            press's own tick made of the sample clock - the press drains after
            the hooks - so it is where the run was when Doh! was pressed, not
            where the Doh fade has taken it since. Reported as a record, so the
            handler that writes it into the root, and a replay, which has no
            playhead, read the same second. A run whose launch was never placed
            has no playhead and keeps the handler's count. */
        if (playheadsOwed.empty())
            return;

        const auto toRead = std::move (playheadsOwed);
        playheadsOwed.clear();

        if (audio == nullptr)
            return;

        for (const auto& id : toRead)
        {
            const auto* run = runs.find (id);

            if (run == nullptr || run->kind != "media" || run->launchedAtSample <= 0)
                continue;

            auto range = -1;
            auto from = run->position;

            if (run->rangeStartedAtSample > 0)
            {
                range = std::max (run->range >= 0 ? run->range : run->startRange, 0);
                from = run->slicePlayed;
            }

            engine.submit (origin::engine, "go.dohPlayhead",
                           { osc::Value::string (id), osc::Value::float64 (from),
                             osc::Value::int32 (static_cast<std::int32_t> (range)) });
        }
    }

    void Runner::notePlayheadOf (Engine& engine, std::int64_t tick, const std::string& runId,
                                 double from, int range)
    {
        for (auto& entry : marks)
        {
            if (! entry.second.root.has_value())
                continue;

            const auto listId = entry.first;
            auto& root = *entry.second.root;

            if (root.kind == DohRoot::Kind::media && root.run == runId)
            {
                if (range >= 0)
                {
                    root.range = range;
                    root.sliceFrom = from;
                    root.offset = 0.0;
                }
                else
                {
                    root.offset = from;
                }

                /*  ALL BUT OVER THERE, by the length the log knows: it ended
                    naturally under the Doh (ruling 16), and the next GO starts
                    it from its top. */
                if (! notOver (document.findById (root.cue), root.range, root.range >= 0 ? root.sliceFrom : root.offset))
                {
                    dropRoot (engine, tick, listId);
                    return;
                }

                /*  AN ARM MADE AT THE COUNTED POINT BEFORE THIS ARRIVED is armed
                    at the wrong second (D2's review, NC): revoked, and the
                    standby arms the cue again at the playhead's - or, for a
                    member under its act, the corrected GO seats it there. */
                std::vector<std::string> early;

                for (const auto& run : runs.all())
                    if (isResumeArm (run, root))
                        early.push_back (run.id);

                for (const auto& id : early)
                    revokePrepared (engine, tick, id);

                if (! early.empty())
                    if (const auto list = document.findById (listId);
                        list.isValid() && list[juce::Identifier ("standby")].toString().toStdString() == entry.second.cue)
                        resetAudioPreparation();

                publishResume (listId);
                return;
            }

            if (root.kind != DohRoot::Kind::tree)
                continue;

            const auto heard = root.heardRuns.find (runId);

            if (heard == root.heardRuns.end())
                continue;

            const auto cueId = heard->second;

            for (auto& wants : root.plan)
            {
                if (wants.cue != cueId || wants.when != planned::sounding)
                    continue;

                if (! notOver (document.findById (cueId), range, from))
                {
                    wants.when = planned::finished;
                }
                else if (range >= 0)
                {
                    wants.range = range;
                    wants.offset = 0.0;
                    root.arrival.sliceFrom[cueId] = from;
                }
                else
                {
                    wants.offset = from;
                }
            }

            publishResume (listId);
            return;
        }
    }

    std::optional<Runner::DohRoot> Runner::rootFor (const std::string& rootId, std::uint64_t serial,
                                                    std::int64_t tick, const std::vector<std::string>& leftCues,
                                                    int fadeTicks) const
    {
        /*  NAMESPACE DRAFT §24.12, FROM HANDLER STATE AT THE PRESS. Carried on only what was
            heard, is unfinished, holds no post-wait and that nothing has asked
            to stop - a scene's own stop cue aimed at itself included. */
        const auto* root = runs.find (rootId);

        if (root == nullptr || root->isFinished() || root->stopAsked || root->state == runState::postWait)
            return std::nullopt;

        DohRoot out;
        out.cue = root->cue;
        out.run = rootId;
        out.landsAt = tick + fadeTicks;

        /*  THE ACT IT SAT IN: a member of an older live manual group (HF) is
            carried on in it. A root under anything else - a scene the machine
            runs, which spawned it - starts from its top: that scene would not
            wait for it. */
        if (! root->parent.empty())
        {
            const auto* above = runs.find (root->parent);

            if (above == nullptr || above->isFinished() || ! above->isGroup()
                  || ! isManualGroup (document.findById (above->cue)))
                return std::nullopt;

            out.underRun = above->id;
        }

        const auto cue = document.findById (root->cue);

        /*  A SOUND: where its file had got to, by the count until the hook's
            playhead arrives - and not at all when it was all but over there,
            by the length the log knows (ruling 16): it ended under the Doh. */
        if (root->kind == "media")
        {
            const auto at = countedPoint (*root, tick);

            if (! notOver (cue, at.range, at.from))
                return std::nullopt;

            out.kind = DohRoot::Kind::media;

            if (at.range >= 0)
            {
                out.range = at.range;
                out.sliceFrom = at.from;
            }
            else
            {
                out.offset = at.from;
            }

            /*  A level is a job's, read here to shape the arrival and nothing
                else. */
            out.levelDb = root->ownLevel;
            return out;
        }

        if (root->kind == "mic")
        {
            out.kind = DohRoot::Kind::mic;
            out.levelDb = root->ownLevel;
            return out;
        }

        if (! root->isGroup())
            return std::nullopt;

        out.kind = DohRoot::Kind::tree;

        /*  THE SCENE'S OWN: the root and its descendants of this GO (GL) -
            never the horizon's next block under it, never what an older act
            ran because of the GO - in the order they were made. */
        std::set<std::string> mine { rootId };

        for (const auto* below : runs.descendantsOf (rootId))
            if (below->goSerial == serial && below->causedBy == 0)
                mine.insert (below->id);

        std::vector<const Run*> inOrder;

        for (const auto& run : runs.all())
            if (mine.count (run.id) > 0)
                inOrder.push_back (&run);

        /*  EVERY SCENE IN IT HAS REACHED ITS MEMBERS, BY LAUNCH EVIDENCE (FS,
            red team A major) - not a member's run merely existing, which the
            horizon's arms adopted with a block always are - and none is in its
            footer, loops, shuffles, plays some of its members or is a sampler
            bank (ruling 17, L4): those start from their top. */
        for (const auto* run : inOrder)
        {
            if (! run->isGroup() || run->isFinished())
                continue;

            const auto groupCue = document.findById (run->cue);
            const auto members = membersOf (groupCue);
            const auto play = numberOf (groupCue, "play");

            if (run->state == runState::preparing || run->iterations != 1
                  || textOf (groupCue, "selection") == "shuffle" || textOf (groupCue, "mode") == "sampler"
                  || (play > 0.0 && play < static_cast<double> (members.size())))
                return std::nullopt;

            auto reached = false;

            for (const auto* child : runs.childrenOf (run->id))
            {
                if (isFooterCueOf (*run, child->cue))
                    return std::nullopt;

                if (std::find (members.begin(), members.end(), child->cue) != members.end()
                      && hasLaunchEvidence (*child))
                    reached = true;
            }

            if (! reached)
                return std::nullopt;
        }

        /*  ONE ROW PER CUE, from its newest run. */
        std::vector<std::string> order;
        std::map<std::string, const Run*> newest;

        for (const auto* run : inOrder)
        {
            if (newest.count (run->cue) == 0)
                order.push_back (run->cue);

            newest[run->cue] = run;
        }

        const auto ancestorsOf = [this] (const Run& run)
        {
            std::vector<std::string> path;

            for (auto at = run.parent; ! at.empty();)
            {
                const auto* above = runs.find (at);

                if (above == nullptr)
                    break;

                path.insert (path.begin(), above->cue);
                at = above->parent;
            }

            return path;
        };

        const auto seconds = [] (std::int64_t ticks)
        {
            return static_cast<double> (ticks) / static_cast<double> (TickClock::rateHz);
        };

        struct Fired
        {
            const Run* run;
            std::size_t row;
            std::string destination;
        };

        std::vector<PlannedRun> groups, rest;
        std::vector<Fired> sends;
        std::vector<std::pair<const Run*, std::size_t>> fades;

        for (const auto& cueId : order)
        {
            const auto* run = newest[cueId];
            const auto cueNode = document.findById (cueId);

            PlannedRun wants;
            wants.cue = cueId;
            wants.ancestors = ancestorsOf (*run);

            const auto* parent = runs.find (run->parent);
            const auto inSequence = run->id != rootId && parent != nullptr && parent->isGroup()
                                      && textOf (document.findById (parent->cue), "mode") != "timeline";
            const auto waiting = run->state == runState::waiting;
            const auto remaining = seconds (std::max<std::int64_t> (0, run->dueTick - tick));

            /*  SPAWNED AND NEVER LAUNCHED - a sequence's next member - is due at
                its own pre-wait (the design's "anything else" row; ME), in a scene the machine
                times all at once. IN A SEQUENCE IT IS LEFT TO THE SEQUENCE
                (implementer's call): seated due, its pre-wait would run out
                beside the member still playing and it would sound early;
                left out, the seated sequence spawns it again after that member,
                as it would have. */
            auto keep = true;

            const auto never = [&]
            {
                if (inSequence)
                    return false;

                wants.when = planned::due;
                wants.startsIn = seconds (run->preWaitTicks);
                return true;
            };

            if (run->isGroup())
            {
                if (run->isFinished() || run->state == runState::postWait)
                {
                    wants.when = planned::finished;
                }
                else if (waiting)
                {
                    wants.when = planned::due;
                    wants.startsIn = remaining;
                }
                else if (hasLaunchEvidence (*run))
                {
                    wants.when = planned::sounding;
                    wants.offset = seconds (tick - run->launchRequestedAtTick);
                    out.arrival.levels[cueId] = run->ownLevel;
                }
                else
                {
                    keep = never();
                }

                if (keep)
                    groups.push_back (wants);

                continue;
            }

            const auto kind = run->kind;

            if (kind == "media")
            {
                if (run->isFinished() || run->state == runState::postWait)
                {
                    wants.when = planned::finished;
                }
                else if (waiting)
                {
                    wants.when = planned::due;
                    wants.startsIn = remaining;
                }
                else if (run->startedAtTick >= 0)
                {
                    /*  HEARD: where it was, by the count until the playhead
                        arrives; over, when the log's length says so. */
                    const auto at = countedPoint (*run, tick);

                    if (! notOver (cueNode, at.range, at.from))
                    {
                        wants.when = planned::finished;
                    }
                    else
                    {
                        wants.when = planned::sounding;
                        wants.range = at.range;
                        wants.offset = at.range >= 0 ? 0.0 : at.from;

                        if (at.range >= 0)
                            out.arrival.sliceFrom[cueId] = at.from;

                        out.arrival.levels[cueId] = run->ownLevel;
                        out.heardRuns[run->id] = cueId;
                    }
                }
                else if (run->launchRequestedAtTick > 0)
                {
                    /*  ASKED FOR AND NOT YET HEARD: at its start. */
                    const auto ranged = ! rangesOf (cueNode).empty();

                    wants.when = planned::sounding;
                    wants.range = ranged ? run->startRange : -1;
                    wants.offset = ranged ? 0.0 : run->armedOrigin;

                    if (ranged && run->sliceFrom > 0.0)
                        out.arrival.sliceFrom[cueId] = run->sliceFrom;
                }
                else
                {
                    keep = never();
                }
            }
            else if (kind == "mic")
            {
                if (run->isFinished() || run->state == runState::postWait)
                {
                    wants.when = planned::finished;
                }
                else if (waiting)
                {
                    wants.when = planned::due;
                    wants.startsIn = remaining;
                }
                else if (hasLaunchEvidence (*run))
                {
                    wants.when = planned::sounding;
                    out.arrival.levels[cueId] = run->ownLevel;
                }
                else
                {
                    keep = never();
                }
            }
            else if (kind == "fade")
            {
                if (waiting)
                {
                    wants.when = planned::due;
                    wants.startsIn = remaining;
                }
                else if (hasLaunchEvidence (*run))
                {
                    /*  Decided below, once every sound's row is known. */
                    wants.when = planned::due;
                    fades.emplace_back (run, rest.size());
                }
                else if (run->isFinished())
                {
                    wants.when = planned::finished;
                }
                else
                {
                    keep = never();
                }
            }
            else if (kind == "transport" || kind == "osc" || kind == "midi" || kind == "start")
            {
                if (waiting)
                {
                    wants.when = planned::due;
                    wants.startsIn = remaining;
                }
                else if (hasLaunchEvidence (*run))
                {
                    /*  WHAT IT HAD FIRED FIRES AGAIN, FROM ITS START (ruling 15):
                        a stop, a start, and a send that takes back - MIDI, an
                        event and a write to an opaque device included, the
                        corrected GO a first GO for them (the author, 2026-09-30).
                        BUT NOT A SEND THAT REACHED A DEVICE LEFT TO ITS OPERATOR
                        (the author, 2026-10-01, HJ): planned over, so no run is
                        seated for it and nothing is sent. */
                    const auto sends_ = kind == "osc" || kind == "midi";

                    if (sends_ && std::find (leftCues.begin(), leftCues.end(), cueId) != leftCues.end())
                    {
                        wants.when = planned::finished;
                    }
                    else
                    {
                        wants.when = planned::due;

                        if (sends_)
                            sends.push_back ({ run, rest.size(),
                                               kind == "osc" ? "osc " + textOf (cueNode, "address")
                                                             : "midi " + textOf (cueNode, "port") });
                    }
                }
                else if (run->isFinished())
                {
                    wants.when = planned::finished;
                }
                else
                {
                    keep = never();
                }
            }
            else
            {
                /*  A memo, and anything else that only marks time. */
                if (waiting)
                {
                    wants.when = planned::due;
                    wants.startsIn = remaining;
                }
                else if (hasLaunchEvidence (*run) || run->isFinished())
                {
                    wants.when = planned::finished;
                }
                else
                {
                    keep = never();
                }
            }

            if (keep)
                rest.push_back (wants);
        }

        /*  ONE TICK APART PER DESTINATION (red team B minor 1): the sender keeps
            one message per address per flush, the last winning, so two sends
            the scene had made to one address would leave as one. Counted in
            the order they were first sent. */
        std::stable_sort (sends.begin(), sends.end(), [] (const Fired& a, const Fired& b)
        {
            return a.run->launchRequestedAtTick < b.run->launchRequestedAtTick;
        });

        std::map<std::string, int> perDestination;

        /*  The first at once; the k-th at k + 1 ticks, since the scheduler's
            first look at a seated wait is the tick after the seat, when a wait
            due on the seat's tick and one due a tick later both fire. */
        for (const auto& fired : sends)
        {
            const auto k = perDestination[fired.destination]++;
            rest[fired.row].startsIn = seconds (k == 0 ? 0 : k + 1);
        }

        /*  ITS OWN FADES ON ITS OWN SOUNDS ARE NOT FIRED AGAIN (HG): fired
            again, a fade took over from the sound's arrival and brought it in
            from silence. A finished one is already in the level the sound
            arrives at; one still moving is carried on as the rest of that
            arrival. A fade on anything the scene does not seat sounding - a
            cue outside it, a member over or still to come, a DCA, a speed - is
            fired again from its start, as the first GO fired it. */
        const auto rowOf = [&groups, &rest] (const std::string& cueId) -> PlannedRun*
        {
            for (auto& row : groups)
                if (row.cue == cueId)
                    return &row;

            for (auto& row : rest)
                if (row.cue == cueId)
                    return &row;

            return nullptr;
        };

        for (const auto& [fadeRun, index] : fades)
        {
            const auto fadeCue = document.findById (fadeRun->cue);
            const auto target = textOf (fadeCue, "target");

            if (textOf (fadeCue, "levelOn") == "false" || ! textOf (fadeCue, "dca").empty())
                continue;

            auto* seated = rowOf (target);

            if (seated == nullptr || seated->when != planned::sounding)
                continue;

            if (fadeRun->isFinished())
            {
                rest[index].when = planned::finished;
                continue;
            }

            const auto targetNode = document.findById (target);

            /*  A SCENE'S LEVEL is carried, and a fade still moving on it is
                fired again from there (L7). */
            if (targetNode.hasType ("Group"))
                continue;

            auto drawn = doc::readFadePoints (textOf (fadeCue, "points"));

            FadeSegment segment;
            segment.toDb = drawn.points.empty() ? numberOf (fadeCue, "level") : drawn.points.back().levelDb;
            segment.ticks = std::max (ticksFor (numberOf (fadeCue, "duration"))
                                        - static_cast<int> (tick - fadeRun->launchRequestedAtTick), 1);
            segment.curve = fadeCurveFrom (textOf (fadeCue, "curve"));
            segment.points = std::move (drawn.points);
            segment.stopWhenDone = textOf (fadeCue, "stopWhenDone") == "true";

            rest[index].when = planned::finished;

            if (targetNode.hasType ("Mic"))
            {
                /*  A FADE-AND-STOP STILL CLOSING ONE OF ITS MICS plans the mic
                    over (L7); a plain one is the mic's own job, from where it
                    was. */
                if (segment.stopWhenDone)
                {
                    seated->when = planned::finished;
                    out.arrival.levels.erase (target);
                }
                else
                {
                    out.arrival.micFades[target] = segment;
                }

                continue;
            }

            out.arrival.carryOn[target] = segment;
        }

        out.plan = groups;
        out.plan.insert (out.plan.end(), rest.begin(), rest.end());
        return out;
    }

    Runner::Resumed Runner::resumeStandby (Engine& engine, std::int64_t tick, const juce::ValueTree& list,
                                           const DohMark& mark, const std::vector<std::string>& supplied)
    {
        Resumed out;

        /*  COPIED: the seat below grows the run table, and `endGo` drops the
            mark this came from. */
        const auto root = *mark.root;
        const auto standby = mark.cue;

        std::size_t taken = 0;

        const auto nextId = [&]
        {
            auto id = taken < supplied.size() ? supplied[taken] : std::string {};
            ++taken;

            if (id.empty())
                id = ids.generate();

            out.made.push_back (id);
            return id;
        };

        /*  THE ACT IT SAT IN, GONE OR BEING STOPPED (GH: by `stopAsked`, never
            `state`): nothing to carry it on in. The GO fires the cue from its
            top, as any GO - and its step is not back-dated. */
        if (! root.underRun.empty())
            if (const auto* act = runs.find (root.underRun);
                act == nullptr || act->isFinished() || act->stopAsked)
            {
                out.made = fireStandby (engine, tick, list, standby, supplied);
                return out;
            }

        /*  THE GROUPS ABOVE IT, outermost first, which a seat keeps. */
        std::map<std::string, std::string> runFor;
        std::vector<std::string> above;

        for (auto at = root.underRun; ! at.empty();)
        {
            const auto* group = runs.find (at);

            if (group == nullptr)
                break;

            runFor[group->cue] = group->id;
            above.insert (above.begin(), group->cue);
            at = group->parent;
        }

        const auto cue = document.findById (root.cue);

        /*  NOTHING TO SEAT (D2's review, NB) - the cue gone from the show since
            the Doh, a scene with no row - and the GO is the one it always was,
            rather than a GO that does nothing. Decided before anything is
            drawn, so the record's identifiers are the plain GO's. */
        if (! cue.isValid() || (root.kind == DohRoot::Kind::tree && root.plan.empty()))
        {
            out.made = fireStandby (engine, tick, list, standby, supplied);
            return out;
        }

        //----------------------------------------------------------------------
        /*  A SCENE, RE-SEATED (namespace draft §24.12): where it was at the press, its members at
            their seconds, what it had fired fired again, what it had sent to a
            device left to its operator not; its own fades on its own sounds
            carried on rather than fired again; under the act it sat in. Its
            mics' old tails are cut first, so the seated gate does not queue
            behind its own reverb (L10). */
        if (root.kind == DohRoot::Kind::tree)
        {
            for (const auto& wants : root.plan)
                if (wants.when == planned::sounding && document.findById (wants.cue).hasType ("Mic"))
                {
                    std::vector<std::string> tails;

                    for (const auto& snapshot : runs.all())
                        if (snapshot.cue == wants.cue && ! snapshot.isFinished() && snapshot.takenBack)
                            tails.push_back (snapshot.id);

                    for (const auto& id : tails)
                        if (auto* tail = runs.find (id))
                        {
                            tail->skipFooter = true;
                            resolveTakeover (id);
                        }
                }

            auto arrival = root.arrival;
            seatPlan (engine, tick, root.plan, runFor, nextId, &arrival);

            const auto seated = runFor.count (root.cue) > 0 ? runFor[root.cue] : std::string {};

            if (! root.underRun.empty() && ! seated.empty())
                adoptIntoParentJob (root.underRun, seated);

            /*  ITS SOUNDS STILL TO COME, armed now - a scene armed them at its
                entry - and kept waiting (`requestArmOn`). */
            for (const auto& wants : root.plan)
            {
                if (wants.when != planned::due)
                    continue;

                const auto found = runFor.find (wants.cue);

                if (found == runFor.end())
                    continue;

                const auto node = document.findById (wants.cue);

                if (node.hasType ("Media"))
                    armMedia (engine, node, found->second);
                else if (node.hasType ("Mic"))
                    armMic (engine, node, found->second);
            }

            /*  A PLAIN FADE STILL MOVING ON ONE OF ITS MICS, a job of its own
                from where the mic was, held until it is heard. */
            for (const auto& [micCue, segment] : arrival.micFades)
            {
                const auto found = runFor.find (micCue);

                if (found == runFor.end())
                    continue;

                const auto level = arrival.levels.find (micCue);

                FadeJob job;
                job.target = found->second;
                job.reportsSelf = false;
                job.fromDb = level != arrival.levels.end() ? level->second : 0.0;
                job.toDb = segment.toDb;
                job.ticksTotal = std::max (segment.ticks, 1);
                job.curve = segment.curve;
                job.points = segment.points;
                job.waitsForLaunch = true;
                running.push_back (job);
            }

            /*  AND A MIC'S TAKE HEARS THIS GO, as a seated mic's does (EP). */
            for (const auto& wants : root.plan)
                if (wants.when == planned::sounding)
                    if (const auto node = document.findById (wants.cue); node.hasType ("Mic"))
                        if (const auto found = runFor.find (wants.cue); found != runFor.end())
                            applyTakeOnGo (node, found->second);

            out.resumed = ! seated.empty();
            out.carriedOn = seated;
            return out;
        }

        //----------------------------------------------------------------------
        /*  A SOUND - at the top of its list, or a member of the act it sat in
            (HF). */
        auto* old = runs.find (root.run);

        /*  IN PLACE, INSIDE THE DOH FADE: the same run, back up from where the
            fade has it to the level it had, over the de-click - when nothing but
            the Doh has asked it to stop since (HH): Esc, a stop or a kill from
            the running pane, a double Esc or a kill of its act each send the GO
            to the arm or the seat instead, which make it again at the point. */
        if (old != nullptr && ! old->isFinished() && old->takenBack && tick < root.landsAt
              && ! old->askedAgain && ! old->killed && ! old->skipFooter)
        {
            resolveTakeover (root.run);

            old = runs.find (root.run);
            old->state = runState::playing;
            old->takenBack = false;
            old->stopAsked = false;
            old->stopAskedBy = 0;
            old->askedAgain = false;
            old->stopEndsWait = false;
            old->postWaitBegan = -1;

            stampSubtree (root.run, currentGo);

            if (root.kind == DohRoot::Kind::media)
            {
                FadeJob up;
                up.target = root.run;
                up.reportsSelf = false;
                up.fromDb = old->ownLevel;
                up.toDb = root.levelDb;
                up.ticksTotal = deClickTicks;
                up.curve = FadeCurve::linear;
                running.push_back (up);
            }
            else
            {
                /*  A MIC'S GATE OPENS AGAIN over the de-click, where the Doh shut
                    it (live only, a Player call), and its take hears the GO. */
                if (audio != nullptr && old->track >= 0 && samplesPerTick > 0)
                    audio->openLive (old->track,
                                     audio->samplesElapsed() + static_cast<std::int64_t> (latencyTicks()) * samplesPerTick,
                                     static_cast<double> (deClickTicks) / static_cast<double> (TickClock::rateHz));

                applyTakeOnGo (cue, root.run);
            }

            /*  BACK AMONG THE ACT'S MEMBERS, as one it has launched: the Doh's
                surgery undone. */
            if (! root.underRun.empty())
                adoptIntoParentJob (root.underRun, root.run);

            out.made.push_back (root.run);
            out.resumed = true;
            out.carriedOn = root.run;
            return out;
        }

        /*  A MIC WHOSE OLD RUN STILL HOLDS ITS CHANNEL - its tail ringing - has
            the tail cut first (L10): a cut, never a kill, so `run.ended` hands
            the channel on. */
        if (root.kind == DohRoot::Kind::mic && old != nullptr && ! old->isFinished())
        {
            old->skipFooter = true;
            resolveTakeover (root.run);
        }

        /*  WARM: THE ARM THE STANDBY MADE AT THE POINT, launched - at the top
            of the list through the armed branch of an ordinary GO, under its
            act as the act's member the horizon armed. Its arrival is its own,
            already on it. */
        std::string armId;

        for (const auto& candidate : runs.all())
            if (isResumeArm (candidate, root))
                armId = candidate.id;

        if (! armId.empty())
        {
            if (root.underRun.empty())
            {
                const auto id = fire (engine, tick, root.cue, nextId());

                if (id.empty())
                    out.made.pop_back();
                else
                    out.made.back() = id;
            }
            else
            {
                out.made = fireStandby (engine, tick, list, standby, supplied);
            }

            out.resumed = true;
            out.carriedOn = armId;
            return out;
        }

        /*  COLD: SEATED AT THE POINT NOW, arriving over the de-click once its
            disk has answered - and under its act, as one of its members. */
        PlannedRun wants;
        wants.cue = root.cue;
        wants.ancestors = above;
        wants.when = planned::sounding;
        wants.range = root.kind == DohRoot::Kind::media ? root.range : -1;
        wants.offset = root.range >= 0 ? 0.0 : root.offset;

        SeatResume arrival;

        if (root.kind == DohRoot::Kind::media)
        {
            arrival.levels[root.cue] = root.levelDb;

            if (root.range >= 0)
                arrival.sliceFrom[root.cue] = root.sliceFrom;
        }

        seatPlan (engine, tick, std::vector<PlannedRun> { wants }, runFor, nextId, &arrival);

        const auto seated = runFor.count (root.cue) > 0 ? runFor[root.cue] : std::string {};

        if (! root.underRun.empty() && ! seated.empty())
            adoptIntoParentJob (root.underRun, seated);

        if (root.kind == DohRoot::Kind::mic && ! seated.empty())
            applyTakeOnGo (cue, seated);

        out.resumed = ! seated.empty();
        out.carriedOn = seated;
        return out;
    }

    std::string Runner::goDoh (Engine& engine, doc::ShowDocument& editable, std::int64_t tick,
                               const std::vector<std::string>& supplied, std::vector<std::string>& drawn)
    {
        /*  THE IDENTIFIERS ITS PUT-BACK DRAWS (D3): supplied by the record on a
            replay, drawn fresh live, in the order they are asked for - and every
            one of them on the applied record, as a jump's are. */
        std::size_t taken = 0;

        const std::function<std::string()> nextId = [&]
        {
            auto id = taken < supplied.size() ? supplied[taken] : std::string {};
            ++taken;

            if (id.empty())
                id = ids.generate();

            drawn.push_back (id);
            return id;
        };

        //  0. TOO SOON AFTER THE LAST DOH - a bounce, a held key, two hands.
        if (dohTooSoon (tick))
            return reason::tooSoon;

        //  1. NOTHING TO TAKE BACK: no GO yet, already taken back, or forgotten
        //     by a jump - UNLESS THE LAST DOH LEFT A RESUME, which a second press
        //     forgets (the author, 2026-09-30, (b)): the next GO starts the cue
        //     from its top, and the arm waiting silent at the point is revoked
        //     first, so nothing is left for that GO to launch there. What was
        //     left with devices' operators stays (L37).
        //     AND WHEN THE LIVE RECORD IS TOO LATE TO TAKE BACK (D2's review, MY):
        //     a GO on another list since the Doh, its window run out, leaves no
        //     GO a press could take back - and the button offers the forget
        //     then, from `lists/dohForget`, which says the same thing.
        if (goRecord.serial == 0 || dohTooLate (tick))
            if (const auto found = marks.find (lastDohList);
                found != marks.end() && found->second.root.has_value())
            {
                const auto forgotten = found->second.cue;
                dropRoot (engine, tick, lastDohList);
                lastDohTick = tick;

                /*  IN AN AUDIO OUTAGE (D4's review, OJ) the press says what it
                    did, on the readout - every client reads it there: the line
                    already says the audio is out, and nothing else would say
                    the press was taken. */
                if (outage && outage())
                    lists.setDohReport ({ lastDohList, tick, cueLabel (forgotten) + ": the next GO starts it from its top" });

                return {};
            }

        if (goRecord.serial == 0)
            return reason::nothingToTakeBack;

        //  2. A TRIGGER, A FIRE BY NAME OR A PAD OF ITS BANK SINCE THE GO.
        if (goRecord.firedAfter)
            return reason::triggerAfterGo;

        //  3. TOO LATE: past the show's window, or the window is nought.
        if (dohTooLate (tick))
            return reason::tooLate;

        const auto record = goRecord;
        const auto changes = goChanges;
        const auto serial = record.serial;

        /*  WHERE THE GO LEFT THE POINTER, for the report's persistent cues
            (D3): what the restored pointer brings back into the plan, and this
            did not have. Read before the door below moves it. */
        const auto leftAt = document.getAttribute (standbyAddressOf (record.list)).value_or (std::string {});
        const auto ranOutThere = document.getAttribute (finishedAddressOf (record.list)).value_or ("false") == "true";

        /*  WHAT ITS ENABLE AND DISABLE CUES SWITCHED, PUT BACK FIRST (namespace
            draft §27, PS), so a cue the GO switched off is a place the pointer
            may stand again by the time the door below is asked - and switched
            back again if the door refuses, so a refused Doh moves nothing. */
        std::vector<std::pair<std::string, std::string>> switchedNow;

        for (const auto& [cueId, word] : record.overridesBefore)
            if (const auto node = document.findById (cueId); node.isValid())
            {
                switchedNow.emplace_back (cueId, overrideWord (node));
                editable.setOverride (cueId, word);
            }

        //  4. THE POINTER, THROUGH ITS OWN DOOR: a cue gone, or no longer one
        //     the pointer may stand on, refuses the Doh before anything moves.
        if (const auto moved = editable.setAttribute (standbyAddressOf (record.list), record.cue); ! moved.ok)
        {
            for (const auto& [cueId, word] : switchedNow)
                editable.setOverride (cueId, word);

            return moved.reason;
        }

        /*  THE LAST REPORT IS THIS PRESS'S TO REPLACE (D3) - by the record the
            flush composes on the next tick, an empty one included, so a press
            that has nothing to say leaves nothing stale on the readout. Not
            cleared here (D4's review, ON): cleared at the press and written a
            tick later, the line blinked off between the two.

            IN AN AUDIO OUTAGE (L18; D4's review, OJ) that tick waits for the
            clock, with everything the Doh puts back, while the pointer has gone
            back now: the readout says so at once, for this press the handler
            accepted - never for one it refused - and the flush replaces it when
            the audio returns. */
        if (outage && outage())
            lists.setDohReport ({ record.list, tick, "the pointer is back; what it puts back comes when the audio returns" });

        //  5. THE LIST'S FLAG, THE DEBOUNCE AND THE HISTORY: back to where the GO
        //     found them, the GO's own steps gone - by its serial, wherever a seek
        //     moved them - and a `d` in their place, which bumps the step count so
        //     the persistent section is checked again at the restored pointer.
        editable.setAttribute (finishedAddressOf (record.list), record.finishedBefore ? "true" : "false");
        lastGoTick = record.lastGoTickBefore;
        const auto erased = lists.unstepped (serial);
        lists.stepped (record.list, { tick, record.cue, 'd' });

        /*  THAT PASS SENDS A DEVICE LEFT TO ITS OPERATOR NOTHING (HN): the
            persistent OSC and MIDI cues, on every list, whose setting is leave.
            And after an Esc it asserts nothing at all: Esc brought the beds
            down, and a Doh never undoes an Esc - the next step asserts them. */
        persistentLeftAt = lists.stepsTaken();
        persistentAllLeft = record.escapedAfter;
        persistentLeft.clear();

        for (const auto& listNode : document.root().getChildWithName ("Lists"))
        {
            std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& node)
            {
                for (const auto& child : node)
                {
                    const auto element = child.getType().toString();
                    const auto id = child[idProperty].toString().toStdString();

                    if ((element == "Osc" || element == "Midi") && ! id.empty()
                          && dohOf (document, id) == dohSetting::leave)
                        persistentLeft.insert (id);

                    collect (child);
                }
            };

            if (const auto section = listNode.getChildWithName ("Persistent"); section.isValid())
                collect (section);
        }

        //  6. THE GO'S RUNS, and its roots: those whose parent is not the GO's.
        std::vector<std::string> goRuns;
        std::set<std::string> ofTheGo;

        for (const auto& run : runs.all())
            if (run.goSerial == serial)
            {
                goRuns.push_back (run.id);
                ofTheGo.insert (run.id);
            }

        std::vector<std::string> roots;

        for (const auto& id : goRuns)
            if (const auto* run = runs.find (id);
                run->parent.empty() || runs.find (run->parent) == nullptr || ofTheGo.count (run->parent) == 0)
                roots.push_back (id);

        /*  A ROOT'S OWN, for every purpose below: its descendants of this GO. */
        const auto subtreeOf = [this, &ofTheGo] (const std::string& rootId)
        {
            std::vector<std::string> subtree { rootId };

            for (const auto* below : runs.descendantsOf (rootId))
                if (ofTheGo.count (below->id) > 0)
                    subtree.push_back (below->id);

            return subtree;
        };

        /*  THE HORIZON'S WORK MADE AFTER THE GO on its list, wherever it sits,
            and never launched: a block still being made ready, an arm. Not the
            GO's - given back, never taken down (GY). */
        std::vector<std::string> horizon;

        for (const auto& run : runs.all())
            if (! run.isFinished() && run.preparedAfterGo >= static_cast<std::int64_t> (serial)
                  && ofTheGo.count (run.id) == 0 && ! hasLaunchEvidence (run)
                  && (run.state == runState::preparing || run.kind == "media" || run.kind == "mic")
                  && listOfCue (run.cue) == record.list)
                horizon.push_back (run.id);

        /*  THE OLDER MANUAL GROUPS IT REACHED, innermost first: the ones a root
            sits in, then the manual parent of any of them that has ended since
            the GO. */
        std::vector<std::string> reached;

        std::function<void (const std::string&, int)> reach = [&] (const std::string& id, int depth)
        {
            if (depth > 64 || std::find (reached.begin(), reached.end(), id) != reached.end())
                return;

            reached.push_back (id);

            const auto* group = runs.find (id);

            if (group == nullptr || ! group->isFinished() || group->endedAtTick < record.tick)
                return;

            if (const auto* above = runs.find (group->parent);
                above != nullptr && above->isGroup() && isManualGroup (document.findById (above->cue)))
                reach (above->id, depth + 1);
        };

        for (const auto& seed : record.touched)
            reach (seed, 0);

        //  8. CLASSIFIED BEFORE ANYTHING MOVES, from handler state alone.
        //     Heard or not is asked of each group as pass B reaches it, and by
        //     its job afterwards: the same predicate, on the same records.
        std::set<std::string> otherHands;

        for (const auto& rootId : roots)
        {
            const auto* root = runs.find (rootId);

            /*  ANOTHER HAND IS ALREADY STOPPING IT - a stop cue on another
                list, the running pane, a group above it - and it is left to that
                stop, footer and all. */
            if (root->stopAsked && root->stopAskedBy != serial)
                for (const auto& id : subtreeOf (rootId))
                    otherHands.insert (id);
        }

        /*  NOBODY HEARD IT, AND THE GO ADOPTED IT (D2, namespace draft §24.12): a preparation -
            an arm, a prepared block, a member the horizon armed under a running
            act - is handed back exactly, as it was before the GO, so the
            corrected GO is the rehearsed one: its full pre-wait, its header's
            remainder again, the pre-sends still on the desk (the author,
            2026-09-30, (c)). Not after an Esc, which a Doh never undoes. */
        std::vector<const Adoption*> unadopting;
        std::set<std::string> handedBack, unadoptRoots;

        if (! record.escapedAfter)
            for (const auto& rootId : roots)
            {
                if (otherHands.count (rootId) > 0 || heardUnder (rootId, serial))
                    continue;

                for (const auto& adoption : record.adoptions)
                    if (adoption.root == rootId)
                    {
                        unadopting.push_back (&adoption);
                        unadoptRoots.insert (rootId);

                        for (const auto& was : adoption.runs)
                            handedBack.insert (was.id);
                    }
            }

        /*  Whether a run sits under a root handed back - the root included. */
        const auto underHandedBack = [this, &unadoptRoots] (const std::string& id)
        {
            for (auto at = id; ! at.empty();)
            {
                if (unadoptRoots.count (at) > 0)
                    return true;

                const auto* above = runs.find (at);

                if (above == nullptr)
                    break;

                at = above->parent;
            }

            return false;
        };

        /*  EACH REACHED GROUP: brought back to life when the GO completed or
            ended it, it plays once and sits in nothing the machine runs; left
            ended and said when it cannot be; and when the GO was not the last
            thing it waited for, it only loses the GO's member. */
        enum class Fate { revive, leave, surgery };
        std::map<std::string, Fate> fates;
        std::set<std::string> revived;

        /*  For the report (D3): the acts the GO ended that cannot come back,
            and those another hand is stopping. */
        std::vector<std::string> endedNotRevived, otherHandsActs;

        for (const auto& id : reached)
        {
            const auto* group = runs.find (id);

            if (group == nullptr || (group->stopAsked && group->stopAskedBy != serial))
            {
                fates[id] = Fate::leave;

                if (group != nullptr)
                    otherHandsActs.push_back (id);

                continue;
            }

            const auto groupCue = document.findById (group->cue);
            auto round = group->round;

            if (round.empty())
                round = membersOf (groupCue);

            const auto children = runs.childrenOf (id);
            const auto inRound = [&round] (const std::string& cueId)
            {
                return std::find (round.begin(), round.end(), cueId) != round.end();
            };

            auto lastLaunched = false;
            auto launchedFinished = true;

            for (const auto* child : children)
            {
                if (! inRound (child->cue) || ! hasLaunchEvidence (*child))
                    continue;

                if (! round.empty() && child->cue == round.back())
                    lastLaunched = true;

                /*  Read before anything moves: a child act this Doh brings back
                    still counts as ended here, so the act above it - whose
                    footer that end set off - is brought back too. Its fresh
                    job then waits for the child again. */
                if (! child->isFinished())
                    launchedFinished = false;
            }

            const auto endedSince = (group->isFinished() && group->endedAtTick >= record.tick)
                                      || group->state == runState::postWait;
            const auto complete = endedSince || (lastLaunched && launchedFinished);

            const auto* above = runs.find (group->parent);
            const auto parentOk = above == nullptr
                                    || (above->isGroup() && isManualGroup (document.findById (above->cue)));

            if (! complete)
                fates[id] = Fate::surgery;
            else if (group->iterations == 1 && parentOk)
            {
                fates[id] = Fate::revive;
                revived.insert (id);
            }
            else
            {
                fates[id] = Fate::leave;
                endedNotRevived.push_back (id);
            }
        }

        /*  WHAT A REVIVED ACT RAN BECAUSE OF THE GO - its footer - is taken down
            with the GO; what an act left ended ran is left, and said. */
        const auto underRevived = [this, &revived] (const Run& run)
        {
            for (auto at = run.parent; ! at.empty();)
            {
                if (revived.count (at) > 0)
                    return true;

                const auto* above = runs.find (at);

                if (above == nullptr)
                    break;

                at = above->parent;
            }

            return false;
        };

        std::vector<std::string> caused;

        for (const auto& run : runs.all())
            if (run.causedBy == serial && underRevived (run))
                caused.push_back (run.id);

        /*  THE LEFT SET (HK, HO, HQ): every send of the GO - its fire, what it
            adopted and committed, what it caused - that counts as having left
            for a device whose setting is leave; and what the footer of an act
            the Doh cannot bring back sent since the GO. Taken now, before the
            take-down rewrites a run's state. */
        std::vector<std::string> leftCues;

        /*  And for the report, each with the kind and the device it went to
            (D3): a device gone since is named by the identifier the run kept. */
        struct LeftSend
        {
            std::string cue, kind, device;
        };

        std::vector<LeftSend> leftSends;

        /*  AND WHAT LEFT FOR A PORT THAT TAKES BACK (D4, NU): a MIDI message is
            an event - nothing reads it back and nothing can call it off the
            cable - so the report names it, and the next GO sends it again as a
            first GO would (the author, 2026-09-30, (a)). Decided by the same
            walk and the same test as what was left: what counts as having left
            (a cue that found no port, or a port switched off, put nothing on a
            cable and is not named), the setting read here, once (L35). For the
            report only - the corrected GO needs no telling. */
        struct SentAgain
        {
            std::string cue, device, footerOf;
        };

        std::vector<SentAgain> sentAgain;

        /*  `footerOf`: the act whose footer sent it, for a footer the Doh does
            not take down with the GO - it goes again when that act ends again. */
        const auto consider = [this, &leftCues, &leftSends, &sentAgain] (const Run& run, const std::string& footerOf)
        {
            if ((run.kind != "osc" && run.kind != "midi") || ! countsAsSent (run))
                return;

            if (dohOfDevice (document, run.kind, run.sentTo, run.cue) != dohSetting::leave)
            {
                if (run.kind == "midi"
                      && std::none_of (sentAgain.begin(), sentAgain.end(),
                                       [&run] (const SentAgain& sent) { return sent.cue == run.cue && sent.device == run.sentTo; }))
                    sentAgain.push_back ({ run.cue, run.sentTo, footerOf });

                return;
            }

            if (std::find (leftCues.begin(), leftCues.end(), run.cue) == leftCues.end())
                leftCues.push_back (run.cue);

            if (std::none_of (leftSends.begin(), leftSends.end(),
                              [&run] (const LeftSend& sent) { return sent.cue == run.cue && sent.device == run.sentTo; }))
                leftSends.push_back ({ run.cue, run.kind, run.sentTo });
        };

        std::set<std::string> consideredIds;

        /*  NOT THE PRE-SENDS OF A BLOCK HANDED BACK WHOLE (HO, D2): they are the
            horizon's again, and the corrected GO adopts them as they are. */
        for (const auto& run : runs.all())
            if ((ofTheGo.count (run.id) > 0 && handedBack.count (run.id) == 0) || run.causedBy == serial)
            {
                consider (run, {});
                consideredIds.insert (run.id);
            }

        /*  AND THE FOOTER OF AN ACT NOT BROUGHT BACK - every reached act's,
            after an Esc, which ran their footers and brings none back. */
        for (const auto& id : reached)
        {
            if (! record.escapedAfter && fates[id] != Fate::leave)
                continue;

            const auto* group = runs.find (id);

            if (group == nullptr)
                continue;

            for (const auto* below : runs.descendantsOf (id))
                if (consideredIds.count (below->id) == 0 && below->launchRequestedAtTick >= record.tick
                      && isFooterCueOf (*group, below->cue))
                {
                    consider (*below, cueLabel (group->cue));

                    for (const auto* deeper : runs.descendantsOf (below->id))
                        consider (*deeper, cueLabel (group->cue));
                }
        }

        const auto seconds = std::max (0.0, osc::parseDouble (document.getAttribute ("/godot/audio/panicFade")
                                                                .value_or ("1")).value_or (1.0));
        const auto ticks = static_cast<int> (std::lround (seconds * TickClock::rateHz));

        /*  THE PRE-SENDS THE GO COMMITTED (D3, HO): a run of the GO's the
            horizon made, holding a restore - read now, before the un-adopt
            hands a block back and before the take-down clears the restore of
            those left to their operators. Each is its address's FIRST GO
            writer: the value before the horizon is what goes back there.
            Hook-consumed fields, shaping only the flush's entries. Not a block
            handed back whole: its pre-sends are the horizon's again. */
        struct Committed
        {
            std::string address, restore, written, cue, device;
        };

        std::vector<Committed> committed;

        for (const auto& run : runs.all())
            if (ofTheGo.count (run.id) > 0 && handedBack.count (run.id) == 0 && run.kind == "osc"
                  && run.preparedAfterGo >= 0 && ! run.restoreAddress.empty())
                committed.push_back ({ run.restoreAddress, run.restoreAtom, run.preSentAtom, run.cue, run.sentTo });

        /*  HEARD, AND CARRIED ON BY THE NEXT GO (D2, namespace draft §24.12): the root of the
            standby's own chain - the cue the GO fired, or the scene it entered
            to fire it - built before anything moves. At most one; a start
            cue's target is a root of its own and starts from its top (L9). */
        std::optional<DohRoot> resumeRoot;

        if (! record.escapedAfter)
            for (const auto& rootId : roots)
            {
                const auto* root = runs.find (rootId);

                if (root == nullptr || otherHands.count (rootId) > 0 || ! heardUnder (rootId, serial)
                      || (root->cue != record.cue && ! isInside (record.cue, root->cue)))
                    continue;

                resumeRoot = rootFor (rootId, serial, tick, leftCues, ticks);
                break;
            }

        //  9. THE UN-ADOPT: what nobody heard, handed back as it was.
        for (const auto* adoption : unadopting)
            unadopt (engine, tick, *adoption, serial);

        PutBack back;

        //  7. AN ESC SINCE THE GO: Esc has stopped everything, footers and all,
        //     and a Doh never undoes an Esc. The pointer went back above; what
        //     reached a device left to its operator is still not to be sent
        //     again, since Esc unsends nothing.
        if (! record.escapedAfter)
        {
            //  10. THE TAKE-DOWN.

            /*  PASS H - THE HORIZON'S WORK, GIVEN BACK AND NEVER TAKEN DOWN,
                outermost first, what is under each going with it: a block asked
                to stop - not taken back - so its own job gives it back by H2's
                road, what it pre-sent put back first; an arm revoked here. */
            std::set<std::string> givenBack;

            const auto underGivenBack = [this, &givenBack] (const std::string& id)
            {
                for (auto at = id; ! at.empty();)
                {
                    if (givenBack.count (at) > 0)
                        return true;

                    const auto* above = runs.find (at);

                    if (above == nullptr)
                        break;

                    at = above->parent;
                }

                return false;
            };

            for (const auto& id : horizon)
            {
                if (underGivenBack (id))
                    continue;

                auto* run = runs.find (id);

                if (run == nullptr || run->isFinished())
                    continue;

                if (run->state == runState::preparing)
                {
                    run->askStop (0);
                    givenBack.insert (id);
                }
                else if (run->kind == "media" || run->kind == "mic")
                {
                    revokePrepared (engine, tick, id);
                    givenBack.insert (id);
                }
            }

            /*  WHAT IS SWEPT: the GO's runs, roots another hand is stopping
                left out, and the release of an act the Doh brings back. */
            std::vector<std::string> swept;

            /*  NOR WHAT WAS HANDED BACK, nor what the GO made under it, which the
                un-adopt has already ended (D2). */
            for (const auto& run : runs.all())
                if ((ofTheGo.count (run.id) > 0 && otherHands.count (run.id) == 0 && ! underGivenBack (run.id)
                       && ! underHandedBack (run.id))
                      || std::find (caused.begin(), caused.end(), run.id) != caused.end())
                    swept.push_back (run.id);

            /*  PASS A - WHAT NEVER SOUNDED, OR HAS NOTHING LEFT TO SOUND: a
                media or mic cue not yet heard, every network, MIDI, memo and
                start cue - whatever its device's setting, which governs only
                what had already left - and anything in a wait. A fade or a stop
                cue that has fired is left to run: its target is not the GO's. */
            for (const auto& id : swept)
            {
                const auto* run = runs.find (id);

                if (run == nullptr || run->isFinished())
                    continue;

                const auto silentSound = (run->kind == "media" || run->kind == "mic") && run->startedAtTick < 0;
                const auto sender = run->kind == "osc" || run->kind == "midi" || run->kind == "memo"
                                      || run->kind == "start";

                if (silentSound || sender || run->isWaiting())
                    endHere (id, tick);
            }

            /*  PASS B - WHAT SOUNDED, BROUGHT DOWN THE WAY ESC BRINGS IT DOWN,
                and never killed: a media cue over the panic fade, a mic's input
                shut with its tail left to ring; every group asked to stop, and
                taken back, so its job runs no footer - and gives a scene nobody
                heard back the way a preparation is given back, its pre-sends put
                back first: those the GO committed whose cue takes back - the
                ones whose cue leaves are cleared here, so nothing a device's
                operator was left with is touched - and the horizon's own. */
            for (const auto& id : swept)
            {
                auto* run = runs.find (id);

                if (run == nullptr || run->isFinished())
                    continue;

                if ((run->kind == "media" || run->kind == "mic") && run->startedAtTick >= 0)
                {
                    run->takenBack = true;
                    run->askStop (0);
                    run->launchRequested = false;

                    /*  A STOP DUE SOONER WINS, as under Esc: a job already
                        holding the voice for a stop that lands before the Doh
                        fade would is left to land, and no fade is pushed over
                        it. Should the run that started it be stopped first -
                        the scene ending its members - the job lets go of that
                        run and still lands its stop (`advanceFades`). Reads the
                        jobs, and only to shape whether one is pushed. */
                    const auto soonerStop = std::any_of (running.begin(), running.end(),
                                                         [&id, stopsBy = tick + ticks] (const FadeJob& held)
                                                         {
                                                             return held.stopWhenDone && held.heldRun() == id
                                                                      && held.stopsAtTick <= stopsBy;
                                                         });

                    if (! soonerStop)
                        panicShapedFade (id, tick, ticks, seconds);

                    continue;
                }

                /*  A PICTURE SEEN, brought down the way Esc brings it down
                    (VK): to black over the panic fade, then gone. The corrected
                    GO puts it up again from its fade-in. */
                if (run->kind == "video" && run->startedAtTick >= 0)
                {
                    run->takenBack = true;
                    run->askStop (0);
                    fadeOutVideo (id, tick, ticks);
                    continue;
                }

                if (! run->isGroup())
                    continue;

                /*  ONLY THE GO'S OWN PRE-SENDS (§24, HO): those of a block it
                    committed. The next scene's block the horizon made after
                    the GO, under this one, is the horizon's - given back by its
                    own rule whatever its device says (L34), its restore kept. */
                if (! heardUnder (id, run->causedBy != 0 ? run->causedBy : run->goSerial))
                    for (const auto* below : runs.descendantsOf (id))
                        if (below->kind == "osc" && ! below->restoreAddress.empty() && ofTheGo.count (below->id) > 0
                              && dohOfDevice (document, "osc", below->sentTo, below->cue) == dohSetting::leave)
                            if (auto* presend = runs.find (below->id))
                            {
                                presend->restoreAddress.clear();
                                presend->restoreAtom.clear();
                            }

                run = runs.find (id);
                run->takenBack = true;
                run->askStop (0);
            }

            /*  EVERY RUN OF THE GO IS TAKEN BACK, finished ones included - a
                finished member answers no question the horizon asks of a child
                any more - and every run of what it caused. */
            for (const auto& id : swept)
                if (auto* run = runs.find (id))
                    run->takenBack = true;

            //  11. THE OLDER ACTS: brought back to life, or a member lost.
            for (const auto& id : reached)
            {
                auto* group = runs.find (id);

                if (group == nullptr)
                    continue;

                if (fates[id] == Fate::surgery)
                {
                    for (auto& job : scheduled)
                    {
                        if (job.run != id || job.retired)
                            continue;

                        for (const auto& rootId : roots)
                        {
                            if (otherHands.count (rootId) > 0)
                                continue;

                            if (const auto at = std::find (job.phaseRuns.begin(), job.phaseRuns.end(), rootId);
                                at != job.phaseRuns.end())
                            {
                                const auto index = static_cast<std::size_t> (at - job.phaseRuns.begin());
                                job.phaseRuns.erase (at);

                                if (index < job.launched)
                                    --job.launched;
                            }

                            if (job.awaiting == rootId)
                                job.awaiting.clear();

                            /*  A ROOT HANDED BACK IS NOT TAKEN EITHER (D2): the
                                corrected GO's ask is taken again. */
                            if (unadoptRoots.count (rootId) > 0)
                                job.taken.erase (std::remove (job.taken.begin(), job.taken.end(), rootId),
                                                 job.taken.end());
                        }
                    }

                    continue;
                }

                if (fates[id] != Fate::revive)
                    continue;

                /*  BACK TO LIFE IN ITS MEMBERS PHASE: no header again, its
                    footer once, after the corrected GO's member. Records its own
                    job decided in this very tick - its end, a footer cue's spawn
                    - are applied and ignored (`unadoptedAt`).

                    AND WITH NO STOP ON ITS ACCOUNT (§24, HB), as every handler
                    that gives a run back leaves it: the stop the taken-back GO
                    asked of it went with that GO. Kept, the corrected GO's own
                    stop on the act was only "asked again", so the footer it set
                    off was nobody's - not that GO's to take back, nor held back
                    from a device left to its operator - and a Doh of it found
                    the act stopped by another hand. */
                group->state = runState::playing;
                group->endedAtTick = -1;
                group->unadoptedAt = tick;
                group->stopAsked = false;
                group->stopAskedBy = 0;
                group->askedAgain = false;
                group->stopEndsWait = false;
                group->postWaitBegan = -1;

                for (auto& job : scheduled)
                    if (job.run == id)
                        job.retired = true;

                const auto groupCue = document.findById (group->cue);
                auto round = group->round;

                if (round.empty())
                    round = membersOf (groupCue);

                GroupJob job;
                job.run = id;
                job.phase = groupPhase::members;
                job.phaseCues = round;

                for (const auto* child : runs.childrenOf (id))
                {
                    /*  A root handed back is a preparation again (D2), and the
                        corrected GO's ask takes it then. */
                    if (unadoptRoots.count (child->id) > 0)
                        continue;

                    job.taken.push_back (child->id);

                    const auto ofThis = child->goSerial == serial || child->causedBy == serial;

                    if (std::find (round.begin(), round.end(), child->cue) != round.end()
                          && hasLaunchEvidence (*child) && ! ofThis)
                        job.phaseRuns.push_back (child->id);
                }

                job.launched = job.phaseRuns.size();
                job.nextMember = job.phaseCues.size();
                scheduled.push_back (job);
            }

            //  13. PUT BACK WHAT THE GO CHANGED ELSEWHERE (D3, namespace draft
            //      §24.13): b. levels, speeds and DCA trims; c. the stops it
            //      issued; d-f. the flags, the banks and the takes; g. its fades
            //      and stops ended, their work undone. Identifiers drawn in
            //      capture order.
            back.tick = tick;
            back.serial = serial;
            back.list = record.list;
            back.fadeTicks = ticks;
            back.fadeSeconds = seconds;
            back.nextId = nextId;
            back.goRuns = ofTheGo;

            putBackLevels (back, changes);
            putBackStops (engine, back, changes);
            putBackFlagsBanksTakes (engine, back, changes);

            /*  g. WHAT ITS FADES AND STOPS WERE DOING IS UNDONE ABOVE, so they
                end here, taken back - those the take-down left to run, and
                those the un-adopt did (MJ). Their jobs on what the GO did not
                start were replaced in b and c; on what it did, by the pause. */
            std::vector<std::string> movers;

            for (const auto& run : runs.all())
                if ((ofTheGo.count (run.id) > 0 || run.causedBy == serial) && otherHands.count (run.id) == 0
                      && ! run.isFinished() && (run.kind == "fade" || run.kind == "transport"))
                    movers.push_back (run.id);

            for (const auto& id : movers)
            {
                /*  A STOP DUE SOONER THAN THE DOH FADE, on a voice the pause
                    gave no fade of its own (HT), stays: it lets go of its run,
                    which ends here, and lands that stop itself. */
                for (auto& job : running)
                    if (job.self == id && job.stopWhenDone && ofTheGo.count (job.heldRun()) > 0)
                    {
                        job.self.clear();
                        job.reportsSelf = false;
                    }

                endHere (id, tick);

                if (auto* run = runs.find (id))
                    run->takenBack = true;
            }
        }

        //  12. THE MARK: as the GO found it - its resume too, so a Doh of a
        //      corrected GO nobody has heard yet gives the resume back - and
        //      what this GO left with devices' operators attached to the cue it
        //      fired; and, when it was heard, the root the next GO on that cue
        //      carries on (D2). After an Esc there is no resume: Esc ended it.
        DohMark mark;

        if (record.markBefore.has_value())
            mark = *record.markBefore;

        if (! leftCues.empty())
        {
            auto& entry = mark.left[record.cue];

            for (const auto& cue : leftCues)
                if (std::find (entry.begin(), entry.end(), cue) == entry.end())
                    entry.push_back (cue);
        }

        if (record.escapedAfter)
        {
            mark.root.reset();
        }
        else if (resumeRoot.has_value())
        {
            mark.cue = record.cue;
            mark.goTick = record.tick;
            mark.dohTick = tick;

            /*  The `g` the Doh erased, as a seek may have moved it; the GO's own
                tick when sixty-four later steps had pushed it out. */
            mark.stepTick = erased.has_value() ? erased->tick : record.tick;
            mark.root = std::move (resumeRoot);

            /*  AND THE HOOK READS WHERE EACH PAUSED SOUND HAD GOT TO, on the
                next tick, off its playhead (`go.dohPlayhead`). */
            playheadsOwed.clear();

            if (mark.root->kind == DohRoot::Kind::media)
                playheadsOwed.push_back (mark.root->run);

            for (const auto& heard : mark.root->heardRuns)
                playheadsOwed.push_back (heard.first);
        }

        setMark (engine, tick, record.list, std::move (mark));

        //  13a. THE DESK (D3, namespace draft §24.13): every address the GO
        //       wrote, each pre-send it committed folded in as that address's
        //       first writer, and each marked left to its operator when the cue
        //       that wrote it LAST leaves - read here, once. Moved into the
        //       stash for the flush, which decides on the next tick against
        //       what the desk holds then. Hook-consumed, all of it.
        auto& out = stashFor (tick);
        out.list = record.list;
        out.report = true;
        out.fromDoh = true;

        auto desk = changes.desk;

        for (const auto& presend : committed)
        {
            const auto restore = osc::valuesFromAtoms (presend.restore);

            if (! restore.has_value())
                continue;

            const auto entry = std::find_if (desk.begin(), desk.end(),
                                             [&presend] (const DeskBefore& before) { return before.address == presend.address; });

            if (entry != desk.end())
            {
                entry->before = *restore;
                continue;
            }

            const auto written = osc::valuesFromAtoms (presend.written);

            if (! written.has_value())
                continue;

            DeskBefore folded;
            folded.address = presend.address;
            folded.device = presend.device;
            folded.before = *restore;
            folded.lastWritten = *written;
            folded.lastWriter = presend.cue;
            desk.push_back (folded);
        }

        for (auto& entry : desk)
        {
            entry.leftToOperator = dohOfDevice (document, "osc", entry.device, entry.lastWriter) == dohSetting::leave;
            out.desk.push_back (entry);
        }

        /*  Where this press's items begin: what was left to an operator goes
            first among them (below). */
        const auto firstItem = static_cast<std::ptrdiff_t> (out.items.size());

        if (changes.deskOverflow)
            out.items.push_back ("the GO wrote more addresses than Doh! keeps - those after the "
                                   + std::to_string (changesKept) + "th were not put back");

        //  14. THE REPORT'S OWN WORDS (D3): what the handler knows - the flush
        //      adds what the desks say on the next tick.
        auto& items = out.items;

        if (record.escapedAfter)
            items.push_back ("Esc since the GO: only the pointer and the desk went back");

        for (const auto& rootId : roots)
            if (otherHands.count (rootId) > 0)
                if (const auto* root = runs.find (rootId))
                    items.push_back (cueLabel (root->cue) + ": already being stopped - left to that stop");

        for (const auto& id : otherHandsActs)
            if (const auto* act = runs.find (id))
                items.push_back (cueLabel (act->cue) + ": being stopped by another hand - left to that stop");

        /*  AN ACT THE GO ENDED THAT CANNOT COME BACK (L27) - it loops, or the
            scene it sits in went on because it ended - and one the GO only
            finished a round of, which went on into its next. */
        for (const auto& id : endedNotRevived)
            if (const auto* act = runs.find (id))
                items.push_back (act->isFinished() || act->iterations == 1
                                   ? cueLabel (act->cue) + " ended when " + cueLabel (record.cue)
                                       + " was fired; it was not brought back - the next GO enters it again from its header"
                                   : cueLabel (act->cue) + " went on to its next round when " + cueLabel (record.cue)
                                       + " was fired; it was not brought back");

        /*  THE ROLLBACK (2026-10-03, OV-OX, namespace draft §24.5): what could
            not be taken back on a device that takes back is taken back by a
            message written ahead - the cue's own, its device's, or the
            previous command - ONE PER DEVICE, the first of the GO's cues that
            sent there, since it alone says where the device stood before the
            GO. Read here, once, from the document; sent by the flush. The
            corrected GO still sends every cue again, as `takeBack` always did.
            THE FIRST CUE DECIDES FOR ITS DEVICE even when it has nothing to
            send: a later cue's previous command is an earlier cue of the same
            GO - a state the GO made, never the one before it. */
        std::map<std::string, bool> rolledBack;

        const auto rollBack = [this, &out, &items, &rolledBack] (const std::string& kind, const std::string& cue,
                                                                 const std::string& device)
        {
            const auto key = kind + ":" + device;

            if (const auto decided = rolledBack.find (key); decided != rolledBack.end())
                return decided->second;

            rolledBack[key] = false;
            const auto text = rollbackOfDevice (document, kind, device, cue);

            if (text.empty())
                return false;

            const auto name = deviceLabel (kind == "midi" ? "port" : "mount", device);

            if (! parseRollback (kind, text).ok)
            {
                items.push_back (cueLabel (cue) + ": the rollback \"" + text + "\" is not a message - nothing sent to "
                                   + name);
                return false;
            }

            rolledBack[key] = true;
            out.rollbacks.push_back ({ kind, device, text });
            items.push_back (name + ": rolled back with " + text + " (" + cueLabel (cue)
                               + ") - the next GO sends it again");
            return true;
        };

        /*  WHAT COULD NOT BE TAKEN BACK, where the cue takes back and has no
            rollback: sent again by the next GO. What leaves is named below,
            with its device. */
        for (const auto& [cue, device] : changes.unputtable)
            if (dohOfDevice (document, "osc", device, cue) == dohSetting::takeBack)
                if (! rollBack ("osc", cue, device))
                    items.push_back (cueLabel (cue) + ": could not be taken back - the next GO sends it again");

        /*  And the MIDI a port that takes back was sent (D4, NU), by the port's
            name: the operator knows a synth by it. WHEN it goes again (D4's
            review, OO): a footer's - an act left ended, or every reached act's
            after an Esc - when that act ends again, not at the next GO; and to
            the port the cue names now, when that is another. */
        for (const auto& sent : sentAgain)
        {
            if (rollBack ("midi", sent.cue, sent.device))
                continue;

            const auto portNow = textOf (document.findById (sent.cue), "port");
            const auto elsewhere = ! portNow.empty() && portNow != sent.device
                                     ? ", to " + deviceLabel ("port", portNow) : std::string {};

            items.push_back (cueLabel (sent.cue) + ": MIDI to " + deviceLabel ("port", sent.device)
                               + " could not be taken back - "
                               + (sent.footerOf.empty() ? std::string ("the next GO sends it again")
                                                        : "sent again when " + sent.footerOf + " ends again")
                               + elsewhere);
        }

        /*  WHAT WAS LEFT TO AN OPERATOR COMES FIRST (D4's review, OL): it is what
            the operator must go and tell the other department. Composed here,
            in that order, rather than sorted by a client out of the sentence. */
        std::vector<std::string> leftFirst;

        /*  EVERY DEVICE AND CUE LEFT TO ITS OPERATOR, by device, in the order
            they were sent (the author, 2026-10-01). */
        {
            std::vector<std::pair<std::string, std::vector<std::string>>> byDevice;

            for (const auto& sent : leftSends)
            {
                const auto name = deviceLabel (sent.kind == "midi" ? "port" : "mount", sent.device);
                auto at = std::find_if (byDevice.begin(), byDevice.end(),
                                        [&name] (const auto& entry) { return entry.first == name; });

                if (at == byDevice.end())
                {
                    byDevice.emplace_back (name, std::vector<std::string> {});
                    at = std::prev (byDevice.end());
                }

                if (std::find (at->second.begin(), at->second.end(), cueLabel (sent.cue)) == at->second.end())
                    at->second.push_back (cueLabel (sent.cue));
            }

            for (const auto& [device, cues] : byDevice)
            {
                std::string list;

                for (const auto& cue : cues)
                    list += (list.empty() ? "" : ", ") + cue;

                leftFirst.push_back (device + ": " + list + " - left to its operator, not sent again");
            }
        }

        /*  THE PERSISTENT CUES THE RESTORED POINTER BROUGHT BACK INTO THE PLAN,
            which the Doh's own pass left alone (HN): in the section's plan at
            the cue the GO fired and not at the one it left. The document only. */
        if (! record.escapedAfter && ! persistentLeft.empty())
        {
            const auto planned = [this, &record] (const std::string& standby, bool ranOut)
            {
                std::set<std::string> cues;
                const auto plan = solvePersistent (document, durations, mounts, record.list, standby, ranOut);

                for (const auto& wants : plan.runs)
                    cues.insert (wants.cue);

                for (const auto& value : plan.values)
                    cues.insert (value.writer);

                return cues;
            };

            const auto before = planned (leftAt, ranOutThere);
            const auto now = planned (record.cue, record.finishedBefore);

            for (const auto& cue : now)
                if (persistentLeft.count (cue) > 0 && before.count (cue) == 0)
                {
                    const auto node = document.findById (cue);
                    const auto kind = node.hasType ("Midi") ? std::string ("midi") : std::string ("osc");
                    const auto device = sendTargetOf (document, kind, cue);

                    leftFirst.push_back ("persistent " + cueLabel (cue) + " on "
                                           + deviceLabel (kind == "midi" ? "port" : "mount",
                                                          device.empty() ? textOf (node, kind == "midi" ? "port" : "address")
                                                                         : device)
                                           + ": not re-asserted - left to its operator");
                }
        }

        items.insert (items.begin() + firstItem, leftFirst.begin(), leftFirst.end());

        for (const auto& item : back.items)
            items.push_back (item);

        //  15. AND THE RECORD IS SPENT: one press, one GO.
        lastDohTick = tick;
        lastDohList = record.list;
        takenBackSerials.insert (serial);
        leftByGo.erase (serial);
        goRecord = {};
        goChanges = {};
        lists.setDohOffer ({});

        return {};
    }

    //==============================================================================
    /*  WHAT A GO CHANGED ELSEWHERE, AND HOW DOH! PUTS IT BACK (2026-10-03, D3,
        PRD §3.32, namespace draft §24.13). The captures below run where each
        change is made, while the GO's record is open; the put-backs in
        `go.doh`'s handler, from handler state; the desk, the values and the
        report in one hook, on the tick after the press. */
    bool Runner::ofTheOpenGo (const std::string& runId) const
    {
        return goRecord.serial != 0 && goOfRun (runId) == goRecord.serial;
    }

    void Runner::noteLevelTouch (const std::string& key, const std::string& heldRun, const std::string& dca,
                                 bool movesRate, double from, double goTo, std::int64_t tick,
                                 const std::optional<FadeJob>& superseded)
    {
        /*  ONE PER KEY, the first fade's `from` and the job it took over, the
            last fade's destination: a GO whose scene fades a bed twice brings it
            back to where it stood before the first. */
        for (auto& touch : goChanges.levels)
            if (touch.key == key)
            {
                touch.goTo = goTo;
                return;
            }

        if (goChanges.levels.size() >= changesKept)
            return;

        LevelTouch touch;
        touch.key = key;
        touch.heldRun = heldRun;
        touch.dca = dca;
        touch.movesRate = movesRate;
        touch.from = from;
        touch.goTo = goTo;
        touch.atTick = tick;
        touch.superseded = superseded;
        goChanges.levels.push_back (touch);
    }

    void Runner::noteGoStop (const GoStop& stop, const std::string& targetParent)
    {
        if (goChanges.stops.size() < changesKept)
            goChanges.stops.push_back (stop);

        /*  AND THE ACT IT SITS IN IS REACHED (HE): a stop that lands on an
            older act's last sounding member ends the act, and the footer that
            sets off is the GO's - taken down with it, and the act brought back
            to life before the member is put back under it. */
        if (const auto* act = runs.find (targetParent);
            act != nullptr && act->isGroup() && ! act->isFinished() && goOfRun (act->id) != goRecord.serial
              && isManualGroup (document.findById (act->cue))
              && std::find (goRecord.touched.begin(), goRecord.touched.end(), act->id) == goRecord.touched.end())
            goRecord.touched.push_back (act->id);
    }

    void Runner::noteHardStop (const Run& target, const std::string& source, std::int64_t tick)
    {
        if (! ofTheOpenGo (source) || goOfRun (target.id) == goRecord.serial)
            return;

        noteGoStop ({ target.id, source, target.kind, tick, tick + 1, 0, target.stopAsked,
                      target.state == runState::waiting, target.dueTick, target.ownLevel,
                      target.state == runState::postWait, false },
                    target.parent);
    }

    void Runner::noteFlag (const Run& target, const std::string& source)
    {
        if (! ofTheOpenGo (source) || goOfRun (target.id) == goRecord.serial
              || goChanges.flags.size() >= changesKept)
            return;

        for (const auto& flag : goChanges.flags)
            if (flag.run == target.id)
                return;

        goChanges.flags.push_back ({ target.id, target.advanceRequested, target.advanceTo, target.stopAfter });
    }

    void Runner::noteTake (const std::string& source, const std::string& channel, TakeVerb verb, const Take& before)
    {
        if (takes == nullptr || ! ofTheOpenGo (source) || goChanges.takes.size() >= changesKept)
            return;

        const auto& after = takes->of (channel);

        /*  A PRESS THAT MOVED NOTHING asks nothing to be undone. */
        if (after.state == before.state && after.layers == before.layers)
            return;

        goChanges.takes.push_back ({ channel, verb, before.state, after.state, before.layers, after.layers });
    }

    void Runner::deskBeforeWrite (const OscJob& job, const std::string& address,
                                  std::optional<osc::Values>& held, bool& captured)
    {
        captured = false;

        if (mounts == nullptr || goRecord.serial == 0)
            return;

        /*  THE DESK'S ECHO OF THE GO'S LAST WRITE HERE, kept before this write
            forgets it (red team m9): a later cue, a pre-send of the next scene.
            Only while the tree still holds the GO's write - an echo of anybody
            else's is not the desk's answer to the GO. */
        if (const auto* now = mounts->valueOf (address))
            for (auto& entry : goChanges.desk)
                if (entry.address == address && ! entry.echo.has_value() && *now == entry.lastWritten)
                    if (const auto* first = mounts->firstObservedOf (address))
                        entry.echo = *first;

        /*  A WRITE OF THE GO'S, and never a pre-send (GY): the next scene's,
            made after the GO, is nobody's GO and fails the test above; one the
            GO committed, which asked the desk first and kept what it said, is
            folded in by the Doh from that restore - so the entry's last write
            stays the GO's own. A pre-send is known by having asked: its restore
            is on it before it writes. A cue of a block the GO adopted that was
            left for the entry - nothing pre-sent - writes when the GO fires it,
            and that is a GO write like any other. */
        if (! ofTheOpenGo (job.self))
            return;

        const auto* writer = runs.find (job.self);

        if (writer == nullptr || ! writer->restoreAddress.empty() || ! writer->prepare.empty())
            return;

        captured = true;

        if (const auto* seen = mounts->observedOf (address))
            held = *seen;
        else if (const auto* value = mounts->valueOf (address))
            held = *value;
    }

    void Runner::deskAfterWrite (const OscJob& job, const std::string& address,
                                 const std::optional<osc::Values>& held,
                                 const std::string& mountId, const osc::Values& values)
    {
        const auto* writer = runs.find (job.self);

        if (writer == nullptr || mounts == nullptr)
            return;

        const auto device = writer->sentTo.empty() ? mountId : writer->sentTo;
        const auto* node = mounts->nodeAt (address);

        /*  NOTHING TO READ BACK - an event, which has no value, or a device
            that describes nothing: named, never put back (L13). */
        if (node == nullptr || node->kind == tree::Kind::event)
        {
            const std::pair<std::string, std::string> sent { writer->cue, device };

            if (goChanges.unputtable.size() < changesKept
                  && std::find (goChanges.unputtable.begin(), goChanges.unputtable.end(), sent) == goChanges.unputtable.end())
                goChanges.unputtable.push_back (sent);

            return;
        }

        auto entry = std::find_if (goChanges.desk.begin(), goChanges.desk.end(),
                                   [&address] (const DeskBefore& before) { return before.address == address; });

        if (entry == goChanges.desk.end())
        {
            if (goChanges.desk.size() >= changesKept)
            {
                goChanges.deskOverflow = true;
                return;
            }

            /*  THE FIRST GO WRITE HERE says what was there before the GO. */
            DeskBefore made;
            made.address = address;
            made.before = held;
            goChanges.desk.push_back (made);
            entry = std::prev (goChanges.desk.end());
        }

        /*  EVERY ONE SAYS WHAT IS THERE NOW, and who put it there: the address
            follows its last writer (L33). */
        entry->lastWritten = values;
        entry->echo.reset();
        entry->lastWriter = writer->cue;
        entry->device = device;
    }

    Runner::DohStash& Runner::stashFor (std::int64_t tick)
    {
        /*  A FILL FINDING AN OLDER STAMP CLEARS IT FIRST: never flushed in a
            replay, it holds no more than one tick's worth there; two fills in
            one tick - a Doh and a jump in one drain - add up. */
        if (stash.tick != tick)
        {
            stash = {};
            stash.tick = tick;
        }

        return stash;
    }

    std::string Runner::cueLabel (const std::string& cueId) const
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return cueId;

        const auto number = cue[juce::Identifier ("number")].toString().toStdString();
        const auto name = cue[juce::Identifier ("name")].toString().toStdString();

        if (! number.empty() && ! name.empty())
            return number + " " + name;

        if (! number.empty())
            return number;

        return name.empty() ? cueId : name;
    }

    std::string Runner::deviceLabel (const std::string& kind, const std::string& deviceId) const
    {
        if (deviceId.empty())
            return "a device";

        /*  A DEVICE GONE SINCE is named by the identifier its runs kept. */
        const auto name = document.getAttribute ("/godot/" + kind + "/" + deviceId + "/name").value_or (std::string {});
        return name.empty() ? deviceId : name;
    }

    std::set<std::string> Runner::leftCuesUnder (const std::string& sceneCue) const
    {
        std::set<std::string> left;

        std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& node)
        {
            for (const auto& child : node)
            {
                const auto id = child[idProperty].toString().toStdString();

                if ((child.hasType ("Osc") || child.hasType ("Midi")) && ! id.empty()
                      && dohOf (document, id) == dohSetting::leave)
                    left.insert (id);

                walk (child);
            }
        };

        if (const auto scene = document.findById (sceneCue); scene.isValid())
            walk (scene);

        return left;
    }

    //------------------------------------------------------------------------------
    void Runner::putBackLevels (PutBack& back, const GoChanges& changes)
    {
        /*  b. LEVELS, SPEEDS AND DCA TRIMS (GS): one job per key, from where it
            is now - back to where it stood before the GO, over the panic fade,
            or, when a fade was moving it before the GO, that fade made again
            where it would be now, its stop still landing on its tick. Never
            over another writer: a key a fade the GO did not fire holds, or one
            a hand has moved off the GO's value, is left and said (L22). The
            jobs and levels are a hook's; nothing here is a record. */
        for (const auto& touch : changes.levels)
        {
            Run* held = nullptr;

            if (! touch.heldRun.empty())
            {
                /*  The pause owns what the GO started; a cue its stop ended is
                    the relaunch's, placed at the level it would have. */
                if (back.goRuns.count (touch.heldRun) > 0)
                    continue;

                held = runs.find (touch.heldRun);

                if (held == nullptr || held->isFinished())
                    continue;
            }

            if (touch.dca.empty() && held == nullptr)
                continue;

            const auto writtenBy = [this, &back] (const FadeJob& job, bool ours)
            {
                return ! job.retired && ! job.self.empty() && (goOfRun (job.self) == back.serial) == ours;
            };

            const auto goHolds = std::any_of (running.begin(), running.end(), [&] (const FadeJob& job)
                                              { return job.target == touch.key && writtenBy (job, true); });
            const auto otherHolds = std::any_of (running.begin(), running.end(), [&] (const FadeJob& job)
                                                 { return job.target == touch.key && writtenBy (job, false); });

            const auto moved = ! touch.move.empty();
            const auto now = ! touch.dca.empty() ? (dcas != nullptr ? dcas->trimOf (touch.dca) : touch.goTo)
                           : moved ? movedValueNow (*held, touch.move)
                           : touch.movesRate ? held->ownRate : held->ownLevel;

            /*  A plugin value is a 0..1 and a frequency is hertz: a tolerance
                in each one's own scale (namespace draft §26, PE). */
            const auto tolerance = moved ? (touch.moveDomain == MoveDomain::logarithm ? std::abs (touch.goTo) * 0.001
                                            : touch.moveDomain == MoveDomain::linear ? 0.001 : 0.05)
                                 : touch.movesRate ? 0.001 : 0.05;

            if (otherHolds || (! goHolds && std::abs (now - touch.goTo) > tolerance))
            {
                const auto what = ! touch.dca.empty() ? deviceLabel ("dca", touch.dca)
                                                      : cueLabel (held->cue);

                back.items.push_back (what + (moved ? ": its " + moveWords (touch.move)
                                                    : std::string (touch.movesRate ? ": its speed" : ": its level"))
                                        + " moved by another hand since the GO - left where it is");
                continue;
            }

            /*  THE GO'S OWN JOB GOES, and any stop it carried with it: a stop
                of the GO's is called off or put back below, and a stop it had
                taken over from before it comes back with that fade. */
            resolveTakeover (touch.key);

            FadeJob job;
            job.target = touch.key;
            job.dca = touch.dca;
            job.handSerial = dcas != nullptr && ! touch.dca.empty() ? dcas->handSerialOf (touch.dca) : 0;
            job.reportsSelf = false;
            job.movesRate = touch.movesRate;
            job.curve = FadeCurve::linear;

            if (moved)
            {
                job.move = touch.move;
                job.moveRun = touch.heldRun;
                job.moveDomain = touch.moveDomain;
                job.fromValue = now;
            }
            else if (touch.movesRate)
                job.fromRate = now;
            else
                job.fromDb = now;

            if (touch.superseded.has_value())
            {
                const auto& before = *touch.superseded;
                const auto remaining = std::max<std::int64_t> (0, before.ticksTotal - before.ticksDone
                                                                    - (back.tick - touch.atTick));

                job.ticksTotal = static_cast<int> (std::max<std::int64_t> (remaining, back.fadeTicks));
                job.curve = before.curve;
                job.stopWhenDone = before.stopWhenDone;
                job.stopsAtTick = before.stopsAtTick;

                if (moved)
                    job.toValue = before.toValue;
                else if (touch.movesRate)
                    job.toRate = before.toRate;
                else
                    job.toDb = before.toDb;
            }
            else
            {
                job.ticksTotal = back.fadeTicks;

                if (moved)
                    job.toValue = touch.from;
                else if (touch.movesRate)
                    job.toRate = touch.from;
                else
                    job.toDb = touch.from;
            }

            running.push_back (job);
        }
    }

    void Runner::putBackStops (Engine& engine, PutBack& back, const GoChanges& changes)
    {
        /*  c. THE STOPS IT ISSUED (GT), in the order it issued them, one answer
            per run - from handler state alone: the command tick the stop was
            counted from, whether one had been asked before it, the run's own
            records. */
        std::set<std::string> answered;

        for (const auto& stop : changes.stops)
        {
            if (! answered.insert (stop.target).second || back.goRuns.count (stop.target) > 0)
                continue;

            auto* target = runs.find (stop.target);

            /*  A STOP ALREADY COMING BEFORE THE GO lands as it was going to:
                the fade that carried it was made again above. */
            if (target == nullptr || stop.wasStopping)
                continue;

            const auto sound = target->kind == "media" || target->kind == "mic";

            /*  1. CALLED OFF: it has not landed. A sound; a scene in the GO's
                own drain, before its job has seen the stop, or whose stop is a
                FADE still holding it - its job waits for that fade before it
                touches a member (K3), so nothing of it has ended yet (D3's
                review, NY). A hard stop on a scene past the GO's own drain has
                ended its members: handed back, the scene would play its next
                member or run its footer (EK). A mic's gate opens again, where
                the GO's stop had shut it (live only, a Player call as the shut
                was). AND BACK TO THE WAIT IT WAS IN (D3's review, NZ): one in
                its post-wait, its sound over, put back to `playing` had nothing
                left to end it, and its sequence stalled. */
            if (! target->isFinished() && back.tick < stop.landsAt
                  && (sound || (target->isGroup() && (back.tick == stop.goTick || stop.fades))))
            {
                target->state = stop.wasWaiting    ? runState::waiting
                              : stop.wasPostWait   ? runState::postWait
                                                   : runState::playing;
                target->stopAsked = false;
                target->stopAskedBy = 0;
                target->askedAgain = false;

                for (auto& job : running)
                    if (job.heldRun() == stop.target && job.stopWhenDone && job.stopsAtTick == stop.jobStopsAt)
                        job.stopWhenDone = false;

                if (target->kind == "mic" && ! stop.wasWaiting && ! stop.wasPostWait && audio != nullptr
                      && target->track >= 0 && samplesPerTick > 0)
                    audio->openLive (target->track,
                                     audio->samplesElapsed() + static_cast<std::int64_t> (latencyTicks()) * samplesPerTick,
                                     back.fadeSeconds);

                continue;
            }

            if (target->killed)
                continue;

            /*  4. A SCENE STILL ENDING - its members down, its footer running or
                about to: put back once it has ended, so its release does not
                land over the relaunch. Unless a GO on its list comes first. */
            if (target->isGroup())
            {
                if (! target->isFinished())
                {
                    PendingRelaunch pending;
                    pending.list = back.list;
                    pending.groupRun = target->id;
                    pending.goTick = stop.goTick;
                    pending.dohTick = back.tick;
                    pending.levelBefore = stop.levelBefore;
                    pending.left = leftCuesUnder (target->cue);
                    pendingRelaunches.push_back (pending);

                    back.items.push_back (cueLabel (target->cue) + " comes back once it has ended, unless a GO comes first");
                    continue;
                }

                if (target->endedAtTick >= stop.goTick)
                    relaunchScene (engine, back, target->id, leftCuesUnder (target->cue), stop.levelBefore);

                continue;
            }

            /*  2 AND 3. A CUE IT STOPPED, made again where it would be now - once
                its stop has landed, ended or not (a second voice for a tick or
                two, L16). A send it stopped after it fired is nothing to put
                back: its message stood. */
            const auto landed = target->isFinished() ? target->endedAtTick >= stop.goTick
                                                     : sound && back.tick >= stop.landsAt;

            if (! landed)
                continue;

            std::optional<FadeJob> before;

            for (const auto& touch : changes.levels)
                if (touch.key == stop.target)
                    before = touch.superseded;

            relaunchStopped (engine, back, stop, before);
        }
    }

    bool Runner::relaunchStopped (Engine& engine, PutBack& back, const GoStop& stop,
                                  const std::optional<FadeJob>& before)
    {
        const auto* target = runs.find (stop.target);

        if (target == nullptr)
            return false;

        const auto cueId = target->cue;
        const auto kind = target->kind;
        const auto cue = document.findById (cueId);
        const auto label = cueLabel (cueId);

        if (! cue.isValid())
            return false;

        const auto sends = kind == "osc" || kind == "midi";
        const auto sound = kind == "media" || kind == "mic";

        if (! sends && ! sound)
            return false;

        /*  RUNNING AGAIN ALREADY - fired by name or by a trigger since (D3's
            review, OB): a second copy would play beside it. */
        if (liveOtherRunOf (cueId, stop.target) != nullptr)
        {
            back.items.push_back (label + ": running again already - not put back");
            return false;
        }

        /*  A SEND THE GO STOPPED AFTER IT HAD FIRED: its message stood (a stop
            unsends nothing), and there is nothing to put back or to say. */
        if (sends && ! stop.wasWaiting)
            return false;

        /*  WHERE IT SITS: at the top of its list, or under running manual acts
            - one the Doh has just brought back to life included. A member of a
            scene the machine paces is the scene's: an automatic sequence's next
            member, which the stop set going, plays on (L14). */
        std::vector<std::string> above;
        std::map<std::string, std::string> runFor;
        std::string act;

        for (auto at = target->parent; ! at.empty();)
        {
            const auto* group = runs.find (at);

            if (group == nullptr || group->isFinished() || group->stopAsked
                  || ! isManualGroup (document.findById (group->cue)))
            {
                back.items.push_back (label + ": stopped by the GO inside a scene - not started again");
                return false;
            }

            if (act.empty())
                act = group->id;

            runFor[group->cue] = group->id;
            above.insert (above.begin(), group->cue);
            at = group->parent;
        }

        PlannedRun wants;
        wants.cue = cueId;
        wants.ancestors = above;

        SeatResume arrival;
        arrival.arrivalTicks = std::max (back.fadeTicks, 1);
        auto arrives = false;

        if (stop.wasWaiting)
        {
            /*  STOPPED IN ITS PRE-WAIT (red team m7): nothing of it had happened.
                Still to come, it waits out the rest of that wait and arrives
                whole, at its own level - whatever a send's device says, since
                nothing of it had left. Its time passed while it was stopped: a
                sound placed as if it had started then, fading in; a send fired
                now, late - only where its cue takes back. The Doh never sends a
                device left to its operator anything late (HR). */
            if (stop.dueTick > back.tick)
            {
                wants.when = planned::due;
                wants.startsIn = static_cast<double> (stop.dueTick - back.tick) / static_cast<double> (TickClock::rateHz);
            }
            else if (sends)
            {
                if (dohOf (document, cueId) != dohSetting::takeBack)
                {
                    back.items.push_back (deviceLabel (kind == "midi" ? "port" : "mount", sendTargetOf (document, kind, cueId))
                                            + ": " + label + " - left to its operator, not sent (the GO stopped it)");
                    return false;
                }

                wants.when = planned::due;
                wants.startsIn = 0.0;
            }
            else
            {
                const auto origin = target->armedOrigin > 0.0 ? target->armedOrigin : numberOf (cue, "startOffset");
                wants.when = planned::sounding;
                wants.offset = origin + static_cast<double> (back.tick - stop.dueTick)
                                          / static_cast<double> (TickClock::rateHz) * documentSpeedOf (cue);
                arrives = true;
            }
        }
        else
        {
            /*  ASKED FOR AND NEVER STARTED: where it would be is the disk's to
                say, and nobody can (L14). */
            if (target->startedAtTick < 0)
            {
                back.items.push_back (label + ": stopped by the GO while it was still arming - not started again");
                return false;
            }

            wants.when = planned::sounding;
            arrives = true;

            if (kind == "media")
            {
                /*  WHERE IT WOULD BE NOW, in its file: from its own start at
                    its cue's speed, as K8 counts a paused bed; a slice at the
                    in-point of the one it was in (L1). Not if it would have run
                    out by now (ruling 16). */
                const auto point = countedPoint (*target, back.tick);

                if (! rangesOf (cue).empty())
                {
                    wants.range = point.range;
                    wants.offset = 0.0;
                }
                else
                {
                    wants.offset = point.from;
                }

                if (! notOver (cue, wants.range, wants.offset))
                {
                    back.items.push_back (label + ": would have ended by now - not put back");
                    return false;
                }
            }
        }

        if (arrives && kind == "media")
        {
            /*  AT THE LEVEL IT WOULD HAVE NOW: where the fade moving it before
                the GO would have it, carried on from there; else where it was. */
            auto level = stop.levelBefore;

            if (before.has_value() && ! before->movesRate && before->ticksTotal > 0)
            {
                const auto done = before->ticksDone + (back.tick - stop.goTick);
                const auto progress = std::clamp (static_cast<double> (done) / before->ticksTotal, 0.0, 1.0);

                level = before->points.empty() ? fadeLevelDb (before->fromDb, before->toDb, progress, before->curve)
                                               : fadeLevelDb (before->fromDb, before->points, progress);

                const auto rest = before->ticksTotal - done;

                if (rest > 0)
                    arrival.carryOn[cueId] = { before->toDb, static_cast<int> (rest), before->curve, {}, false };
            }

            if (! stop.wasWaiting)
                arrival.levels[cueId] = level;
        }

        makingForPutBack = true;
        seatPlan (engine, back.tick, std::vector<PlannedRun> { wants }, runFor, back.nextId,
                  arrives ? &arrival : nullptr);
        makingForPutBack = false;

        const auto found = runFor.find (cueId);

        if (found == runFor.end())
            return false;

        const auto made = found->second;

        /*  STILL TO COME: armed now and kept waiting, as an entry arms it. */
        if (wants.when == planned::due)
        {
            if (kind == "media")
                armMedia (engine, cue, made);
            else if (kind == "mic")
                armMic (engine, cue, made);
        }

        /*  UNDER ITS ACT, as a member it has launched (§24.12). */
        if (! act.empty())
            adoptIntoParentJob (act, made);

        back.items.push_back (label + ": put back where it would be now");
        return true;
    }

    bool Runner::relaunchScene (Engine& engine, PutBack& back, const std::string& groupRun,
                                const std::set<std::string>& left, double levelBefore)
    {
        const auto* scene = runs.find (groupRun);

        if (scene == nullptr)
            return false;

        const auto sceneCue = scene->cue;
        const auto group = document.findById (sceneCue);
        const auto label = cueLabel (sceneCue);

        /*  RUNNING AGAIN ALREADY - fired by name or by a trigger since (D3's
            review, OB): a second copy would play beside it. */
        if (liveOtherRunOf (sceneCue, groupRun) != nullptr)
        {
            back.items.push_back (label + ": running again already - not put back");
            return false;
        }

        /*  ONLY A SCENE THE WALK TIMES AS WRITTEN - a timeline or an automatic
            sequence that plays once, its members in order - has a second to be
            put back at (L14). */
        if (! group.isValid() || textOf (group, "mode") == "sampler" || ! seeksAsWritten (*scene, group))
        {
            back.items.push_back (label + ": stopped by the GO - not started again (only a scene that plays once"
                                          " by itself can be put back where it would be)");
            return false;
        }

        std::vector<std::string> above;
        std::map<std::string, std::string> runFor;
        std::string act;

        for (auto at = scene->parent; ! at.empty();)
        {
            const auto* up = runs.find (at);

            /*  UNDER RUNNING ACTS A PERSON RUNS, as a cue is (D3's review, OC):
                a scene a sequence or a timeline runs is that scene's, which
                went on when the stop ended it - seated again it would play
                beside the member that came next. */
            if (up == nullptr || up->isFinished() || up->stopAsked || ! isManualGroup (document.findById (up->cue)))
            {
                back.items.push_back (label + ": stopped by the GO inside a scene - not started again");
                return false;
            }

            if (act.empty())
                act = up->id;

            runFor[up->cue] = up->id;
            above.insert (above.begin(), up->cue);
            at = up->parent;
        }

        /*  THE SECOND IT WOULD HAVE REACHED NOW, had the GO not stopped it. */
        const auto listId = listOfCue (sceneCue);
        const auto elapsed = static_cast<double> (back.tick - scene->launchRequestedAtTick)
                               / static_cast<double> (TickClock::rateHz);
        const auto plan = solve (document, handlerDurations(), mounts, { listId, sceneCue, std::max (0.0, elapsed) });

        if (! plan.ok || ! plan.confused.empty())
        {
            back.items.push_back (label + ": stopped by the GO - not started again (where it would be is not known)");
            return false;
        }

        std::vector<PlannedRun> wanted;

        for (const auto& wants : plan.runs)
            if (wants.cue == sceneCue
                  || std::find (wants.ancestors.begin(), wants.ancestors.end(), sceneCue) != wants.ancestors.end())
                wanted.push_back (wants);

        const auto own = std::find_if (wanted.begin(), wanted.end(),
                                       [&sceneCue] (const PlannedRun& wants) { return wants.cue == sceneCue; });

        /*  OVER BY NOW: it would have ended, and stays ended - and says so. AND
            SO IF ONLY ITS FOOTER WOULD STILL BE RUNNING (D3's review): its
            members over, the footer already ran when the GO's stop ended it,
            and seated again it would run a second time. */
        const auto inItsFooter = [this, &sceneCue] (const std::string& cueId)
        {
            const auto holder = document.findById (cueId).getParent();
            return holder.isValid() && holder.hasType ("Footer")
                     && holder.getParent()[idProperty].toString().toStdString() == sceneCue;
        };

        const auto membersLeft = std::any_of (wanted.begin(), wanted.end(), [&] (const PlannedRun& wants)
        {
            return wants.cue != sceneCue && wants.when != planned::finished && ! inItsFooter (wants.cue);
        });

        if (own == wanted.end() || own->when == planned::finished || ! membersLeft)
        {
            back.items.push_back (label + ": would have ended by now - not put back");
            return false;
        }

        /*  THE ONE RULE (HR): a cue left to its operator that the plan places
            before this second - sent before the stop, or due while the scene
            was stopped - is planned over and named, never sent late; one after
            it is seated, and sends at its time. */
        std::vector<std::pair<std::string, std::vector<std::string>>> heldBack;
        std::vector<std::string> placedBefore;

        for (const auto& wants : wanted)
            if (wants.when == planned::finished && left.count (wants.cue) == 0
                  && document.findById (wants.cue).hasType ("Osc"))
                placedBefore.push_back (wants.cue);

        for (auto& wants : wanted)
        {
            if (left.count (wants.cue) == 0 || wants.when == planned::due)
                continue;

            wants.when = planned::finished;

            const auto node = document.findById (wants.cue);
            const auto kind = node.hasType ("Midi") ? std::string ("midi") : std::string ("osc");
            const auto device = deviceLabel (kind == "midi" ? "port" : "mount", sendTargetOf (document, kind, wants.cue));
            auto at = std::find_if (heldBack.begin(), heldBack.end(),
                                    [&device] (const auto& entry) { return entry.first == device; });

            if (at == heldBack.end())
            {
                heldBack.emplace_back (device, std::vector<std::string> {});
                at = std::prev (heldBack.end());
            }

            at->second.push_back (cueLabel (wants.cue));
        }

        /*  ITS RUNS ARE NOBODY'S GO, and its sounds fade in over the panic
            fade (FW): the scene was stopped, not paused. */
        SeatResume arrival;
        arrival.arrivalTicks = std::max (back.fadeTicks, 1);

        /*  AT THE LEVEL IT HAD WHEN THE GO STOPPED IT (D3's review): a scene a
            fade had dipped comes back dipped, as a carried scene does (HG). */
        arrival.levels[sceneCue] = levelBefore;

        makingForPutBack = true;
        seatPlan (engine, back.tick, wanted, runFor, back.nextId, &arrival);
        makingForPutBack = false;

        for (const auto& wants : wanted)
        {
            if (wants.when != planned::due)
                continue;

            const auto found = runFor.find (wants.cue);

            if (found == runFor.end())
                continue;

            const auto node = document.findById (wants.cue);

            if (node.hasType ("Media"))
                armMedia (engine, node, found->second);
            else if (node.hasType ("Mic"))
                armMic (engine, node, found->second);
        }

        if (const auto seated = runFor.find (sceneCue); seated != runFor.end() && ! act.empty())
            adoptIntoParentJob (act, seated->second);

        /*  WHAT ITS CUES HOLD AT THIS SECOND, where they take back: the value
            of each network cue the plan places before it, the last writer of
            an address winning, diffed and sent by the flush after this tick's
            give-backs - never an event, which has no value to hold. The walk's
            own values stop at the scene's row, so they are read here from the
            cues the plan placed. */
        auto& out = stashFor (back.tick);

        for (const auto& wants : placedBefore)
        {
            const auto node = document.findById (wants);
            const auto address = textOf (node, "address");
            const auto value = osc::valuesFromAtoms (textOf (node, "value"));

            /*  Every one here takes back: `left` was read at the Doh, once (L35),
                and those it names are not among them. */
            if (address.empty() || ! value.has_value())
                continue;

            if (mounts != nullptr)
                if (const auto* target = mounts->nodeAt (address); target != nullptr && target->kind == tree::Kind::event)
                    continue;

            const auto earlier = std::find_if (out.values.begin(), out.values.end(),
                                               [&address] (const auto& entry) { return entry.first == address; });

            if (earlier != out.values.end())
                earlier->second = *value;
            else
                out.values.emplace_back (address, *value);
        }

        for (const auto& [device, cues] : heldBack)
        {
            std::string list;

            for (const auto& cue : cues)
                list += (list.empty() ? "" : ", ") + cue;

            back.items.push_back (device + ": " + list + " - left to its operator, not sent");
        }

        back.items.push_back (label + ": put back where it would be now");
        return true;
    }

    void Runner::putBackFlagsBanksTakes (Engine& engine, PutBack& back, const GoChanges& changes)
    {
        /*  d. AN ADVANCE OR A BOUNDARY STOP the GO asked, as it was: withdrawn
            while its boundary is still to be placed (once placed, the hook has
            let it go already). */
        for (const auto& flag : changes.flags)
            if (auto* run = runs.find (flag.run); run != nullptr && ! run->isFinished())
            {
                run->advanceRequested = flag.advanceRequested;
                run->advanceTo = flag.advanceTo;
                run->stopAfter = flag.stopAfter;
            }

        /*  e. A BANK THE GO CLOSED opens again; one that has finished since is
            fired again, nobody's GO - its identifier drawn here. A strip a
            member of the GO's took from another bank is given back to it. */
        for (const auto& bankRun : changes.closedSamplers)
        {
            auto* bank = runs.find (bankRun);

            if (bank == nullptr)
                continue;

            if (! bank->isFinished())
            {
                bank->closing = false;
                continue;
            }

            /*  ARMED AGAIN AS IT WAS, not entered again (D3's review, OD): its
                header had run before the GO, and run again it sent a device
                left to its operator its cues a second time - so it is passed
                over, as a seat passes over the header of a scene it seats; and
                under the act it sat in, while that act runs. */
            const auto bankCue = bank->cue;
            const auto bankNode = document.findById (bankCue);
            const auto* above = runs.find (bank->parent);
            const auto parent = above != nullptr && ! above->isFinished() ? bank->parent : std::string {};
            const auto id = back.nextId();

            if (! bankNode.isValid())
                continue;

            makingForPutBack = true;
            createRun (id, bankCue, "group", parent);
            makingForPutBack = false;

            if (auto* made = runs.find (id))
                made->postWaitTicks = ticksFor (numberOf (bankNode, "postWait"));

            fireKind (engine, back.tick, bankNode, "group", id);

            for (auto& job : scheduled)
                if (job.run == id && ! job.retired)
                    job.prepared = membersOf (bankNode.getChildWithName ("Header"));

            if (! parent.empty())
                adoptIntoParentJob (parent, id);

            back.items.push_back (cueLabel (bankCue) + ": armed again");
        }

        for (const auto& [holder, strip] : changes.lostStrips)
            if (auto* group = runs.find (holder))
                group->lostStrips.erase (std::remove (group->lostStrips.begin(), group->lostStrips.end(), strip),
                                         group->lostStrips.end());

        /*  f. A TAKE PRESS, BY ITS EXACT INVERSE - newest first, and only while
            the take is still where the press left it (GU). A pass or a layer it
            closed, and a clear, cannot be undone (L17). */
        if (takes == nullptr)
            return;

        for (auto press = changes.takes.rbegin(); press != changes.takes.rend(); ++press)
        {
            const auto& now = takes->of (press->channel);
            const auto channel = samplingChannelOf (document, press->channel);
            const auto name = document.findById (press->channel)[juce::Identifier ("name")].toString().toStdString();
            const auto label = name.empty() ? press->channel : name;

            if (now.state != press->stateAfter || now.layers != press->layersAfter)
            {
                back.items.push_back (label + ": the take has moved since the GO - left as it is");
                continue;
            }

            const auto from = press->stateBefore;
            const auto to = press->stateAfter;

            if ((from == "empty" && to == "recording") || (from == "looping" && to == "overdubbing"))
            {
                takes->press (press->channel, TakeVerb::undo, channel.layers);
            }
            else if (from == "held" && to == "overdubbing")
            {
                takes->press (press->channel, TakeVerb::undo, channel.layers);
                takes->press (press->channel, TakeVerb::hold, channel.layers);
            }
            else if (from == "held" && to == "looping" && press->layersBefore == press->layersAfter)
            {
                takes->press (press->channel, TakeVerb::hold, channel.layers);
            }
            else
            {
                back.items.push_back (label + ": what the GO did to the take cannot be undone");
            }
        }
    }

    void Runner::dohRelaunch (Engine& engine, std::int64_t tick, const std::string& groupRun,
                              const std::vector<std::string>& supplied, std::vector<std::string>& drawn)
    {
        std::size_t taken = 0;

        const std::function<std::string()> nextId = [&]
        {
            auto id = taken < supplied.size() ? supplied[taken] : std::string {};
            ++taken;

            if (id.empty())
                id = ids.generate();

            drawn.push_back (id);
            return id;
        };

        /*  NO ENTRY NAMES IT ANY MORE - a GO on its list, a jump or Esc came
            first, perhaps in the very tick the hook asked: applied, nothing. */
        const auto found = std::find_if (pendingRelaunches.begin(), pendingRelaunches.end(),
                                         [&groupRun] (const PendingRelaunch& pending) { return pending.groupRun == groupRun; });

        if (found == pendingRelaunches.end())
            return;

        const auto pending = *found;
        pendingRelaunches.erase (found);

        const auto seconds = std::max (0.0, osc::parseDouble (document.getAttribute ("/godot/audio/panicFade")
                                                                .value_or ("1")).value_or (1.0));

        PutBack back;
        back.tick = tick;
        back.list = pending.list;
        back.fadeTicks = static_cast<int> (std::lround (seconds * TickClock::rateHz));
        back.fadeSeconds = seconds;
        back.nextId = nextId;

        relaunchScene (engine, back, groupRun, pending.left, pending.levelBefore);

        /*  AND IT ALWAYS SAYS WHAT BECAME OF IT (D3's review): the Doh's report
            promised it would come back, and that promise is replaced. */
        if (back.items.empty())
            if (const auto* scene = runs.find (groupRun))
                back.items.push_back (cueLabel (scene->cue) + ": not put back");

        /*  AND ITS OWN REPORT (red team C minor 4): what it held back, by
            device, said on the readout from this tick. */
        auto& out = stashFor (tick);
        out.list = pending.list;
        out.report = true;

        for (const auto& item : back.items)
            out.items.push_back (item);
    }

    void Runner::flushDohWrites (Engine& engine)
    {
        /*  A SCENE THE GO STOPPED, ENDED NOW: asked for once, and the handler
            relaunches it - the decision a record, so a replay makes the same. */
        for (const auto& pending : pendingRelaunches)
            if (const auto* scene = runs.find (pending.groupRun); scene == nullptr || scene->isFinished())
                if (relaunchAsked.insert (pending.groupRun).second)
                    engine.submit (origin::engine, "go.dohRelaunch", one (pending.groupRun));

        if (stash.tick < 0)
            return;

        const auto held = std::move (stash);
        stash = {};

        /*  WHAT A RUN FIRED IN THIS TICK WILL WRITE (D3's review, OE): the
            waits fired before this hook, their `run.fire` queued ahead of
            anything submitted here - a member a jump's seat made due on its
            first tick, a cue a relaunch seated due at once. Their write is the
            newer one, so nothing is sent here for an address one of them
            writes: sent after them, a jump's value from an earlier cue landed
            on top of the member that should have replaced it - a jump into a
            scene left the desk where the cue before the scene put it. */
        std::set<std::string> firing;

        for (const auto& run : runs.all())
            if (run.kind == "osc" && run.state == runState::waiting && run.dueTick <= currentTick)
                if (const auto address = textOf (document.findById (run.cue), "address"); ! address.empty())
                    firing.insert (address);

        /*  WHAT THE DESK WILL HOLD after this tick's give-backs: what they
            restore, else what it was seen to hold, else what Go.dot wrote. */
        const auto willHold = [this] (const std::string& address) -> std::optional<osc::Values>
        {
            if (const auto restored = restoredThisTick.find (address); restored != restoredThisTick.end())
                return restored->second;

            if (mounts == nullptr)
                return std::nullopt;

            if (const auto* seen = mounts->observedOf (address))
                return *seen;

            if (const auto* written = mounts->valueOf (address))
                return *written;

            return std::nullopt;
        };

        std::set<std::string> wanted;

        for (const auto& [address, value] : held.values)
            wanted.insert (address);

        /*  THE DESK THE GO WROTE (FX), FIRST (D3's review, OF): what a jump or
            a relaunch wants there is the newer intent, and goes after - and for
            an address it sets, nothing of the Doh's is sent at all. The GO's
            write - or the desk's own echo of it - still standing goes back to
            what was there before; anything else is another writer, left and
            said. An address left to its operator is passed over in silence:
            the handler named it with its device, and the write it holds is the
            GO's own. */
        auto items = held.items;

        /*  THE ROLLBACKS (2026-10-03, OV-OX), FIRST: each takes its device back
            to where it stood before the GO, and the desk's exact put-back of a
            value it reads back goes on top. An OSC one as an engine `node.set`
            - logged, so a replay's tree reaches the same value, and sent only
            where the device's `tx` is on, by the door every engine write takes;
            a MIDI one straight onto the port, as nothing in the tree holds a
            MIDI message - a port switched off sends nothing, as a cue on it
            would not. */
        for (const auto& rollback : held.rollbacks)
        {
            const auto message = parseRollback (rollback.kind, rollback.text);

            if (! message.ok)
                continue;

            if (rollback.kind == "osc")
                engine.submit (origin::engine, "node.set", addressAnd (message.address, message.value));
            else if (midiOut != nullptr
                       && document.getAttribute ("/godot/port/" + rollback.device + "/tx").value_or (std::string {}) != "false")
                if (const auto problem = midiOut->send (rollback.device, message.bytes); ! problem.empty())
                    items.push_back (deviceLabel ("port", rollback.device) + ": the rollback could not be sent - " + problem);
        }

        if (mounts != nullptr)
            for (auto entry : held.desk)
            {
                if (entry.leftToOperator || wanted.count (entry.address) > 0 || firing.count (entry.address) > 0)
                    continue;

                /*  A GO SINCE THE PRESS - in its own drain - has written the
                    address already: its write is newer than anything the Doh
                    remembers, and is not put back over. */
                if (std::any_of (goChanges.desk.begin(), goChanges.desk.end(),
                                 [&entry] (const DeskBefore& newer) { return newer.address == entry.address; }))
                    continue;

                const auto* holds = mounts->valueOf (entry.address);

                if (! entry.echo.has_value() && holds != nullptr && *holds == entry.lastWritten)
                    if (const auto* first = mounts->firstObservedOf (entry.address))
                        entry.echo = *first;

                const auto now = willHold (entry.address);

                /*  ALREADY THERE - a give-back of this tick puts it there. */
                if (entry.before.has_value() && now.has_value() && *now == *entry.before)
                    continue;

                const auto theGos = now.has_value()
                                      && (*now == entry.lastWritten || (entry.echo.has_value() && *now == *entry.echo));

                if (! theGos)
                    items.push_back (entry.address + ": changed since the GO - left as it is");
                else if (! entry.before.has_value())
                    items.push_back (entry.address + ": what it held before the GO is not known - left as it is");
                else
                    engine.submit (origin::engine, "node.set", addressAnd (entry.address, *entry.before));
            }

        /*  THE VALUES A JUMP OR A RELAUNCH WANTS ON THE DESK: a minimal
            correction (§3.13), against what the desk will hold once this
            tick's give-backs have landed - a value equal to what the desk holds
            now, which a give-back is about to restore over, still goes. */
        for (const auto& [address, value] : held.values)
        {
            if (firing.count (address) > 0)
                continue;

            if (const auto now = willHold (address); now.has_value() && *now == value)
                continue;

            engine.submit (origin::engine, "node.set", addressAnd (address, value));
        }

        /*  THE REPORT, ONE RECORD. A Doh's replaces the readout - when it has
            nothing to say, with nothing (D4's review, OJ): an outage's pending
            sentence, or the report before it, must not stand for this press. A
            relaunch's alone is appended to what stands (OK): the Doh's news is
            still somebody's to read. */
        if (! held.report || (items.empty() && ! held.fromDoh))
            return;

        std::string sentence;

        for (const auto& item : items)
            sentence += (sentence.empty() ? "" : "; ") + item;

        std::vector<osc::Value> args { osc::Value::string (held.list), osc::Value::string (sentence) };

        if (! held.fromDoh)
            args.push_back (osc::Value::boolean (true));

        engine.submit (origin::engine, "list.dohReport", std::move (args));
    }

    std::string Runner::pressStrip (Engine& engine, std::int64_t tick, const std::string& stripId,
                                    int velocity, const std::string& origin)
    {
        const auto strip = document.findById (stripId);

        if (! strip.isValid() || strip.getType().toString() != "Strip")
            return reason::unknownId;

        /*  A DCA STRIP IS NOT PRESSED: it rides a trim and holds no clip. */
        if (document.getAttribute ("/godot/slot/" + stripId + "/role").value_or (std::string {})
              != "sampler")
            return reason::badValue;

        /*  NOTHING ON IT IS APPLIED AND DOES NOTHING: a pad hit between banks,
            or while the member on it waits, is not a mistake. */
        const auto* holding = runs.holderOf (stripId);

        if (holding == nullptr)
            return {};

        auto* run = runs.find (holding->id);

        if (run == nullptr || run->isFinished() || ! run->sampler)
            return {};

        /*  A CLOSING GROUP LAUNCHES NOTHING NEW, and nor does a group on a strip
            another group has taken: the clip it holds may finish, and that is
            all. */
        if (const auto* group = runs.find (run->parent);
            group != nullptr
              && (group->closing
                    || std::find (group->lostStrips.begin(), group->lostStrips.end(), stripId)
                         != group->lostStrips.end()))
            return {};

        /*  THE HAND THAT HOLDS IT OWNS IT (PRD §3.27): a press from anywhere
            else is a no-op while a hold clip is down. */
        if (run->held && run->heldBy != origin)
            return {};

        /*  A SOLO IN THE BANK HOLDS THE OTHERS (author, 2026-09-25: "The solo
            switch could be engaged on a track to prevent other faders in the
            bank to trigger"): while a clip of this bank is soloed, a press on
            another of its strips is applied and starts nothing - a pad, a fire
            by name, and the fader's own touch, whose edge is spent by it, so a
            finger resting there starts nothing when the solo lets go either. */
        if (! run->solo && bankSoloed (run->parent))
            return {};

        const auto cue = document.findById (run->cue);

        if (! cue.isValid())
            return {};

        const auto hold = textOf (cue, "release") == "hold";
        const auto byVelocity = textOf (cue, "velocity") == "true" && velocity > 0;
        const auto floor = numberOf (cue, "velocityFloor");

        const auto step = [this, &run, tick]
        {
            /*  A PRESS IS A STEP, letter `p` (plan decision 9): the live
                recorder keeps it, so a take replays what the hands played;
                load-to-time skips it, because a press is not a state the show
                can be solved back into. */
            if (const auto listId = listOfCue (run->cue); ! listId.empty())
                lists.stepped (listId, { tick, run->cue, 'p' });
        };

        if (run->state == runState::armed && ! run->launchRequested)
        {
            /*  THE PRESS THAT STARTS IT, at the level the strip's fader is at -
                the member's `initialLevel`, where it flew when the member was
                armed, unless a hand has moved it since. Velocity sets the level
                instead when the clip asks for that (decision AA).

                A FADER SOMEBODY PULLED TO THE BOTTOM is the one exception, for
                a press that is not the touch - a pad, an encoder, a fired
                command: it plays at the member's initial level, or at unity if
                that is the bottom too, because a press is a request to hear it.
                The touch itself keeps the trim wherever it is: the hand on the
                fader is the one setting the level, and the motor cannot move
                under it. The engine's own press is that touch (`samplerEdges`),
                and its origin is logged, so a replay decides the same. */
            if (byVelocity)
                run->trim = levelForByte (velocity, floor);
            else if (origin != origin::engine)
                liftParkedFader (*run, cue);

            run->launchRequested = true;
            run->launchRequestedAtTick = tick;
            run->prepare.clear();

            if (hold)
            {
                run->held = true;
                run->heldBy = origin;
            }

            notePlayed (*run);
            step();
            return {};
        }

        /*  PRESSED AGAIN WHILE IT PLAYS: a hold clip cannot be, by the hand
            holding it (§3.8's table); a play-out clip does what its
            `secondPress` says. */
        if (hold)
            return {};

        const auto second = textOf (cue, "secondPress");

        if (second == "noop")
            return {};

        if (second == "stop")
        {
            notePlayed (*run);
            beginReleaseFade (run->id, numberOf (cue, "releaseFade"), tick);
            return {};
        }

        if (byVelocity)
            run->trim = levelForByte (velocity, floor);

        /*  FROM THE TOP, AND ITS FIRST PASS: a restart, not a scrub. */
        notePlayed (*run);
        seekMedia (engine, tick, run->id, 0.0, false);
        step();
        return {};
    }

    void Runner::liftParkedFader (Run& run, const juce::ValueTree& cue) const
    {
        if (run.trim > FaderEdge::parkedDb)
            return;

        const auto initial = numberOf (cue, "initialLevel");
        run.trim = initial > FaderEdge::parkedDb ? initial : 0.0;
    }

    void Runner::notePlayed (const Run& member)
    {
        /*  A HAND ON A PAD OF THE BANK THE LAST GO ARMED (2026-10-01, namespace
            draft §24, GI): that GO is being played, and taking it back would cut
            the performer's clip - so Doh! refuses it with its sentence. Only
            where the press does something; a press on a bank the GO did not arm
            is not counted. */
        if (goRecord.serial != 0 && member.goSerial == goRecord.serial)
        {
            goRecord.firedAfter = true;
            lists.setDohOffer ({});
        }
    }

    bool Runner::bankSoloed (const std::string& groupRunId) const
    {
        if (groupRunId.empty())
            return false;

        for (const auto& member : runs.all())
            if (member.parent == groupRunId && member.solo && member.soloCanHold())
                return true;

        return false;
    }

    void Runner::releaseSolos()
    {
        /*  A SOLO NEVER OUTLIVES ITS CLIP: at its end, a stop, a kill or a
            release, the flag goes - and with it the lock on the bank and the
            light on the button. */
        for (const auto& snapshot : runs.all())
            if (snapshot.solo && ! snapshot.soloCanHold())
                if (auto* run = runs.find (snapshot.id))
                    run->solo = false;
    }

    std::string Runner::releaseStrip (Engine&, std::int64_t tick, const std::string& stripId,
                                      const std::string& origin)
    {
        const auto strip = document.findById (stripId);

        if (! strip.isValid() || strip.getType().toString() != "Strip")
            return reason::unknownId;

        const auto* holding = runs.holderOf (stripId);

        if (holding == nullptr)
            return {};

        auto* run = runs.find (holding->id);

        if (run == nullptr || run->isFinished() || ! run->sampler)
            return {};

        /*  ONLY THE HAND THAT PRESSED IT LETS GO OF IT (PRD §3.27). */
        if (run->held && run->heldBy != origin)
            return {};

        run->held = false;
        run->heldBy.clear();

        const auto cue = document.findById (run->cue);

        if (! cue.isValid() || textOf (cue, "release") != "hold")
            return {};

        /*  LET GO BEFORE IT SOUNDED: the launch had been asked for and not yet
            placed, so it is simply not placed - the member stays armed for the
            next press, and nothing was heard to stop. */
        if (run->state == runState::armed && run->launchRequested)
        {
            run->launchRequested = false;
            return {};
        }

        if (run->state == runState::armed)
            return {};

        beginReleaseFade (run->id, numberOf (cue, "releaseFade"), tick);
        return {};
    }

    void Runner::armAgain (Engine& engine, const std::string& runId)
    {
        auto* run = runs.find (runId);

        if (run == nullptr || run->isFinished() || run->track >= 0)
            return;

        run->pending.erase (std::remove (run->pending.begin(), run->pending.end(), std::string ("voice")),
                            run->pending.end());

        const auto cue = document.findById (run->cue);

        /*  A MIC CUE WHOSE CHANNEL CAME FREE (Phase 9b) takes it through the
            same door a sampler member takes a voice by. */
        if (cue.isValid() && cue.hasType ("Mic"))
            armMic (engine, cue, runId);
        else if (cue.isValid())
            armMedia (engine, cue, runId);
    }

    bool Runner::isSamplerMember (const std::string& cueId) const
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid() || kindOfCue (cue) != "media")
            return false;

        const auto parent = cue.getParent();

        return parent.isValid() && parent.getType().toString() == "Group"
                 && textOf (parent, "mode") == "sampler";
    }

    std::string Runner::pressMember (Engine& engine, std::int64_t tick, const std::string& cueId,
                                     const std::string& origin)
    {
        /*  THE STRIP THE MEMBER HOLDS, through its live run. A member waiting
            for its strip holds none yet, and a member of a group nobody armed
            has no run at all: both are `needs-strip`. */
        for (const auto& candidate : runs.all())
        {
            if (candidate.cue != cueId || candidate.isFinished() || ! candidate.sampler
                 || candidate.strip.empty())
                continue;

            if (std::find (candidate.claims.begin(), candidate.claims.end(), candidate.strip)
                  == candidate.claims.end())
                continue;

            return pressStrip (engine, tick, candidate.strip, -1, origin);
        }

        return reason::needsStrip;
    }

    std::vector<CurveTarget> Runner::curveTargetsOf (const juce::ValueTree& cue) const
    {
        std::vector<CurveTarget> targets;

        /*  Each message read at its own address - the cue's at /godot/cue, a
            further one's at /godot/message - with the curves under it, each
            judged as the door judges it so a curve the show could not have
            saved is no curve here either. */
        const auto add = [this, &targets] (const juce::ValueTree& message, const std::string& base)
        {
            CurveTarget target;
            target.address = document.getAttribute (base + "address").value_or (std::string {});
            target.base = osc::valuesFromAtoms (document.getAttribute (base + "value").value_or (std::string {}))
                            .value_or (osc::Values {});

            for (const auto& child : message)
            {
                if (! child.hasType ("Curve"))
                    continue;

                const auto curveBase = "/godot/curve/" + child[idProperty].toString().toStdString() + "/";
                const auto arg = static_cast<int> (child[juce::Identifier ("arg")]);

                if (arg < 0)
                    continue;

                std::string problem;
                const auto range = doc::readLaneRange (document.getAttribute (curveBase + "range")
                                                         .value_or (std::string {}), problem);

                CurveLane lane;
                lane.id = child[idProperty].toString().toStdString();
                lane.arg = static_cast<std::size_t> (arg);
                lane.points = doc::readLane (document.getAttribute (curveBase + "points").value_or (std::string {}),
                                             range).points;
                target.lanes.push_back (std::move (lane));
            }

            targets.push_back (std::move (target));
        };

        add (cue, "/godot/cue/" + cue[idProperty].toString().toStdString() + "/");

        for (const auto& child : cue)
            if (child.hasType ("Message"))
                add (child, "/godot/message/" + child[idProperty].toString().toStdString() + "/");

        return targets;
    }

    double Runner::curveDurationOf (const juce::ValueTree& cue, const std::vector<CurveTarget>& targets) const
    {
        const auto written = numberOf (cue, "duration");
        return written > 0.0 ? written : lastPointOf (targets);
    }

    bool Runner::writeCurve (CurveJob& job, CurveTarget& target, const osc::Values& values,
                             std::int64_t tick, std::string& why)
    {
        target.last = values;

        if (mounts == nullptr)
            return true;

        /*  THE GO'S WRITE, kept for Doh! as any other (§24.13): the proxy job
            carries the run, which is all the capture reads of it. */
        OscJob proxy;
        proxy.self = job.self;

        std::optional<osc::Values> held;
        auto captured = false;
        deskBeforeWrite (proxy, target.address, held, captured);

        const auto written = mounts->write (target.address, values);

        if (! written.ok)
        {
            why = written.reason;
            return false;
        }

        if (captured)
            deskAfterWrite (proxy, target.address, held, written.mountId, written.values);

        target.written = written.values;

        /*  ONTO THE WIRE where the device is spoken to, in the run's name, so a
            double Esc drops what is still queued; `tx` off, the tree alone. */
        if (const auto* declaration = mounts->declarationOf (written.mountId);
            declaration != nullptr && declaration->tx && sender_ != nullptr)
        {
            target.ticket = sender_->queue (written.mountId,
                                            { declaration->host, declaration->port,
                                              declaration->rateCap, declaration->bundles },
                                            target.address, written.values, job.self);
            target.ticketTick = tick;
        }

        return true;
    }

    bool Runner::isCurving (const std::string& runId) const
    {
        return std::any_of (curving.begin(), curving.end(),
                            [&runId] (const CurveJob& job) { return job.self == runId && ! job.finished; });
    }

    void Runner::seekCurves (const std::string& runId, double seconds, std::int64_t tick)
    {
        for (auto& job : curving)
        {
            if (job.self != runId || job.finished)
                continue;

            /*  WITHIN THE DURATION: a seek past the end of a cue that does not
                loop lands on its last second, which the next tick ends on. */
            job.originTick = tick;
            job.originSeconds = job.loop || ! (job.duration > 0.0) ? seconds
                                                                   : std::min (seconds, job.duration);
        }
    }

    void Runner::fireOsc (const juce::ValueTree& cue, const std::string& runId, std::int64_t tick)
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

        /*  WHAT IT SENDS HAD ALREADY REACHED A DEVICE LEFT TO ITS OPERATOR, under
            a GO that Doh! took back (2026-10-01, namespace draft §24): nothing is
            written and nothing is queued - the device holds what the early GO
            sent, or what its operator has made of it since - and the run ends
            `left-to-operator` on its next tick, with a first GO's timing. */
        if (selfRun->sendsLeft)
        {
            job.left = true;
            sending.push_back (job);
            return;
        }

        const auto address = textOf (cue, "address");
        const auto atom = textOf (cue, "value");

        /*  THE VALUE IS SPELLED THE WAY THE LOG SPELLS ONE, and reusing that
            grammar is worth more than the four lines it saves. A document, a
            log record and a value on the wire then say the same thing the same
            way - so a cue can be written by copying the atom out of a log of
            the night somebody got it right by hand.

            A LIST OF THEM (namespace draft §45): every argument of the
            message, `i:3 f:0.5`, one for a node of one, none for a message
            that carries nothing - `/go` to a great many desks. */
        const auto value = osc::valuesFromAtoms (atom);

        if (! value.has_value())
        {
            job.failure = reason::typeMismatch;
            sending.push_back (job);
            return;
        }

        /*  AND ITS FURTHER MESSAGES (namespace draft 45, YP), in their order:
            one whose values do not spell fails the cue as the first's would. */
        for (const auto& child : cue)
        {
            if (! child.hasType ("Message"))
                continue;

            /*  READ AT ITS OWN ADDRESS, /godot/message/<id> - `textOf` reads a
                cue's, and a message is not one. */
            const auto base = "/godot/message/" + child[idProperty].toString().toStdString() + "/";

            OscJob::Further next;
            next.address = document.getAttribute (base + "address").value_or (std::string {});

            const auto values = osc::valuesFromAtoms (document.getAttribute (base + "value")
                                                        .value_or (std::string {}));

            if (! values.has_value())
            {
                job.failure = reason::typeMismatch;
                sending.push_back (job);
                return;
            }

            next.pending = *values;
            job.further.push_back (std::move (next));
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

        /*  ONE DEVICE PER CUE (namespace draft 45, YV, the author's pick): a
            further message under another device than the first's fails the
            run before anything is written - asked here, at GO, because the
            door cannot (see `oscError::severalDevices`). */
        if (! job.further.empty())
        {
            const auto device = mounts->mountOf (address);

            for (const auto& next : job.further)
                if (mounts->mountOf (next.address) != device)
                {
                    job.failure = oscError::severalDevices;
                    sending.push_back (job);
                    return;
                }
        }

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
        /*  A CUE WITH CURVES (namespace draft 45, O.4) writes at GO what its
            curves say at the clock's nought, then plays them from there: a
            CurveJob takes the run, and this job says nothing more. Never
            prepared ahead (ZH), as a cue of several messages is not. */
        auto targets = curveTargetsOf (cue);

        /*  AND A CUE A PASS IS RECORDING (O.9) plays its clock whatever its
            curves hold, an empty one included: the pass writes onto it. */
        const auto recordingThis = curveTable != nullptr && curveTable->recording
                                     && curveTable->cue() == cue[idProperty].toString().toStdString();
        const auto curved = hasCurves (targets) || recordingThis;

        if (curved)
        {
            job.pending = valuesAt (targets.front(), 0.0);

            for (std::size_t n = 0; n < job.further.size() && n + 1 < targets.size(); ++n)
                job.further[n].pending = valuesAt (targets[n + 1], 0.0);
        }

        if (selfRun->prepare.empty() || ! job.further.empty() || curved)
        {
            writeOscNow (job);

            if (curved && job.failure.empty() && ! job.left)
            {
                CurveJob curves;
                curves.self = self;
                curves.cue = cue[idProperty].toString().toStdString();
                curves.originTick = tick;
                curves.duration = curveDurationOf (cue, targets);
                curves.loop = textOf (cue, "loop") == "true";
                curves.revision = document.revision();
                curves.targets = std::move (targets);

                /*  WHAT GO WROTE, so the first tick sends only what has moved
                    since - and its tickets, so a cue whose curves end at once
                    still waits for them. */
                const auto wrote = [this, tick] (CurveTarget& target, const osc::Values& pending,
                                                 std::uint64_t ticket)
                {
                    target.last = pending;
                    const auto* held = mounts->valueOf (target.address);
                    target.written = held != nullptr ? *held : pending;
                    target.ticket = ticket;
                    target.ticketTick = tick;
                };

                wrote (curves.targets.front(), job.pending, job.ticket);

                for (std::size_t n = 0; n < job.further.size() && n + 1 < curves.targets.size(); ++n)
                    wrote (curves.targets[n + 1], job.further[n].pending, job.further[n].ticket);

                curving.erase (std::remove_if (curving.begin(), curving.end(),
                                               [] (const CurveJob& done) { return done.finished; }),
                               curving.end());
                curving.push_back (std::move (curves));
                job.curved = true;
            }

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

        /*  Nor the write a read-back held for one (§24): sent nothing, the same. */
        if (const auto* writer = runs.find (job.self); writer != nullptr && writer->sendsLeft)
        {
            job.left = true;
            return;
        }

        /*  WHAT DOH! WOULD PUT BACK (2026-10-03, D3, namespace draft §24.13):
            the desk's echo of the GO's last write here, kept before this write
            forgets it, and - for a write of the GO's own - what the address
            held before it. Hook-consumed, both: they shape the flush's
            put-back and nothing a handler decides. */
        std::optional<osc::Values> held;
        auto captured = false;
        deskBeforeWrite (job, job.address, held, captured);

        const auto written = mounts->write (job.address, job.pending);

        if (! written.ok)
        {
            job.failure = written.reason;
            return;
        }

        if (captured)
            deskAfterWrite (job, job.address, held, written.mountId, written.values);

        /*  AND THE CUE'S FURTHER MESSAGES, after its own and in their order
            (namespace draft 45), each into the tree as the first went - its
            put-back kept for Doh! the same way - before anything is queued, so
            a device switched off still holds every value the cue asked for.
            A refusal stops there and fails the run naming it; what was written
            before it is still sent below, so the tree and the wire agree on
            every value that landed. */
        std::vector<osc::Values> furtherWritten;

        for (auto& next : job.further)
        {
            std::optional<osc::Values> heldNext;
            auto capturedNext = false;
            deskBeforeWrite (job, next.address, heldNext, capturedNext);

            const auto writtenNext = mounts->write (next.address, next.pending);

            if (! writtenNext.ok)
            {
                job.failure = writtenNext.reason;
                break;
            }

            if (capturedNext)
                deskAfterWrite (job, next.address, heldNext, writtenNext.mountId, writtenNext.values);

            furtherWritten.push_back (writtenNext.values);
        }

        /*  IT REACHED THE TREE; NOW IT REACHES THE WIRE. The two are separate
            on purpose: the tree is what a client reads back and what a replay
            reproduces, and the socket is what the other box hears. A cue that
            updated one and not the other would be a lie in whichever direction
            somebody happened to look. */
        const auto* declaration = mounts->declarationOf (written.mountId);

        /*  UNLESS THE DEVICE IS SWITCHED OFF, which is the one case where
            those two are meant to disagree.

            `tx` off says: this box is not in the room tonight. The write still
            lands in the tree and in the log, so the show runs, a replay
            reproduces it and a client sees the value the cue asked for - and
            nothing goes on the wire. The run ends DONE carrying `not-sent`
            (see `runWarning::notSent`), because the operator turned it off on
            purpose and a column of red would teach them to stop reading the
            colour that matters.

            Before the verify is set up, deliberately: a cue whose wait is
            `verified` against a device nobody is talking to would sit asking
            until it timed out, which is a failure produced by a setting rather
            than by anything in the rig. */
        if (declaration != nullptr && ! declaration->tx)
        {
            job.notSent = true;
            return;
        }

        /*  Queued in the run's name, which is how a double Esc knows a
            pre-send of the standby it spares from what it drops (§23.10). */
        if (sender_ != nullptr && declaration != nullptr)
        {
            job.ticket = sender_->queue (written.mountId,
                                         { declaration->host, declaration->port,
                                           declaration->rateCap, declaration->bundles },
                                         job.address, written.values, job.self);

            /*  In their order after the first, the order the queue keeps -
                one bundle where the device takes them (namespace draft 45). */
            for (std::size_t n = 0; n < furtherWritten.size(); ++n)
                job.further[n].ticket = sender_->queue (written.mountId,
                                                        { declaration->host, declaration->port,
                                                          declaration->rateCap, declaration->bundles },
                                                        job.further[n].address, furtherWritten[n], job.self);
        }

        if (job.wait == OscWait::verified)
        {
            job.mountId = written.mountId;
            job.expected = written.values;

            /*  EVERY ADDRESS IS ASKED, each forgotten first for the reason the
                first is below (namespace draft 45). */
            for (std::size_t n = 0; n < furtherWritten.size(); ++n)
            {
                auto& next = job.further[n];
                next.expected = furtherWritten[n];
                mounts->forgetReadback (next.address);

                if (const auto* node = mounts->nodeAt (next.address))
                    next.typeTag = node->typeTags;
            }

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
        spec.data1 = static_cast<int> (numberOf (cue, "data1"));
        spec.data2 = static_cast<int> (numberOf (cue, "data2"));
        spec.sysex = textOf (cue, "sysex");

        const auto built = midi::messageFor (spec);

        if (! built.ok())
        {
            job.failure = runError::badMessage;
            sending.push_back (job);
            return;
        }

        /*  LEFT TO WHATEVER PLAYS ON THE PORT by a Doh of the GO that sent it
            first (2026-10-01, namespace draft §24): nothing reaches the cable. */
        if (selfRun->sendsLeft)
        {
            job.left = true;
            sending.push_back (job);
            return;
        }

        /*  A PORT SWITCHED OFF SENDS NOTHING (2026-10-01, J3, namespace draft
            §23.7), which its row has promised all along: "Off, the cue still
            runs and finishes carrying the warning not-sent, exactly as a
            network device's tx does". A network device kept that promise in
            `writeOscNow`; a MIDI port did not, and a cue on a bound port with
            its `tx` off went out on the cable.

            READ FROM THE DOCUMENT, as the surface bridge reads it - anything but
            `false` is on, so one port reads one way everywhere - and after the
            message is built, as a network cue's failed write comes before its
            `tx` test: what is wrong with the cue is said whatever the switch
            says. The job ends the run `done` carrying `not-sent` on the next
            tick, `advanceSends`' branch for the network device's case. */
        const auto portId = textOf (cue, "port");

        if (document.getAttribute ("/godot/port/" + portId + "/tx").value_or (std::string {}) == "false")
        {
            job.notSent = true;
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

        /*  AS A CUE'S MESSAGE, carrying its run (2026-10-02, H4, namespace draft
            §23.10): the kind a double Esc drops while it is still queued, and
            whose notes the sender remembers once they have left. */
        const auto problem = midiOut->sendForRun (self, portId, built.bytes);

        if (! problem.empty())
            job.failure = problem;

        sending.push_back (job);
    }

    void Runner::advanceCurves (Engine& engine, std::int64_t tick)
    {
        for (auto& job : curving)
        {
            if (job.finished)
                continue;

            auto* run = runs.find (job.self);

            /*  OVER BY ANOTHER ROAD - revoked, taken back by Doh!, ended by a
                jump: nothing more is written. */
            if (run == nullptr || run->isFinished())
            {
                job.finished = true;
                continue;
            }

            /*  ESC, AND A DOUBLE ESC (CLAUDE.md §4.4): the curves stop where
                they are - the values hold, nothing is put back - and the run
                ends. A double Esc has already dropped what was still queued. */
            if (run->state == runState::stopping || beingKilled (*run))
            {
                engine.submit (origin::engine, "run.ended", one (job.self));
                job.finished = true;
                continue;
            }

            /*  AN EDIT REACHES A CUE THAT IS PLAYING, on the clock it is on:
                its curves read again, what was written kept. */
            if (job.revision != document.revision())
            {
                const auto cue = document.findById (job.cue);

                if (! cue.isValid())
                {
                    engine.submit (origin::engine, "run.ended", one (job.self));
                    job.finished = true;
                    continue;
                }

                auto fresh = curveTargetsOf (cue);

                for (std::size_t n = 0; n < fresh.size() && n < job.targets.size(); ++n)
                {
                    fresh[n].last = job.targets[n].last;
                    fresh[n].written = job.targets[n].written;
                    fresh[n].ticket = job.targets[n].ticket;
                    fresh[n].ticketTick = job.targets[n].ticketTick;
                }

                job.targets = std::move (fresh);
                job.duration = curveDurationOf (cue, job.targets);
                job.loop = textOf (cue, "loop") == "true";
                job.revision = document.revision();
            }

            /*  A PASS WITH NO DURATION TO END IT RUNS UNTIL IT IS STOPPED (O.9):
                a curve recorded from nothing has no last point yet. */
            const auto recordingNow = curveTable != nullptr && curveTable->recording && curveTable->run == job.self;
            auto place = placeOnCurves (job.secondsAt (tick), job.duration, job.loop);

            if (recordingNow && ! (job.duration > 0.0) && ! job.loop)
                place = { std::max (0.0, job.secondsAt (tick)), false, 1 };

            run->position = place.seconds;

            if (job.loop)
                run->iteration = place.iteration;

            /*  SENT WHEN IT CHANGED (YX): a message whose values a curve moves,
                written where they differ from what was last written - its rate
                cap, its `tx` and its bundle as any write to its device. */
            for (auto& target : job.targets)
            {
                if (target.lanes.empty())
                    continue;

                auto values = valuesAt (target, place.seconds);

                /*  WHO MOVES AN ARMED CURVE (ZJ): the device it is reporting -
                    and then nothing of its message is sent, the device being
                    the one moving it - or a hand, whose value is sent, so the
                    device follows. */
                auto deviceSays = false;

                if (recordingNow)
                    for (const auto& lane : target.lanes)
                    {
                        if (! curveTable->isArmed (lane.id))
                            continue;

                        const auto found = curveTable->rides.find (lane.id);

                        if (found == curveTable->rides.end() || ! found->second.latched)
                            continue;

                        if (found->second.source == "heard")
                            deviceSays = true;
                        else if (lane.arg < values.size() && values[lane.arg].isNumber())
                            values[lane.arg] = values[lane.arg].isInt32() ? osc::Value::int32 (static_cast<std::int32_t> (std::lround (found->second.value)))
                                             : values[lane.arg].isInt64() ? osc::Value::int64 (static_cast<std::int64_t> (std::llround (found->second.value)))
                                             : values[lane.arg].isFloat64() ? osc::Value::float64 (found->second.value)
                                                                            : osc::Value::float32 (static_cast<float> (found->second.value));
                    }

                if (deviceSays || values == target.last)
                    continue;

                std::string why;

                if (! writeCurve (job, target, values, tick, why))
                {
                    engine.submit (origin::engine, "run.failed",
                                   { osc::Value::string (job.self), osc::Value::string (why) });
                    job.finished = true;
                    break;
                }
            }

            if (job.finished || ! place.ended)
                continue;

            /*  THE END OF THE DURATION: the last values are written above, and
                the run is done by its wait - handed to `advanceSends` as a job
                already sent, with this tick's tickets for `sent` (a value that
                left earlier has left) and every message's last values for
                `verified`. */
            const auto cue = document.findById (job.cue);

            OscJob done;
            done.self = job.self;
            done.wait = oscWaitFrom (textOf (cue, "wait"));
            done.ticksAllowed = std::max (0, static_cast<int> (std::lround (numberOf (cue, "timeout") * 50.0)));

            for (std::size_t n = 0; n < job.targets.size(); ++n)
            {
                const auto& target = job.targets[n];
                const auto ticket = target.ticketTick == tick ? target.ticket : 0u;
                const auto& expected = target.written.empty() ? target.last : target.written;

                if (n == 0)
                {
                    done.address = target.address;
                    done.ticket = ticket;
                    done.expected = expected;
                    continue;
                }

                OscJob::Further next;
                next.address = target.address;
                next.ticket = ticket;
                next.expected = expected;
                done.further.push_back (std::move (next));
            }

            if (mounts != nullptr)
            {
                done.mountId = mounts->mountOf (done.address);

                if (const auto* declaration = mounts->declarationOf (done.mountId))
                {
                    done.notSent = ! declaration->tx;
                    done.host = declaration->host;
                    done.queryPort = declaration->queryPort;
                }

                /*  ASKED AFRESH, each forgotten first - the verify's own rule. */
                if (done.wait == OscWait::verified && ! done.notSent)
                {
                    mounts->forgetReadback (done.address);

                    if (const auto* node = mounts->nodeAt (done.address))
                        done.typeTag = node->typeTags;

                    for (auto& next : done.further)
                    {
                        mounts->forgetReadback (next.address);

                        if (const auto* node = mounts->nodeAt (next.address))
                            next.typeTag = node->typeTags;
                    }
                }
            }

            sending.push_back (std::move (done));
            job.finished = true;
        }
    }

    void Runner::advanceSends (Engine& engine)
    {
        for (auto& job : sending)
        {
            /*  SETTLED ALREADY THIS TICK, by a revocation decided before this
                ran (`submitRevocation`, 2026-09-30): nothing more is read,
                written or reported for it. */
            if (job.finished)
                continue;

            /*  ITS CURVES HAVE THE RUN (namespace draft 45, O.4), unless its
                first write failed - then it fails here, as any cue's would. */
            if (job.curved && job.failure.empty())
            {
                job.finished = true;
                continue;
            }

            /*  Killed while it waited. A network cue holds no voice either, so
                the same gap as a fade's: `stopping` with nothing to act on it.
                A `verified` cue that somebody gave up on is the case - the
                device is not answering and the operator would like the show to
                stop asking. */
            /*  AND KILLED FROM ABOVE (2026-10-02, H4, namespace draft §23.10). A
                member of a killed scene is reached by the scene's job a tick
                after the press, and in that tick its job went on: a pre-send
                still asking read its answer and wrote the value - a datagram
                after the press, and a desk value nothing would put back - and
                a `sent` wait read its dropped ticket as a send that failed.
                Ended here, as the kill would have ended it a tick later. */
            const auto* selfRun = runs.find (job.self);

            if (selfRun != nullptr && ! selfRun->isFinished()
                  && (selfRun->state == runState::stopping || beingKilled (*selfRun)))
            {
                engine.submit (origin::engine, "run.ended", one (job.self));
                job.finished = true;
                continue;
            }

            /*  ALREADY OVER BY ANOTHER ROAD: revoked with the scene that held
                it, or ended by a jump. Nothing more is written and nothing is
                reported (2026-09-30, namespace draft §23). A pre-send still
                asking what the desk held used to go on asking, and wrote its
                value when the answer came - after the revocation that should
                have put the desk back, with nothing left to put it back again.
                A revocation reaches its runs without passing through
                `stopping`, which is why the branch above never saw it. */
            if (selfRun != nullptr && selfRun->isFinished())
            {
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
                        run->restoreAtom = osc::atomsOf (*held);
                    }

                    job.reading = false;
                    job.ticksWaited = 0;
                    writeOscNow (job);

                    /*  AND WHAT IT WROTE, as the tree took it (2026-10-02, K5's
                        review, LO): a put-back after a clock move is made only
                        while the desk still holds this. */
                    if (job.failure.empty() && ! job.left)
                        if (auto* writer = runs.find (job.self))
                            if (const auto* written = mounts->valueOf (job.address))
                                writer->preSentAtom = osc::atomsOf (*written);

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

            /*  NOTHING WAS SENT, BECAUSE NOBODY IS LISTENING BY CHOICE.

                Ended rather than failed, and the warning is set on the run
                here rather than submitted as a command for the reason
                `no-channel` is: it is a fact about how this run went, it is
                already being published from the run table, and a second
                command carrying it would be a record of nothing anybody
                decided. `run.ended` is the decision, and a replay takes the
                same branch off the same document. */
            /*  AND NOTHING WAS SENT BECAUSE A DOH LEFT IT WITH THE DEVICE'S
                OPERATOR (2026-10-01, namespace draft §24): ended the same way,
                its own word on the run, whatever its wait - a `verified` cue
                asks nothing of a device it did not write. */
            if (job.notSent || job.left)
            {
                if (auto* run = runs.find (job.self))
                    run->warning = job.left ? runWarning::leftToOperator : runWarning::notSent;

                engine.submit (origin::engine, "run.ended", one (job.self));
                job.finished = true;
                continue;
            }

            /*  `none` FINISHES WITHOUT ASKING ANYTHING, which is what makes it
                the right wait for a target that will never answer - a lighting
                desk, a projector, anything that takes a message and says
                nothing. It still finishes on the tick AFTER the cue fired,
                because that is when a report is allowed to leave, not because
                it waited for anything. */
            /*  NOTHING QUEUED AT ALL, for any of its messages - a curve's last
                values that had already left (namespace draft 45) - is done
                as `none` is. */
            const auto queuedAny = job.ticket != 0
                                     || std::any_of (job.further.begin(), job.further.end(),
                                                     [] (const OscJob::Further& next) { return next.ticket != 0; });

            if (job.wait == OscWait::none || sender_ == nullptr || ! queuedAny)
            {
                engine.submit (origin::engine, "run.ended", one (job.self));
                job.finished = true;
                continue;
            }

            /*  DROPPED BY A DOUBLE ESC (2026-10-02, H4, namespace draft §23.10):
                ended, never failed - the press is the account, and nothing went
                wrong on the wire. A run the press killed is ended above before
                it gets here; this is for a drop that reaches a run nobody
                killed, so that it reads as what happened rather than as
                `send-failed`. */
            /*  ANY OF ITS MESSAGES (namespace draft 45): the press dropped the
                cue, whichever of them it caught still waiting. */
            const auto dropped = sender_->wasDropped (job.ticket)
                                   || std::any_of (job.further.begin(), job.further.end(),
                                                   [this] (const OscJob::Further& next)
                                                   {
                                                       return next.ticket != 0 && sender_->wasDropped (next.ticket);
                                                   });

            if (dropped)
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

                /*  EVERY ADDRESS THE CUE WROTE (namespace draft 45): the first
                    to disagree fails it at once, and it is done when all have
                    answered with what they were given.

                    COMPARED AS THE NODE'S OWN TYPE, exactly. osc::Value's
                    equality is identity and not numeric equivalence, so a
                    float32 0.5 and a double 0.5 are different answers - and
                    that is right: the client coerced what the target said
                    to the type the node declared, so anything that still
                    differs is a difference the device made. */
                auto answered = 0u;
                auto disagreed = false;

                const auto judge = [&] (const std::string& address, const osc::Values& expected)
                {
                    if (const auto* said = mounts != nullptr ? mounts->readbackOf (address) : nullptr)
                    {
                        ++answered;
                        disagreed = disagreed || *said != expected;
                    }
                };

                judge (job.address, job.expected);

                for (const auto& next : job.further)
                    judge (next.address, next.expected);

                if (disagreed || answered == job.further.size() + 1)
                {
                    engine.submit (origin::engine,
                                   disagreed ? "run.failed" : "run.ended",
                                   disagreed ? std::vector<osc::Value> {
                                                   osc::Value::string (job.self),
                                                   osc::Value::string (oscError::disagreed) }
                                             : one (job.self));

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
                    outstanding" rather than "ask fifty times a second" - one
                    for each address still to answer. */
                if (asker != nullptr)
                {
                    if (mounts == nullptr || mounts->readbackOf (job.address) == nullptr)
                        asker->ask ({ job.mountId, job.host, job.queryPort,
                                      job.address, job.typeTag });

                    for (const auto& next : job.further)
                        if (mounts == nullptr || mounts->readbackOf (next.address) == nullptr)
                            asker->ask ({ job.mountId, job.host, job.queryPort,
                                          next.address, next.typeTag });
                }

                continue;
            }

            /*  `sent` ASKS THE SENDER WHAT HAPPENED. The flush ran at the end of
                the tick that queued this, so the answer is here by now; still
                pending means the flush never ran, which is a wiring fault and
                not something to keep waiting on. Either way the cue reports
                what happened rather than what was asked for, which is the whole
                difference between this wait and the one above. */
            /*  EVERY MESSAGE OF THE CUE (namespace draft 45): one that failed
                fails it, one still queued keeps it waiting, and it is done when
                all have left. */
            auto outcome = job.ticket != 0 ? sender_->outcomeOf (job.ticket) : tree::MountSender::Outcome::sent;
            auto queuedStill = job.ticket != 0 && outcome == tree::MountSender::Outcome::pending
                                 && sender_->stillQueued (job.ticket);

            for (const auto& next : job.further)
            {
                if (next.ticket == 0 || outcome == tree::MountSender::Outcome::failed)
                    continue;

                const auto its = sender_->outcomeOf (next.ticket);

                if (its == tree::MountSender::Outcome::failed)
                    outcome = its;
                else if (its == tree::MountSender::Outcome::pending)
                {
                    if (sender_->stillQueued (next.ticket))
                        queuedStill = true;
                    else
                        outcome = tree::MountSender::Outcome::failed;
                }
            }

            if (queuedStill && outcome != tree::MountSender::Outcome::failed)
                outcome = tree::MountSender::Outcome::pending;

            /*  STILL WAITING FOR A FLUSH THAT WILL TAKE IT, which a rate cap
                makes an ordinary thing rather than a wiring fault: the message
                is queued, in order, holding the newest value for its address,
                and it will go. What the cue asked for was that the value reach
                the target, so it keeps waiting - up to its own timeout, which
                is the same patience a `verified` cue has. */
            if (outcome == tree::MountSender::Outcome::pending && queuedStill)
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

        /*  WHETHER A JOB IS THE LAST OF ITS FADE CUE STILL MOVING: a fade that
            moves the level and the speed is two jobs under one run, and the run
            reports once, when the second of them is done (§22.6). */
        const auto lastOfItsCue = [this] (const FadeJob& job)
        {
            return std::none_of (running.begin(), running.end(),
                                 [&job] (const FadeJob& other)
                                 {
                                     return &other != &job && ! other.retired && other.self == job.self;
                                 });
        };

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
                /*  NEVER A GROUP (2026-09-30, namespace draft §23). A group's
                    `stopping` is its job's, which asked its members to stop on
                    the first tick of it; handed back to `playing` with a member
                    still on its way out, the scene played its next member once
                    that one had gone - after Esc, which stops the stop cue's run
                    with every other root. The hand-back is about a voice, which
                    `enforceStops` would otherwise cut at once.

                    NOR A TARGET THAT WAS KILLED ITSELF (2026-10-01, namespace
                    draft §23.6). The hand-back asked only whether the target's
                    stop had been issued: the running pane killing the stop cue
                    and its target in one drain handed the killed target back to
                    `playing`, and the kill was lost - the cue played on.

                    STILL NEVER A GROUP SINCE K3 (2026-10-02, §23.14), though
                    the group's job now waits for this fade before it asks its
                    members anything: handed back by a hook, with no record, a
                    group would be `playing` live and `stopping` in a replay,
                    and a GO on its row decides between the two (`fireStandby`).
                    So the stop it was asked lands now, gracefully - the job
                    that held it to the fade's end is gone. */
                /*  AND NOT A VOICE DOH! TOOK BACK (2026-10-01, namespace
                    draft §24): it is on its way out whatever becomes of the run
                    that set this stop going. The Doh gave it no fade of its own
                    because this stop lands sooner, and its scene then stopped
                    its members - this job's run among them - which handed the
                    sound back to `playing`: cut by the scene a tick later, short
                    of the stop it was fading to, or played on where no scene
                    asked again. So the job lets go of its run, which ends now,
                    and lands the stop itself, held by nobody's run - what Esc
                    does with a sooner stop. Decided from `takenBack`, which only
                    a handler writes. */
                if (job.stopWhenDone)
                    if (const auto* held = runs.find (job.heldRun());
                        held != nullptr && held->takenBack && ! held->isFinished())
                    {
                        const auto lastOne = lastOfItsCue (job);
                        const auto owner = job.self;

                        job.self.clear();
                        job.reportsSelf = false;

                        if (lastOne)
                            engine.submit (origin::engine, "run.ended", one (owner));

                        continue;
                    }

                if (job.stopWhenDone)
                    if (auto* held = runs.find (job.heldRun()))
                        if (held->state == runState::stopping && ! held->stopIssued
                              && ! held->isGroup() && ! held->skipFooter)
                            held->state = runState::playing;

                job.retired = true;

                if (lastOfItsCue (job))
                    engine.submit (origin::engine, "run.ended", one (job.self));

                continue;
            }

            /*  HELD UNTIL ITS SOUND IS HEARD (D2, MC): a job pushed for a run
                still on its disk keeps the run at where it starts from, so the
                ramp is not spent before anything sounds. With no audio side
                there is no launch to wait for. */
            if (job.waitsForLaunch && audio != nullptr && job.dca.empty())
                if (auto* held = runs.find (job.heldRun());
                    held != nullptr && ! held->isFinished()
                      && (held->launchedAtSample <= 0 || audio->samplesElapsed() < held->launchedAtSample))
                {
                    if (! job.movesRate && job.move.empty())
                        held->ownLevel = job.currentDb();

                    continue;
                }

            /*  The LEVEL stops advancing when it arrives; the JOB may not be
                over, because it can still be holding a stop that is due later.
                Before the author settled that, the two were the same thing and
                one counter did for both. */
            if (! job.isFinished())
                ++job.ticksDone;

            /*  A FADE ON A DCA writes the DCA's trim and nothing else: there is
                no run behind it to have gone, and nothing to stop when it
                arrives. It is over when its level is. */
            if (! job.dca.empty())
            {
                /*  A HAND ON THE DCA'S FADER TAKES OVER (namespace draft §26,
                    PI): it had been written under the hand fifty times a
                    second. The fade ends where the hand put the trim, and its
                    run reports done, as one a later fade takes over from. */
                if (dcas != nullptr && dcas->handSerialOf (job.dca) != job.handSerial)
                {
                    if (job.reportsSelf)
                        engine.submit (origin::engine, "run.ended", one (job.self));

                    job.retired = true;
                    continue;
                }

                if (dcas != nullptr)
                    dcas->set (job.dca, job.currentDb());

                if (! job.isFinished())
                    continue;

                /*  ONLY A JOB THAT HAS A RUN TO REPORT (2026-10-03, Doh! D3): a
                    trim Doh! brings back is nobody's run, and a `run.ended` of
                    nobody would be a refused record in the log. */
                if (job.reportsSelf)
                    engine.submit (origin::engine, "run.ended", one (job.self));

                job.retired = true;
                continue;
            }

            auto* target = runs.find (job.heldRun());

            /*  What was being faded has gone - it ended on its own, or somebody
                killed it. The fade has nothing left to do and says so, rather
                than writing levels into a voice that has moved on to another
                cue. */
            if (target == nullptr || (target->isFinished() && ! job.stopWhenDone))
            {
                job.retired = true;

                if (job.reportsSelf && lastOfItsCue (job))
                    engine.submit (origin::engine, "run.ended", one (job.self));

                continue;
            }

            /*  A SPEED FADE moves the run's own speed, which `applyRates`
                places on the voice a horizon ahead (namespace draft §22.4), and
                leaves its level to whatever else is moving it. */
            if (job.movesRate)
                target->ownRate = job.currentRate();

            /*  ONE OF THE RUN'S OWN NUMBERS (namespace draft §26): held on the
                run, and the three apply passes told by the revision - only when
                the value moved, so an arrived job holding a stop costs nothing. */
            if (! job.move.empty())
            {
                const auto value = job.currentValue();
                const auto was = target->moved.find (job.move);

                if (was == target->moved.end()
                      || std::bit_cast<std::uint64_t> (was->second) != std::bit_cast<std::uint64_t> (value))
                {
                    target->moved[job.move] = value;
                    ++movedRevision;
                }
            }

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
            if (! job.movesRate && job.move.empty())
                target->ownLevel = job.currentDb();

            /*  WHAT THIS JOB IS WAITING FOR. A plain fade is done when its
                level arrives. A job carrying a stop is done when the STOP is
                due, which for a stop cue fired on its own is the same tick and
                for one a later fade took over from is the tick the original
                stop was always going to land on. */
            if (job.stopWhenDone ? tick < job.stopsAtTick : ! job.isFinished())
                continue;

            /*  ARRIVED, AND CARRYING ON (D2, HG): a run Doh! carried on arrives
                at the level it had, then goes on, in this same job, to where
                the fade its scene had moving at the press was going - over the
                time that fade had left, and to its stop when it was a
                fade-and-stop. One job on the level throughout. */
            if (! job.stopWhenDone && job.then.has_value())
            {
                const auto segment = *job.then;
                job.then.reset();

                job.fromDb = job.toDb;
                job.toDb = segment.toDb;
                job.ticksTotal = std::max (segment.ticks, 1);
                job.ticksDone = 0;
                job.curve = segment.curve;
                job.points = segment.points;
                job.stopWhenDone = segment.stopWhenDone;
                job.stopsAtTick = tick + job.ticksTotal;
                continue;
            }

            if (job.stopWhenDone)
            {
                /*  SILENT FIRST, THEN STOPPED, and the order is the whole point
                    of the fade verb: by the time the clip stops the level is
                    already at silence, so Tracktion's own click suppression has
                    nothing left to suppress. */
                /*  NOT A RUN THAT HAS ALREADY ENDED (2026-09-28). A cue whose
                    file ran out during the fade gave its voice back when it
                    did, and a finished run still names that track - so a stop
                    issued now would land on whatever cue took the voice since.
                    Esc made that likely rather than rare: the fade is a second
                    long, and a GO inside it is exactly what an operator does
                    next. The job still runs to its tick for the stop cue's
                    own run; it just has nothing left to stop. */
                if (audio != nullptr && target->track >= 0 && ! target->isFinished())
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
                    the session it reproduces.

                    A SOUND'S END, NOT A SCENE'S (2026-09-30, namespace draft
                    §23): a group has no sound to report, and ending it here
                    ended the scene on the spot - its members not stopped in
                    order, its footer never run - in every `wfg serve` without
                    `--hosted`. Its own job ends it, through the footer. */
                if (audio == nullptr && ! target->isGroup())
                    engine.submit (origin::engine, "run.ended", one (target->id));
            }

            if (job.reportsSelf && lastOfItsCue (job))
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
                                [&snapshot] (const GroupJob& job) { return job.run == snapshot.id; })
                /*  A PICTURE ESC IS TAKING DOWN (Phase 8a): ended when it is
                    black, not on the press. */
                || std::any_of (showing.begin(), showing.end(),
                                [&snapshot, tick] (const VideoJob& job)
                                { return job.self == snapshot.id && job.endsAtTick > tick; })
                /*  AN OSC CUE'S CURVES (namespace draft 45): `advanceCurves` ends
                    its run where Esc found it - and its send job was handed over
                    to the curves at GO, so `sending` no longer claims it. */
                || isCurving (snapshot.id);

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
                is the whole reason the document has one door. `runsNow`
                reads the absent property as the default too (Override.h), and
                puts the override over it: a member a disable cue switched off
                is skipped from the next round on (namespace draft §27, PQ). */
            if (! runsNow (child))
                continue;

            /*  A SOUND LOCKED TO ITS MOVIE is fired by the movie (37.5, WJ). */
            if (followsAMovie (child))
                continue;

            out.push_back (id);
        }

        return out;
    }

    std::vector<std::string> Runner::soundsLockedTo (const std::string& movieCue) const
    {
        std::vector<std::string> out;

        if (movieCue.empty())
            return out;

        const juce::Identifier lockedTo { "lockedTo" };
        const juce::String wanted (movieCue);

        std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& node)
        {
            if (node.hasType ("Media") && node[lockedTo].toString() == wanted && runsNow (node))
                out.push_back (node[idProperty].toString().toStdString());

            for (const auto& child : node)
                visit (child);
        };

        if (const auto showLists = document.root().getChildWithName ("Lists"); showLists.isValid())
            visit (showLists);

        return out;
    }

    bool Runner::followsAMovie (const juce::ValueTree& cue) const
    {
        if (! cue.hasType ("Media"))
            return false;

        const auto movie = cue[juce::Identifier ("lockedTo")].toString().toStdString();
        return ! movie.empty() && document.findById (movie).hasType ("Video");
    }

    void Runner::fireLockedSounds (Engine& engine, std::int64_t tick, const std::string& movieCue,
                                   const std::string& movieRun)
    {
        for (const auto& soundCue : soundsLockedTo (movieCue))
        {
            std::string id;

            /*  ARMED BY THE STANDBY, or under a block the movie is in: taken. */
            if (const auto* live = runs.liveRunOf (soundCue))
            {
                const auto adoptable = live->state == runState::armed && ! live->launchRequested
                                    && ! claimedByAJob (live->id)
                                    && (live->parent.empty() || inAncestryOf (live->parent, movieRun));

                /*  PLAYING ALREADY, fired by name: left as it is. */
                if (! adoptable)
                    continue;
            }
            else
            {
                /*  MADE NOW: FNV-1a over the two identifiers, the same on
                    every machine, salted until free. */
                const auto joined = movieRun + "/" + soundCue;

                for (std::uint64_t salt = 0; salt < 64 && id.empty(); ++salt)
                {
                    std::uint64_t hash = 14695981039346656037ull ^ (salt * 0x9e3779b97f4a7c15ull);

                    for (const auto c : joined)
                    {
                        hash ^= static_cast<std::uint8_t> (c);
                        hash *= 1099511628211ull;
                    }

                    if (const auto candidate = doc::Id::encode (hash);
                        runs.find (candidate) == nullptr && ids.reserve (candidate))
                        id = candidate;
                }

                if (id.empty())
                    continue;
            }

            if (const auto made = spawnChild (engine, movieRun, soundCue, id, tick); ! made.empty())
                launchRun (engine, tick, made);
        }
    }

    void Runner::endLockedSounds (Engine& engine, const std::string& movieRun)
    {
        const auto* movie = runs.find (movieRun);

        if (movie == nullptr)
            return;

        const auto graceful = ! movie->killed && ! movie->skipFooter;

        for (const auto* child : runs.childrenOf (movieRun))
            if (child != nullptr && ! child->isFinished() && child->state != runState::stopping
                  && followsAMovie (document.findById (child->cue)))
                endMember (engine, *child, graceful);
    }

    std::string Runner::spawnChild (Engine& engine, const std::string& parentRun,
                                    const std::string& cueId, const std::string& runId,
                                    std::int64_t tick, bool fromItsRecord)
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

            /*  TAKEN UNDER A GROUP A GO MADE OR ADOPTED, it is that GO's too
                (2026-10-01, namespace draft §24). */
            if (const auto* parent = runs.find (parentRun); parent != nullptr && parent->goSerial != 0)
                stampSubtree (id, parent->goSerial);

            /*  AND A SAMPLER GROUP'S MEMBER takes its strip even when it was
                armed ahead - which the horizon does not do for a sampler group,
                and which costs nothing to be sure of. */
            if (auto* adopted = runs.find (id); adopted != nullptr && ! adopted->sampler)
                if (const auto* parent = runs.find (parentRun))
                    if (const auto parentCue = document.findById (parent->cue);
                        parentCue.isValid() && textOf (parentCue, "mode") == "sampler"
                          && kind == "media")
                    {
                        adopted->sampler = true;
                        const auto stripId = stripForMember (parentCue, cueId);
                        adopted->trim = numberOf (cue, "initialLevel");
                        claimStripFor (*adopted, stripId);
                    }

            return id;
        }

        createRun (id, cueId, kind, parentRun);

        auto* run = runs.find (id);

        if (run == nullptr)
            return {};

        run->preWaitTicks = ticksFor (numberOf (cue, "preWait"));
        run->postWaitTicks = ticksFor (numberOf (cue, "postWait"));

        /*  SPAWNED INTO A GROUP DOH! TOOK BACK - or, by a `run.spawn` record,
            into one brought back to life in this very tick, whose job decided
            the spawn on the state the Doh has undone (2026-10-01, namespace
            draft §24, GZ) - it is over before it began: made, so the record
            that drew its identifier still names a run, and done at once,
            holding nothing.

            THE RECORD ONLY. A GO drained after the Doh in that same tick - a
            script, a second hand - decided on the state the Doh left, and the
            member it fires into the act brought back is the corrected GO's
            own: born done, the act took it for its last member played and ran
            its footer, and that GO played nothing. */
        if (const auto* parent = runs.find (parentRun);
            parent != nullptr
              && (parent->takenBack
                    || (fromItsRecord && parent->unadoptedAt >= 0 && parent->unadoptedAt == tick)))
        {
            run = runs.find (id);
            run->state = runState::done;
            run->endedAtTick = tick;

            /*  AND TAKEN BY THE JOB IT WAS SPAWNED FOR (D2): a block the Doh
                handed back holds its job as it was before the GO, which has
                never heard of this run - and a phase finding a finished run of
                one of its cues that it has not taken walks past that cue as
                already run. */
            for (auto& job : scheduled)
                if (job.run == parentRun && ! job.retired && ! job.hasTaken (id))
                    job.taken.push_back (id);

            return id;
        }

        /*  ARMED AND NOT LAUNCHED. A media member reserves its voice and asks
            for its file here, which is the whole reason spawning is a separate
            moment from launching: an auto sequence spawns the next member while
            the current one is still playing, so the disk is paid for before the
            chain arrives rather than after. Every other kind has nothing to make
            ready and simply waits in the state it was born in. */
        /*  A SAMPLER GROUP'S MEMBER IS ARMED ONTO A STRIP (PRD §3.27): the one
            its place among the group's media members lands on, counted onto
            the sampler strips of every surface. Its trim starts at the
            member's `initialLevel` (author, 2026-09-23) - where a fader flies
            to and waits for the touch that starts the clip, and where a pad's
            press without velocity plays - so §3.9a's start value "reasserted
            at every handover" is simply a fresh run. A strip another group's
            clip is still sounding on is WAITED for, and the member is armed
            when it lands: a voice held for a strip nobody can press would be a
            voice for nothing. */
        if (kind == "media")
            if (const auto* parent = runs.find (parentRun))
                if (const auto parentCue = document.findById (parent->cue);
                    parentCue.isValid() && textOf (parentCue, "mode") == "sampler")
                {
                    run->sampler = true;
                    const auto stripId = stripForMember (parentCue, cueId);
                    run->trim = numberOf (cue, "initialLevel");
                    claimStripFor (*run, stripId);

                    if (std::find (run->claims.begin(), run->claims.end(), stripId)
                          == run->claims.end())
                        return id;
                }

        /*  A SOUND INSIDE A RUNNING ACT THAT A DOH PAUSED, made ready again by
            the horizon under that act (D2, HF): at its point, with no
            pre-wait, arriving over the de-click - the arm the corrected GO asks
            for, as it asks for any member the horizon armed. */
        if (makingForHorizon)
            if (const auto* root = soundRootOf (cueId); root != nullptr && root->underRun == parentRun)
                armAtRoot (*run, *root, cue);

        if (kind == "media")
            armMedia (engine, cue, id);

        return id;
    }

    void Runner::launchRun (Engine& engine, std::int64_t tick, const std::string& runId)
    {
        auto* run = runs.find (runId);

        /*  NOR ONE DOH! TOOK BACK (2026-10-01): its job's launch, decided
            before the Doh, launches nothing. Nor one a kill has reached, which
            `fireNow` says why (H4): its pre-wait is not begun either. */
        if (run == nullptr || run->isFinished() || run->takenBack || beingKilled (*run))
            return;

        /*  THE HORIZON'S ARM, LAUNCHED AS A MEMBER OF THE GO'S OWN SCENE, IS
            THAT GO'S (D2's review, MZ, which retires L42). A second GO pressed
            before the horizon's `run.prepare` of the pointer's scene had drained
            - two GOs inside a tick, a footswitch that bounces with the debounce
            at nought, PRD §4.5's double GO - entered the scene cold, and the
            preparation then armed the scene's first sound under it, nobody's
            GO; the scene's job took it from a hook, which cannot stamp. Heard,
            the scene read unheard to a Doh, and was taken down rather than
            paused. Stamped here, by the launch's own handler, from fields only
            handlers write: made by the horizon after the open record's GO,
            under a run of that GO. */
        if (run->goSerial == 0 && goRecord.serial != 0
              && run->preparedAfterGo >= static_cast<std::int64_t> (goRecord.serial))
            if (const auto* parent = runs.find (run->parent); parent != nullptr && parent->goSerial == goRecord.serial)
            {
                stampSubtree (runId, goRecord.serial);
                run = runs.find (runId);
            }

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
        /*  AND A SHUFFLING RUN WITH NO SEED OF ITS OWN YET DRAWS ONE, whatever
            round it is on (J1, 2026-10-01, namespace draft §23.8). A fired run
            has one from its first round - the group's, or one drawn then - but
            a run a jump seated is in round one already, holding the group's
            seed, which is nought for a shuffle nobody seeded. Read as its own,
            every round after the jump came from nought: the same order after
            every jump, from a seed the show never used. Nought is "no seed" -
            `IdRegistry::drawSeed` never draws it - so it is drawn here, and the
            record below writes it into the log, as every seed is. */
        const auto seed = run->iteration > 0 && (run->seed != 0 || ! shuffles)
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
            /*  A RETIRED JOB IS OVER, whatever its run is doing. A re-seat
                (`seekGroup`) retires the scene's job and pushes a fresh one
                for the same run in one drain, and the old one - still in the
                list until the sweep below - would otherwise take the members
                the fresh one just made and launch them a second time, which
                is what the first live scrub did (2026-09-18). */
            if (job.retired)
                continue;

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
                and asks nothing of the cue" - the double Esc's. The two are
                told apart by the flag `run.kill` and `run.killAll` set, because
                both write the same `stopping` state and the state alone cannot
                say which was meant.

                A FOOTER ALREADY RUNNING IS LEFT TO FINISH by a graceful stop,
                which asked for exactly that - and CUT by a kill (2026-09-30,
                namespace draft §23). A double Esc pressed while Esc's footer
                holds is still §4.4's immediate level, and it skips footers,
                that one included: the branch used to stand aside for any footer,
                and the scene played its release to the end through the press
                that promises everything is dropped.

                AND THE KILL IS READ FROM ABOVE (2026-09-30, namespace draft
                §23.2). A killed group reaches its members through its own job, a
                tick after the press - so a scene inside one, Esc's graceful stop
                already on it, read its own unmarked flag for that tick and could
                begin its footer under the double Esc that promises none. Every
                double Esc is two presses, Esc first, which made that tick an
                ordinary one. A group under a killed one ends as a kill, whatever
                it was told itself. */
            if (run->state == runState::stopping)
            {
                const auto graceful = ! run->skipFooter && ! underAKill (*run);

                /*  A SCENE DOH! TOOK BACK, NONE OF IT HEARD (2026-10-01,
                    namespace draft §24), wherever its job had got to - its
                    footer included: a Doh runs no footer, so it does not leave
                    one running either. Left to finish there, as a graceful stop
                    leaves a footer, an unheard scene caught in its footer waited
                    on a scene in that footer, which waited on it. */
                const auto givenBack = run->takenBack
                                         && (job.phase == groupPhase::entering || job.phase == groupPhase::header
                                               || job.phase == groupPhase::members || job.phase == groupPhase::footer)
                                         && ! heardUnder (job.run, goOfRun (job.run));

                /*  A FADE-AND-STOP IS A FADE UNTIL IT ARRIVES (2026-10-02, K3,
                    namespace draft §23.14; the author: "Fade and stop is a fade
                    behaviour, not a stop feature that just stops"). A stop cue's
                    `fade`, or a fade cue that stops when done, marks the group
                    `stopping` from its first tick - a cue on its way out says
                    so - and this branch used to stop its members on the next:
                    the scene ended at once, and only its level, already over
                    nothing, moved for the duration. Now, while such a job holds
                    the group and its stop is still ahead, the job plays the
                    scene on as it would - members sounding, the sequence
                    advancing, under the fading level - and once the stop is due
                    the branch below brings it down as any stop of the group
                    does: members aborted (K2), footer run, under the level the
                    fade has left at silence. The one path a cue's fade-and-stop
                    takes, where `enforceStops` holds the voice to the fade's
                    end. An abort on it or above it lets go at once
                    (`fadingToItsStop`); a preparation is given back as before. */
                const auto fading = graceful && ! givenBack
                                      && job.phase != groupPhase::preparing && job.phase != groupPhase::prepared
                                      && fadingToItsStop (*run);

                if (! fading && (job.phase != groupPhase::footer || ! graceful || givenBack))
                {
                    /*  A GROUP THAT NEVER STARTED HAS NO FOOTER (2026-09-30,
                        namespace draft §23). Stopped while the horizon was still
                        making it ready - a scene prepared inside one that Esc is
                        bringing down, or a stop aimed at the prepared run - it
                        has taken nothing a footer would give back, and running
                        one would release things it never held. It is given back
                        the way the pointer moving away gives a block back: what
                        it pre-sent put back first, then revoked, the whole block
                        with it. A kill is given back the same way - a revocation
                        is not a footer.

                        BUT WHAT SOMEBODY ASKED FOR INSIDE IT IS STOPPED FIRST.
                        A block can hold a sound: a member fired by name out of
                        it launches where the horizon armed it, and a revocation
                        ends a run on the model alone - that voice played on with
                        no run owning it, out of every later Esc's reach. So each
                        such run is ended the way the group is ending, and the
                        block is given back once they have all gone. What is left
                        is only made ready, and `run.revoke` ends the rest of the
                        block, nested prepared groups included, without asking
                        any member to stop: a member stopped on the way would end
                        by another road with its job still running. */
                    if (job.phase == groupPhase::preparing || job.phase == groupPhase::prepared)
                    {
                        if (endAskedForIn (engine, job.run, graceful))
                            continue;

                        submitRevocation (engine, job.run);
                        job.retired = true;
                        continue;
                    }

                    /*  A SCENE DOH! TOOK BACK, NONE OF IT HEARD: a lighting
                        scene, a scene still in its header, a cue in its
                        pre-wait - nothing of it reached the room, so it is given
                        back the way a preparation is, H2's road: what it
                        pre-sent put back first - the pre-sends whose cue takes
                        back, the Doh having cleared the others - then revoked,
                        the whole scene with it. The horizon's restore base stays
                        true, and the next GO prepares it again.

                        WHAT IT PRE-SENT GOES BACK AT ONCE, on the first tick it
                        is stopping: before this tick's `armStandby` asks the
                        horizon to prepare the scene again for the restored
                        pointer. Held back, as the first build held it, behind a
                        fade the scene had fired that ran on for seconds, the
                        restore landed over the new preparation's pre-send - the
                        value from before the GO written over the corrected GO's
                        own. Each restore is submitted once, so asking every tick
                        is safe.

                        WHAT STILL MOVES UNDER IT IS LET FINISH, so nothing is
                        ended on the model under a job still writing: a fade or
                        a stop cue the GO fired on a cue outside the scene, which
                        a Doh leaves to run. BUT ONLY WHAT A JOB DRIVES. A member
                        spawned and never launched, a fade whose job a double Esc
                        dropped or Esc let go of - nothing would ever end those,
                        and the scene waited on them for ever, an act around it
                        never reaching its footer. So each is asked to stop the
                        way the scene is stopping, as the members of any stopping
                        scene are; under a kill, everything is. A scene inside
                        this one gives itself back the same way, and this one
                        waits for it, innermost first. */
                    if (givenBack)
                    {
                        submitRestores (engine, job.run);

                        for (const auto* child : runs.childrenOf (job.run))
                            if (! graceful || ! drivenByAJob (*child))
                                endMember (engine, *child, graceful);

                        if (! runs.allChildrenFinished (job.run))
                            continue;

                        engine.submit (origin::engine, "run.revoke", one (job.run));
                        job.retired = true;
                        continue;
                    }

                    for (const auto* child : runs.childrenOf (job.run))
                        endMember (engine, *child, graceful);

                    if (! runs.allChildrenFinished (job.run))
                        continue;

                    /*  AND A SCENE DOH! TOOK BACK RUNS NO FOOTER (§24): a
                        Doh is a pause, not an end. Never `skipFooter`, which
                        since H1 means cut: its members came down the Esc way. */
                    if (! graceful || run->takenBack || ! beginPhase (engine, job, group, groupPhase::footer))
                    {
                        engine.submit (origin::engine, "run.ended", one (job.run));
                        job.retired = true;
                    }

                    continue;
                }
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
                    /*  NOR A MIC CUE'S (2026-09-30, namespace draft §23.3): its
                        preparation is its arm - the channel claimed, the
                        plugins set, the gate shut - made in `beginPreparation`.
                        Launched here, a mic cue in a scene's header opened its
                        gate while the pointer was only resting on the scene;
                        and so did the pointer's own mic member, which the
                        horizon puts under the block ahead of the GO. The GO
                        that enters the scene launches either. */
                    if (child->kind == "media" || child->kind == "mic")
                        continue;

                    /*  NOR A BLOCK NESTED IN THIS ONE (2026-10-01, namespace
                        draft §23.9, IE). The horizon makes the next scene down
                        the pointer's chain under this block, and it is not a
                        header cue to pre-send: it waits for a GO as this one
                        does. Launched here, it was let out of its hold through
                        `fireKind`'s third door and played, with no GO pressed,
                        whenever this block's header had anything to take ahead
                        - a bed, a pre-send - and since IB a sampler bank so
                        launched closed the others as well. Its parent's members
                        launch it, once a GO has entered the parent. */
                    if (child->isGroup())
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

                /*  AN ADOPTED BLOCK STAYS ADOPTED (2026-10-01, namespace draft
                    §23.9). A GO that adopts a block under a running group clears
                    its mark and leaves it here until that group's job launches
                    it, a tick later; marked again in that tick, it read as a
                    scene nobody had asked for - which the pointer moving on
                    gives back, the GO's own scene and its desk value with it,
                    and which a group's job does not take. */
                if (preparationSettled (job))
                {
                    job.phase = groupPhase::prepared;

                    if (! run->prepare.empty())
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
            /*  A SAMPLER GROUP'S MEMBERS ARE NOT A ROUND (PRD §3.27): nothing
                is drawn, nothing is launched by the scheduler, and nothing is
                waited on in order. What the phase does every tick is keep the
                bank armed - its own function, because it shares nothing with
                the loop below but the phase's name. */
            if (job.phase == groupPhase::members && textOf (group, "mode") == "sampler")
            {
                samplerTick (engine, job, group, *run);
                continue;
            }

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

            /*  A SAMPLER GROUP IS A WINDOW ON THE SIDE OF THE CUES (author,
                2026-09-23): its members can be played at any moment, and the
                list goes on until somebody stops it. So a sequence that plays
                itself arms one and moves on at once - waiting for it to finish
                would hold the rest of the scene for as long as the pads were
                live, which is the whole of the time they are wanted. */
            const auto handedOver = [this] (const Run& member)
            {
                return member.kind == "group" && member.state == runState::playing
                         && textOf (document.findById (member.cue), "mode") == "sampler";
            };

            if (awaited == nullptr || ! (awaited->isFinished() || handedOver (*awaited)))
                continue;

            job.awaiting.clear();

            /*  A MEMBER THE HORIZON ALREADY RAN IS ALREADY OVER, and the phase
                walks past it rather than waiting for it.

                §13.7 gives the horizon a member's PREPARABLE PART and leaves
                the rest for its own moment: for a media cue the rest is the
                launch, and for a network cue that was read, pre-sent and
                verified there is no rest at all - the value is on the desk and
                the desk agreed. Its run is finished before the group ever
                reaches its row.

                Without this the phase stopped there for ever: the advance below
                marks the standing run as asked-for and waits for something
                `armed` to launch, and a run that is already `done` will never
                be either. A scene whose second member was a preset network cue
                played its first member and then held the show. Found by the
                Phase 4 driver, which is a scene of exactly that shape. */
            while (job.nextMember < job.phaseCues.size())
            {
                const auto& candidate = job.phaseCues[job.nextMember];
                const Run* alreadyRan = nullptr;

                for (const auto* child : runs.childrenOf (job.run))
                    if (child->cue == candidate && ! job.hasTaken (child->id)
                         && child->isFinished())
                        alreadyRan = child;

                if (alreadyRan == nullptr)
                    break;

                askedFor (alreadyRan->id);
                ++job.nextMember;
            }

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

            /*  AND THE SEQUENCE LASTS UNTIL ITS BANK IS STOPPED: the round is
                not over while a sampler group it armed is still live, so a
                scene's pads live as long as the scene, stopping the scene takes
                them with it, and a second round never arms a second copy of a
                bank that is still armed. Every other member of a sequence has
                finished before the next began, so this waits on nothing else -
                and it waits by AWAITING the bank, which is where the next tick
                looks: with nothing awaited, the loop above only ever goes
                looking for an armed member to launch. */
            if (! allFinished())
            {
                for (const auto* child : children)
                    if (! child->isFinished())
                    {
                        job.awaiting = child->id;
                        break;
                    }

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
        /*  A SAMPLER GROUP DRAWS NO ROUND: its members are played by hand in
            any order, so `selection`, `play` and `loops` mean nothing to it -
            the manual group's rule (namespace draft §12.5), for the same
            reason. Its phase is its media members, and the scheduler's tick
            arms them; nothing is spawned here. */
        const auto sampler = phase == groupPhase::members && textOf (group, "mode") == "sampler";

        auto cues = sampler ? membersOf (group)
                  : phase == groupPhase::members
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

        if (sampler)
            return true;

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
            `advanceGroups` returns before it can, and one that is stopped is
            revoked there rather than ended (2026-09-30) - and this says so at
            the door rather than leaving it to a reading of a loop three hundred
            lines long. What it would otherwise do is run the group's footer and
            end the scene before anybody pressed GO. */
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

        /*  WHAT A CLOCK MOVE GAVE BACK, PUT BACK, AND THEN THE STANDBY MADE
            READY AGAIN (2026-10-02, K5, namespace draft §23.16; the author's
            ruling 6d: "They can be sent back").

            `audio.clockMoved` revoked the prepared runs in its handler and
            handed them here (`putBackWhenWritable`). Their values go back as at
            every give-back - the ordinary `node.set`, once each - but not while
            the outage stands: the engine refuses every write until the show
            runs on the interface's clock again, and a restore refused is a
            restore lost. So they wait for the engine to take a write.

            THEN THE STANDBY IS MADE READY AGAIN, from this same pass below and
            so AFTER the restores: they are submitted first and applied first in
            the drain, ahead of the `run.prepare`, and the fresh pre-send asks
            the desk what it holds a tick later at the earliest - so it reads
            the value put back, which is what it will restore to in its turn.
            Read before the put-back, it would have kept the scene's own value
            as the desk's, and the next give-back would have left it there.
            `audio.settingsReady` asks for the preparation again too
            (`resetAudioPreparation`); asking here as well covers an outage the
            follow ended without one, which resumes on the graph it had.

            AND NOT AT ONCE WHEN SOMETHING WENT BACK (K5's extension, LE): the
            preparation waits `putBackSettleTicks` after the restores, below,
            so the desk has had the put-back's datagram for a tenth of a second
            before the fresh pre-send asks it what it holds - a device slow to
            apply a write would otherwise answer with the scene's value. The
            same since 2026-10-02 for a settings operation (`audio.apply`,
            `audio.setup`, `plugin.load`), which revokes in its handler and
            hands over the same way. */
        /*  AND NOTHING IS MADE READY WHILE A PUT-BACK WAITS FOR THE CLOCK
            (2026-10-02, K5's review, LM). The first tick the show runs again
            can still have `audio.settingsReady` queued and not drained, so a
            write is not admitted yet; a pointer Doh! moved during the outage
            then had the latch below ask for its scene in that same drain -
            made ready and pre-sent ahead of the put-back, reading the scene's
            value as the desk's. Every preparation waits for the put-back now,
            as it would have been refused anyway while the clock was away. */
        if (waitingForWrites)
        {
            if (! engine.admits ("node.set"))
                return;

            if (advancePutBacks (engine))
                prepareAfterPutBack = currentTick + putBackSettleTicks;

            armedStandby.clear();
            waitingForWrites = ! owedPutBacks.empty();
        }

        /*  A BLOCK THE POINTER LEFT WITH SOMETHING PLAYING IN IT, given back
            once that has gone (2026-09-30, namespace draft §23.3) - asked every
            tick, above the gate below, because what it waits for is the end of
            a cue and not a move of the pointer. Let go of instead when the block
            ended by another road, was entered by a GO, or is wanted again: the
            pointer back in it. */
        /*  Given back in this tick, so the loop below - which reads the table
            before this tick's `run.revoke` is applied - does not give the same
            block back twice, restores and all. */
        std::vector<std::string> givenBack;

        if (! blocksToGiveBack.empty())
        {
            /*  Wanted again, or adopted: `leftBehind` asks both, of a block at
                the top of a list and of one under an act alike. */
            const auto horizon = horizonGroupsFor (list, standby);
            std::vector<std::string> stillPlaying;

            for (const auto& blockId : blocksToGiveBack)
            {
                const auto* block = runs.find (blockId);

                if (block == nullptr || ! leftBehind (*block, standby, horizon))
                    continue;

                if (runs.askedForUnder (blockId))
                {
                    stillPlaying.push_back (blockId);
                    continue;
                }

                submitRevocation (engine, blockId);
                givenBack.push_back (blockId);
            }

            blocksToGiveBack = std::move (stillPlaying);
        }

        /*  A SCENE MADE READY UNDER A SCENE A FADE IS TAKING DOWN, given back at
            once (2026-10-02, K3's review, namespace draft §23.14, KU) - asked
            every tick, above the gate below, since what it waits for is a fade
            beginning and not a move of the pointer. Since K3 a fade-and-stop
            keeps its scene `stopping` for the length of the fade, and nothing
            will ever enter a block under it: a GO on that row builds the scene
            afresh beside it (`fireStandby`). Left to the scene's job, the block
            was given back when the fade ended - its pre-send put back over the
            value the GO in between had sent, the desk left as it was before
            the show got there. Given back now, before any such GO, and made
            again for the pointer when it is the pointer's own scene: the
            horizon builds it under a fresh run of the act (`prepareStandby`,
            `goingOut`), which the GO adopts. Only a block nobody asked for
            anything in, as at every give-back; a block under a scene whose stop
            has landed is its job's, in the same tick, as it always was. */
        {
            const auto horizon = horizonGroupsFor (list, standby);
            auto prepareAgain = false;

            const auto isBlock = [] (const Run& candidate)
            {
                return candidate.onlyPrepared() && candidate.state == runState::preparing
                         && ! candidate.prepare.empty();
            };

            /*  The first `stopping` run above it decides: a fade still holding
                it, or a stop that has landed and is its job's. */
            const auto underAFade = [this] (const Run& candidate)
            {
                const auto* above = runs.find (candidate.parent);

                for (std::size_t guard = 0; above != nullptr && ! above->isFinished()
                                              && guard <= runs.all().size(); ++guard)
                {
                    if (above->state == runState::stopping)
                        return fadingToItsStop (*above);

                    above = above->parent.empty() ? nullptr : runs.find (above->parent);
                }

                return false;
            };

            for (const auto& snapshot : runs.all())
            {
                if (! isBlock (snapshot) || snapshot.parent.empty()
                      || std::find (givenBack.begin(), givenBack.end(), snapshot.id) != givenBack.end())
                    continue;

                /*  THE OUTERMOST BLOCK ONLY: a revocation takes the blocks
                    inside it with it, restores and all. */
                if (const auto* parentBlock = runs.find (snapshot.parent);
                    parentBlock != nullptr && isBlock (*parentBlock))
                    continue;

                if (! underAFade (snapshot) || runs.askedForUnder (snapshot.id))
                    continue;

                submitRevocation (engine, snapshot.id);
                givenBack.push_back (snapshot.id);

                if (std::find (horizon.begin(), horizon.end(), snapshot.cue) != horizon.end())
                    prepareAgain = true;
            }

            if (prepareAgain && ! standby.empty())
                engine.submit (origin::engine, "run.prepare", one (standby));
        }

        /*  A VOICE DOH! IS FADING OUT, WAITED FOR (2026-10-01, namespace
            draft §24): the standby's arm of the cue was put off while the old
            run still held it, and once that run has gone it is asked for again
            - `run.prepare` for a standby inside a scene, which rebuilds only
            what is missing, or the arm alone - with the latch below left alone,
            so nothing else this pass does is done twice. A move of the pointer
            forgets it. */
        if (! waitingForVoice.empty() && standby != armedStandby)
            waitingForVoice.clear();

        /*  A SCENE A DOH PAUSED, AT THE STANDBY, IS RE-SEATED BY THE NEXT GO
            (2026-10-02, D2, namespace draft §24.12): nothing is made ready for it - a block
            prepared now would be a second copy of a scene the GO carries on.
            A paused sound is made ready, at its point, by the roads below. */
        const auto listId = list.isValid() ? list[idProperty].toString().toStdString() : std::string {};
        const auto* markHere = markFor (listId, standby);
        const auto pausedScene = markHere != nullptr && markHere->root->kind == DohRoot::Kind::tree;

        if (pausedScene)
            waitingForVoice.clear();

        if (! waitingForVoice.empty())
        {
            const auto* waited = runs.find (waitingForVoice);

            if (waited == nullptr || waited->isFinished())
            {
                waitingForVoice.clear();

                if (const auto cue = document.findById (standby); cue.isValid())
                {
                    if (cue.getType().toString() == "Group" || ! descentTo (list, standby).empty())
                    {
                        engine.submit (origin::engine, "run.prepare", one (standby));
                    }
                    else if (audio != nullptr)
                    {
                        for (const auto& id : armablesFor (cue))
                        {
                            if (const auto* live = runs.liveRunOf (id))
                            {
                                if (live->takenBack)
                                    waitingForVoice = live->id;

                                continue;
                            }

                            engine.submit (origin::engine, "audio.arm", one (id));
                        }
                    }
                }
            }
        }

        /*  THE PUT-BACK SETTLING (above, LE): the pointer's arm and block are
            asked for once it has, the latch left open until then. */
        if (currentTick < prepareAfterPutBack)
            return;

        if (standby == armedStandby)
            return;

        armedStandby = standby;

        /*  WHAT THE POINTER LEFT BEHIND.

            A horizon prepares ONE block - §3.12 extends anticipation from a row
            to a block and no further - so anything parentless still in
            `preparing` that is not the block the pointer is in now is a scene
            got ready for a GO that is not coming. Its voices and its slots go
            back.

            AND A SCENE MADE READY UNDER AN ACT THAT IS RUNNING (2026-10-01,
            namespace draft §23.9, J2): the horizon prepares the pointer's scene
            under the act when the act is live, so a block can be left behind
            with a parent. Only the parentless were looked at, and a scene the
            pointer passed over inside a running act held its voices and its
            pre-sends until the act ended - and after. `leftBehind` is the test,
            for both.

            Submitted rather than done here, because a hook decides and a
            handler applies: `wfg replay` runs no hooks, so a revocation that
            happened only inside one would be missing from every replay - and
            the replay would then hold voices the session let go of. */
        const auto horizon = horizonGroupsFor (list, standby);

        for (const auto& snapshot : runs.all())
        {
            /*  A PLAIN MEDIA CUE AT STANDBY IS THE SMALLEST HORIZON THERE IS,
                and it leaked until the Phase 4 driver walked the pointer past
                one. Arming it reserves a voice and, since PR 4.3, claims its
                slots; moving the pointer on left all of that held by a run in
                `armed` that nobody would ever launch, so scrolling a list of
                media cues emptied the rack one cue at a time.

                WHAT MAKES IT SAFE TO END is the `prepare` mark and nothing
                else. A run the horizon armed carries it; `askedFor` clears it
                the moment anybody asks for that cue - a GO, a `cue.fire`, a
                group adopting it - so a cue somebody fired is never in this
                set, however recently it was armed. The mark IS the difference
                between "made ready in case" and "wanted", and
                `Run::onlyPrepared` is that test with `preparing` beside it -
                the same one Esc leaves alone by (2026-09-30). */
            if (! leftBehind (snapshot, standby, horizon)
                 || std::find (givenBack.begin(), givenBack.end(), snapshot.id) != givenBack.end())
                continue;

            /*  NOT WHILE SOMETHING IN IT PLAYS (2026-09-30, namespace draft
                §23.3). A member fired by name out of the block launched where
                the horizon armed it, and a revocation ends everything under a
                block on the model alone: the voice would play on with no run
                owning it, out of reach of any Esc. It is somebody's cue now and
                plays on; the block waits for it, above. */
            if (runs.askedForUnder (snapshot.id))
            {
                if (std::find (blocksToGiveBack.begin(), blocksToGiveBack.end(), snapshot.id)
                      == blocksToGiveBack.end())
                    blocksToGiveBack.push_back (snapshot.id);

                continue;
            }

            submitRevocation (engine, snapshot.id);
        }

        if (standby.empty() || pausedScene)
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
                happened.

                OR ON ITS WAY OUT UNDER A DOH (2026-10-01, §24): remembered, and
                armed once its voice is free - an arm now would take a second
                voice, the corrected GO's while the old one fades. */
            if (const auto* live = runs.liveRunOf (id))
            {
                if (live->takenBack)
                    waitingForVoice = live->id;

                continue;
            }

            engine.submit (origin::engine, "audio.arm", one (id));
        }
    }

    void Runner::putBackWhenWritable (const std::vector<std::string>& revokedRuns)
    {
        for (const auto& id : revokedRuns)
            if (std::find (owedPutBacks.begin(), owedPutBacks.end(), id) == owedPutBacks.end())
                owedPutBacks.push_back (id);

        if (! revokedRuns.empty())
            waitingForWrites = true;
    }

    bool Runner::advancePutBacks (Engine& engine)
    {
        /*  ONE PASS OVER WHAT A CLOCK MOVE OR A SETTINGS OPERATION GAVE BACK
            (2026-10-02, K5's review, namespace draft §23.16), each pre-send
            decided by what the mounted tree holds at its address now:

            - THE VALUE TO PUT BACK: it went back - the `node.set` this hook
              submitted on an earlier pass was applied. Settled.
            - WHAT THE PRE-SEND WROTE: nobody has written since, so the value
              goes back, as an ordinary `node.set` - submitted, and the run kept
              owed until a later pass sees it landed. Admission is only a
              forecast here: a record queued ahead of the restore in its drain -
              `audio.connection` false from an interface that drops again, a
              settings operation - can still have it refused, and a restore
              forgotten as it was submitted was lost for good (LN).
            - ANYTHING ELSE: somebody wrote since - a client's `node.set`, a
              load-to-time's writes, in the drain that ended the outage - and
              that write is newer than anything the show remembers. Not put
              back over (LO). Settled.

            A pre-send with no record of what it wrote, or a show with no mount
            table, is put back once and forgotten, as every other give-back
            does. Hook state throughout: the restore fields are the hooks', the
            `node.set` is a logged record a replay re-injects. */
        auto submitted = false;
        std::set<std::string> seen;
        std::vector<std::string> stillOwed;

        for (const auto& blockId : owedPutBacks)
        {
            std::vector<std::string> under;

            for (const auto* below : runs.descendantsOf (blockId))
                under.push_back (below->id);

            auto owes = false;

            for (const auto& runId : under)
            {
                if (! seen.insert (runId).second)
                    continue;

                auto* run = runs.find (runId);

                if (run == nullptr || run->restoreAddress.empty())
                    continue;

                const auto restore = osc::valuesFromAtoms (run->restoreAtom);
                const auto preSent = osc::valuesFromAtoms (run->preSentAtom);
                const auto* now = mounts != nullptr ? mounts->valueOf (run->restoreAddress) : nullptr;

                const auto forget = [run]
                {
                    run->restoreAddress.clear();
                    run->restoreAtom.clear();
                    run->preSentAtom.clear();
                };

                if (! restore.has_value())
                {
                    forget();
                    continue;
                }

                if (! preSent.has_value() || mounts == nullptr)
                {
                    engine.submit (origin::engine, "node.set", addressAnd (run->restoreAddress, *restore));
                    restoredThisTick.insert_or_assign (run->restoreAddress, *restore);
                    submitted = true;
                    forget();
                    continue;
                }

                if (now != nullptr && *now == *restore)
                {
                    forget();
                    continue;
                }

                if (now != nullptr && *now == *preSent)
                {
                    engine.submit (origin::engine, "node.set", addressAnd (run->restoreAddress, *restore));
                    restoredThisTick.insert_or_assign (run->restoreAddress, *restore);
                    submitted = true;
                    owes = true;
                    continue;
                }

                forget();
            }

            if (owes)
                stillOwed.push_back (blockId);
        }

        owedPutBacks = std::move (stillOwed);
        return submitted;
    }

    void Runner::submitRevocation (Engine& engine, const std::string& runId)
    {
        submitRestores (engine, runId);
        engine.submit (origin::engine, "run.revoke", one (runId));
    }

    void Runner::submitRestores (Engine& engine, const std::string& runId)
    {

        /*  WHAT WAS PRE-SENT GOES BACK FIRST, and it goes back as an ORDINARY
            WRITE.

            §13.1: anticipation is only as good as its revocation, and a
            revocation of a value on somebody else's desk is putting the old one
            there. `node.set` is how any client writes a mounted node, so this
            is that command with the value the target held before the horizon
            touched it - read before the write, kept on the run.

            An ordinary command rather than a private path, because a replay then
            reproduces the restore exactly as it reproduces every other write:
            the record is in the log with the value in it, and the mounted tree
            comes out the same with no network in the room.

            EVERY DESCENDANT, finished ones included: a pre-send whose wait was
            `none` finished the moment it had written, and it is exactly the
            one with something to put back.

            BEFORE the revocation, so that a client watching sees the desk put
            back and then the runs end, rather than a scene vanishing and a value
            changing afterwards for no visible reason. */
        for (const auto* run : runs.descendantsOf (runId))
        {
            if (run->restoreAddress.empty())
                continue;

            if (const auto value = osc::valuesFromAtoms (run->restoreAtom))
            {
                engine.submit (origin::engine, "node.set", addressAnd (run->restoreAddress, *value));

                /*  AND WHAT THE DESK WILL HOLD after this tick's give-backs
                    (2026-10-03, Doh! D3): read by the flush, which runs after
                    them and puts back what the GO wrote there from there. */
                restoredThisTick.insert_or_assign (run->restoreAddress, *value);
            }

            /*  ONCE (2026-10-01, namespace draft §24): a block given back inside
                one being given back - the next scene's, under an act Doh! is
                taking back - reaches this restore twice, from its own job and
                from the act's. The hook's own field, so a replay - which takes
                the restore from the log - is untouched. */
            if (auto* restored = runs.find (run->id))
            {
                restored->restoreAddress.clear();
                restored->restoreAtom.clear();
            }
        }

        /*  AND NOTHING OF THE BLOCK IS WRITTEN FROM HERE ON (2026-09-30,
            namespace draft §23.3). A pre-send still asking what the desk held
            can have its answer in this very tick - taken in the drain that
            stopped the scene - and `advanceSends`, which runs after this, read
            it and wrote the value out with the `run.revoke` still unapplied:
            the desk left holding what a scene that is not coming put there,
            with no restore asked for, the value to restore having been learned
            a moment too late. Every job of the block is settled here, by the
            hook that decided; the jobs are the hooks' own, so a replay is
            untouched. */
        for (auto& job : sending)
            if (inAncestryOf (runId, job.self))
                job.finished = true;
    }

    bool Runner::drivenByAJob (const Run& run) const
    {
        /*  WHAT A JOB STILL HAS IN HAND ends on that job's own record: a fade's
            or a stop cue's when it lands, a network cue's when its wait is
            over, a group's through its phases, a memo's on the tick after it
            fired. Anything else unfinished is ended only by being asked to. */
        if (run.isGroup())
            return std::any_of (scheduled.begin(), scheduled.end(),
                                [&run] (const GroupJob& job) { return job.run == run.id && ! job.retired; });

        return std::any_of (running.begin(), running.end(),
                            [&run] (const FadeJob& job) { return job.self == run.id && ! job.retired; })
            || std::any_of (sending.begin(), sending.end(),
                            [&run] (const OscJob& job) { return job.self == run.id && ! job.finished; })
            || std::find (finishing.begin(), finishing.end(), run.id) != finishing.end();
    }

    void Runner::endMember (Engine& engine, const Run& member, bool graceful)
    {
        /*  THE MEMBERS ARE ENDED THE WAY THE GROUP WAS (2026-09-30, namespace
            draft §23): stopped when it was stopped, killed only when it was
            killed. It used to kill them either way, so a scene inside a scene
            skipped its footer under Esc - the releasing §4.4 promises, lost one
            level down - and a mic cue inside one was cut dead, its reverb
            reset, where Esc lets it ring (CG, CN). The group waits for every
            member before its own footer, so the innermost comes down first.

            ONCE A MEMBER, NOT ONCE A TICK. A member already on its way out is
            left to finish - `stopping`. One holding its post-wait answered a
            second stop by starting the post-wait again, for ever; it is ended
            now, below, under a stop as under a kill. A kill
            passes over only a member that is stopping AND already killed, so a
            double Esc during Esc's teardown still reaches the members Esc had
            only stopped.

            AND ASKED AGAIN IF IT CAME BACK. A stop cue whose own run is ended
            hands its target back its level and `playing` (the rule for a fade
            killed on its own, in `advanceFades`), and a stop cue that is a
            member here is ended with the rest - so a member it was fading can
            be back to `playing` after this reached it, and has to be reached
            again, or it plays on to the end of its file with the scene waiting
            for it.

            A POST-WAIT A KILL FINDS IS OVER. The member's sound has ended and
            what it holds is the wait - its voice and its slots with it - and a
            kill asks nothing of the cue: `run.done`, the post-wait's own ending,
            lets all of it go on the spot. `run.kill` would only have written
            `stopping` over the wait.

            AND SO IS ONE A STOP FINDS (2026-10-02, K2, namespace draft §23.13;
            the author: "Esc ends them too"). Under Esc - or a stop cue, or the
            pane's stop aimed at the scene - a member that had just begun a
            thirty-second post-wait kept its voice, its slots and its scene for
            that half-minute, the footer waiting behind it. A stopping scene
            aborts its members - its `run.stop` owes no post-wait - so the one
            under way ends here the same way, with `run.done`. Its voice is not
            cut: a stop lets a tail ring, as Esc lets a mic cue's. The scene's
            OWN post-wait is another matter: brought down by a stop cue, it
            still spaces whatever follows it (JX).

            AND ITS VOICE IS KILLED AS WELL (2026-10-01, namespace draft §23.6,
            GD). It is the clip that is over, not the chain: the voice's EQ and
            inserts still ring what the file's last moments put into them, at
            the cue's level, and `run.done` reaches no Player - so the pane's
            kill of a scene left a member's reverb ringing until it died away,
            or until the next arm on that voice cleared it. Asked here, by the
            hook that decided, as `enforceStops` asks: while the run still holds
            the voice, and with no record, so a replay - which has no Player -
            is the same with it or without. */
        if (member.isFinished())
            return;

        if (member.state == runState::postWait)
        {
            if (! graceful && audio != nullptr && member.track >= 0)
                audio->kill (member.track);

            engine.submit (origin::engine, "run.done", one (member.id));
            return;
        }

        if (member.state == runState::stopping && (graceful || member.skipFooter))
            return;

        engine.submit (origin::engine, graceful ? "run.stop" : "run.kill", one (member.id));
    }

    bool Runner::endAskedForIn (Engine& engine, const std::string& blockRun, bool graceful)
    {
        /*  ONLY THE OUTERMOST OF WHAT WAS ASKED FOR: a group fired by name in
            there ends its own members, and runs its own footer when it was
            stopped, so what is under it is its job's. What holds the block back
            is anything asked for that has not finished, at any depth. */
        auto unfinished = false;

        for (const auto* under : runs.descendantsOf (blockRun))
        {
            if (under->isFinished() || ! under->prepare.empty())
                continue;

            unfinished = true;

            if (const auto* above = runs.find (under->parent);
                above != nullptr && above->id != blockRun && ! above->isFinished()
                  && above->prepare.empty())
                continue;

            endMember (engine, *under, graceful);
        }

        return unfinished;
    }

    bool Runner::underAKill (const Run& run) const
    {
        /*  BOUNDED BY THE TABLE, as `applyLevels`' walk up is: a `parent` that
            pointed at itself would otherwise hang the tick. */
        auto at = run.parent;

        for (std::size_t guard = 0; guard <= runs.all().size() && ! at.empty(); ++guard)
        {
            const auto* above = runs.find (at);

            if (above == nullptr)
                return false;

            if (above->skipFooter)
                return true;

            at = above->parent;
        }

        return false;
    }

    bool Runner::fadingToItsStop (const Run& group) const
    {
        /*  HELD WHILE ITS STOP IS STILL AHEAD. A `hard` stop is a fade of no
            length, due on the tick it was asked, and so never holds: the scene
            stops at once, as it always did. A fade that took over from a
            fade-and-stop inherits the stop's tick, so riding the level back up
            keeps the scene going to that tick, as it keeps a cue. */
        const auto held = std::any_of (running.begin(), running.end(),
                                       [this, &group] (const FadeJob& job)
                                       {
                                           return job.stopWhenDone && job.heldRun() == group.id
                                                    && currentTick < job.stopsAtTick;
                                       });

        if (! held)
            return false;

        /*  BUT AN ABORT DOES NOT WAIT FOR A FADE, on the group or anywhere
            above it. Esc, the pane's stop and the stop a stopping scene's job
            sends each member all mark what they stop (`stopEndsWait`, K2), a
            kill marks it `skipFooter`, a Doh `takenBack` - all handler-written.
            Above, because a stopping scene leaves a member already on its way
            out to finish (`endMember`): without the walk, the pane's stop on an
            act left a scene a stop cue was fading inside it playing on to the
            fade's end, the act's footer waiting behind it. (Esc reaches the
            stop cue's own run too, which lets go by itself.) Bounded by the
            table, as `underAKill` is; a finished parent ends the walk, as it
            ends `stopEveryRoot`'s (JV).

            AND NOR DOES A STOP ABOVE IT THAT HAS LANDED (2026-10-02, K3's
            review, KV): an authored stop - a `hard` stop cue on the act, or
            the act's own fade-and-stop reaching its end - makes the act
            `stopping` with no abort's mark, and its job passed over the scene
            inside it as one already on its way out, so a hard stop of the act
            waited for the inner scene's fade. An ancestor `stopping` and not
            itself held by a fade is one whose stop is being carried out: the
            scene inside it comes down now, with it. */
        const auto* at = &group;

        for (std::size_t guard = 0; at != nullptr && guard <= runs.all().size(); ++guard)
        {
            if (at->stopEndsWait || at->skipFooter || at->takenBack)
                return false;

            if (at != &group && at->state == runState::stopping && ! fadingToItsStop (*at))
                return false;

            if (at->parent.empty())
                break;

            const auto* above = runs.find (at->parent);

            if (above == nullptr || above->isFinished())
                break;

            at = above;
        }

        return true;
    }

    bool Runner::goingOut (const Run& run) const
    {
        /*  Bounded by the table, as `underAKill` is. */
        const auto* at = &run;

        for (std::size_t guard = 0; at != nullptr && guard <= runs.all().size(); ++guard)
        {
            if (at->isFinished())
                return false;

            if (at->state == runState::stopping)
                return true;

            at = at->parent.empty() ? nullptr : runs.find (at->parent);
        }

        return false;
    }

    bool Runner::beingKilled (const Run& run) const
    {
        /*  ITS OWN KILL STILL STANDING: `skipFooter`, which nothing clears, and
            the stop it asked, which a seek withdraws - a seek brings a killed
            cue back, playing where the hand put it, and it must launch. Both
            handler-written (§24.1), so a handler may decide by them. */
        return (run.skipFooter && run.stopAsked) || underAKill (run);
    }

    bool Runner::sparedByThePress (const std::string& runId) const
    {
        /*  UP TO THE ROOT, as `stopEveryRoot` finds one - no parent, or a parent
            gone or finished (since 2026-10-02, K2, namespace draft §23.13) -
            bounded by the table as `underAKill` is. An empty name - a client's
            `node.set`, a restore - is nobody's, and is not spared. */
        const auto* at = runs.find (runId);

        for (std::size_t guard = 0; at != nullptr && guard <= runs.all().size(); ++guard)
        {
            const auto* above = at->parent.empty() ? nullptr : runs.find (at->parent);

            if (above == nullptr || above->isFinished())
                return ! at->isFinished() && at->onlyPrepared() && ! runs.askedForUnder (at->id);

            at = above;
        }

        return false;
    }

    std::vector<std::string> Runner::armablesFor (const juce::ValueTree& cue) const
    {
        const auto element = cue.getType().toString();

        /*  ONLY A MEDIA CUE HAS ANYTHING TO MAKE READY. Asking to arm a memo
            would be a rejection every time the pointer passed over one, which
            would fill the log with a refusal about something nobody did wrong. */
        if (element == "Media" || element == "Mic")
        {
            /*  A SOUND LOCKED TO ITS MOVIE is the movie's to arm. */
            if (followsAMovie (cue))
                return {};

            const auto id = cue[idProperty].toString().toStdString();
            return id.empty() ? std::vector<std::string> {} : std::vector<std::string> { id };
        }

        /*  A MOVIE MAKES ITS OWN SOUNDS READY (namespace draft 37.5, WJ), so
            the GO that brings the picture up starts them on its sample. */
        if (element == "Video")
            return textOf (cue, "source") == "movie" ? soundsLockedTo (cue[idProperty].toString().toStdString())
                                                     : std::vector<std::string> {};

        if (element != "Group")
            return {};

        /*  A SAMPLER GROUP LAUNCHES NOTHING FIRST: GO arms every member onto
            its strip at once, and a member armed ahead without a strip would be
            a voice held for a cue no hand could press. */
        if (textOf (cue, "mode") == "sampler")
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
            if (! runsNow (child))
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

        /*  WHAT THIS TICK'S GIVE-BACKS PUT BACK, from nothing (Doh! D3). */
        restoredThisTick.clear();

        /*  THE LENGTHS FOR THIS TICK, held so nothing swaps them mid-solve. A
            file imported since the show opened has no length until the
            analyser reads it, and learning one swaps the map - so it is asked
            for here rather than remembered by address. A replay, and a test,
            hand in a map of their own and keep it. */
        if (fixedDurations == nullptr && mediaInfo != nullptr)
        {
            durationsHeld = mediaInfo->durations();
            durations = durationsHeld.get();
        }

        /*  WHAT THE START CUES ASKED FOR, fired by name now: the hook decides,
            the handler applies, and the record is `cue.fire`'s own. */
        /*  AND THE GO EACH FIRES UNDER (2026-10-01, namespace draft §24): the
            start cue's own, carried in the record as its third argument, so a
            replay reads it and a Doh of that GO drops a fire still queued. */
        for (const auto& [target, cause] : startsToFire)
        {
            if (cause == 0)
                engine.submit (origin::engine, "cue.fire", one (target));
            else
                engine.submit (origin::engine, "cue.fire",
                               { osc::Value::string (target), osc::Value::string (std::string {}),
                                 osc::Value::int64 (static_cast<std::int64_t> (cause)) });
        }

        startsToFire.clear();

        /*  AND THE JUMP CUES' MOVES (namespace draft §27), the same road: a
            record the session logs and a replay takes, carrying the GO the jump
            cue fired under, so its "and Go" is that GO's to take back. */
        for (const auto& jump : jumpsToMake)
            engine.submit (origin::engine, "standby.jump",
                           { osc::Value::string (jump.list), osc::Value::string (jump.target),
                             osc::Value::boolean (jump.andGo),
                             osc::Value::int64 (static_cast<std::int64_t> (jump.cause)) });

        jumpsToMake.clear();

        /*  ABOVE THE NULL-PLAYER GATE, all three of them, and that is not an
            ordering detail. `wfg serve` without `--hosted` has no Player at all
            and must still run a show made of memos, network cues and fades -
            which is the configuration a designer works in on a train, and the
            configuration every replay is in. A wait that only elapsed when
            there was a sound card would be a cue list that only worked in a
            theatre. */
        advanceWaits (engine, tick);
        advanceGroups (engine);
        mirrorSeekable();
        mirrorRound();
        samplerEdges (engine);
        releaseSolos();
        armStandby (engine);

        /*  WHAT A DOH PUTS BACK ON THE DESKS, what a jump sends there, a scene
            put back once it has ended, and the report (2026-10-03, Doh! D3,
            namespace draft §24.13): after every give-back of this tick - a
            stopped block's own, in `advanceGroups`, and the standby's - whose
            restores are already submitted, so what this sends lands after
            theirs. Above the gate below: a `serve` with no audio side puts
            values back too. */
        flushDohWrites (engine);

        advanceFades (engine, tick);
        applyLanes();
        recordLane (engine);
        applyLevels();
        applyOutputLevels();
        applyRouting();
        applyEq();
        applyFx();
        recordCurves (engine, tick);
        advanceCurves (engine, tick);
        advanceSends (engine);
        observeAfterStep (engine, tick);

        /*  THE PLAYHEADS AN ESC PAUSED, read before the pass that may carry
            them on and before `updatePositions` below moves the readout on
            (K8's review): what it holds now is the press's own tick's. And the
            ones a Doh paused, for the next GO to carry on (D2). */
        notePausedPlayheads();
        noteDohPlayheads (engine);

        /*  WHAT A PRESS OF DOH! WOULD FORGET NOW (D2's review, MY): a
            readout, written here because it turns on the clock - the live
            record's window running out - and read by the button. The press's
            own handler decides from its own state. */
        {
            ListState::DohForget forget;

            if (const auto found = marks.find (lastDohList);
                found != marks.end() && found->second.root.has_value()
                  && (goRecord.serial == 0 || dohTooLate (tick)))
                forget = { lastDohList, found->second.cue };

            lists.setDohForget (forget);
        }
        assertPersistent (engine, tick);

        /*  Above the gate too: with no player there are no inputs, and the
            meters say so rather than holding the last numbers a player left. */
        takeInputMeters();

        /*  And the pictures, which a show with no player still has. */
        advanceVideo (engine, tick);

        if (audio == nullptr)
            return;

        armWaitingMics (engine);
        serviceTakes (engine);
        launchIfDue (engine, tick);

        /*  A RUN CARRIED ON MID-FILE COMES UP FROM SILENCE, from the tick its
            launch was placed (K8's review; §24, GQ). */
        deClickLaunched();

        /*  AFTER THE LAUNCH AND BEFORE THE RANGES: a launch starts its run's
            clock, and a range's boundary is read off it (§22.4). */
        applyRates();

        /*  AFTER THE SPEED AND BEFORE THE BOUNDARIES (namespace draft §33): a
            slice's in or out moved while it sounds moves the slice's clock,
            which the boundary is read off, and is placed on the run's clock
            as it stands after this tick's speed. */
        applySlices (engine, tick);
        advanceRanges (engine);
        endTurnedRuns();

        /*  AFTER THE RANGES AND BEFORE THE EDGES, which is the only place it
            can go: it reads the range bookkeeping `advanceRanges` has just
            moved, and a run that `observeEdges` finishes below keeps the
            playhead it had while it was still sounding rather than a nought
            written after the sound stopped. */
        updatePositions (tick);
        enforceStops();
        observeEdges (engine);
    }

    std::int64_t Runner::videoClockNow() const noexcept
    {
        if (samplesPerTick <= 0)
            return -1;

        return audio != nullptr ? audio->samplesElapsed()
                                : currentTick * static_cast<std::int64_t> (samplesPerTick);
    }

    int Runner::videoSampleRate() const noexcept
    {
        return samplesPerTick * TickClock::rateHz;
    }

    std::int64_t Runner::videoSampleAhead() const noexcept
    {
        /*  THE AUDIO'S CLOCK WHEN THERE IS ONE, which is the clock the picture
            is presented against (PRD §3.19d); the tick's own when the show has
            no player, which is the clock the renderer is then told. */
        const auto now = videoClockNow();

        if (now < 0)
            return -1;

        return now + static_cast<std::int64_t> (videoLeadTicks()) * samplesPerTick;
    }

    int Runner::videoLeadTicks() const noexcept
    {
        /*  A LAUNCH HORIZON, as a clip's launch is placed - and two ticks with
            no player, which is the time the region takes to reach the
            renderer and the renderer a frame. */
        return std::max (latencyTicks(), 2);
    }

    std::int64_t Runner::videoSamplesFor (double seconds) const noexcept
    {
        if (! (seconds > 0.0) || samplesPerTick <= 0)
            return 0;

        return std::llround (seconds * static_cast<double> (samplesPerTick)
                               * static_cast<double> (TickClock::rateHz));
    }

    void Runner::fadeOutVideo (const std::string& runId, std::int64_t tick, int ticks)
    {
        /*  THE JOB OWNS THE RUN'S ENDING until the picture is black - its
            fade-out a horizon ahead, as every point is - and the hook places
            the points. */
        for (auto& job : showing)
            if (job.self == runId && ! job.removed)
            {
                job.fadeOutTicks = std::max (0, ticks);
                job.endsAtTick = tick + std::max (0, ticks) + videoLeadTicks();
            }
    }

    void Runner::prepareStandbyPictures()
    {
        /*  THE STANDBY'S PICTURE, READ BEFORE GO (VX): every list's standby
            that is a picture cue, as whole paths - a short list, said again
            only when it changed. */
        std::vector<std::string> standbys;

        for (const auto& list : document.root().getChildWithName ("Lists"))
            if (const auto listId = list[idProperty].toString().toStdString(); ! listId.empty())
                standbys.push_back (document.getAttribute ("/godot/list/" + listId + "/standby").value_or (std::string {}));

        /*  ONLY WHEN SOMETHING MOVED - a standby, or the show - since finding a
            cue walks the show, and this is the tick thread. */
        if (standbys == standbysPrepared && document.revision() == revisionPrepared)
            return;

        standbysPrepared = standbys;
        revisionPrepared = document.revision();

        std::vector<std::string> wanted;

        for (const auto& standby : standbys)
        {
            if (standby.empty())
                continue;

            const auto cue = document.findById (standby);

            if (! cue.isValid() || ! cue.hasType ("Video") || textOf (cue, "source") != "picture")
                continue;

            if (const auto path = mediaPathOf (textOf (cue, "file")); ! path.empty())
                wanted.push_back (path);
        }

        if (wanted != picturesPrepared)
        {
            picturesPrepared = wanted;

            if (videoSink != nullptr)
                videoSink->prepare (wanted);
        }
    }

    void Runner::followCanvasLevels()
    {
        if (videoSink == nullptr)
            return;

        if (! canvasLevelsRead || canvasLevelsRevision != document.showRevision())
        {
            canvasLevelSources.clear();
            canvasLevelsRead = true;
            canvasLevelsRevision = document.showRevision();

            for (const auto& canvas : document.root().getChildWithName ("Canvases"))
            {
                CanvasLevelSource source;
                source.id = canvas[idProperty].toString().toStdString();
                source.level = std::clamp (static_cast<double> (canvas.getProperty ("level", 100.0)) / 100.0, 0.0, 1.0);
                source.dcaChain = dcaNestingFrom (canvas.getProperty ("dca").toString().toStdString());

                if (! source.id.empty())
                    canvasLevelSources.push_back (std::move (source));
            }
        }

        std::vector<std::pair<std::string, double>> levels;
        levels.reserve (canvasLevelSources.size());

        for (const auto& source : canvasLevelSources)
        {
            auto trim = 0.0;

            if (dcas != nullptr)
                for (const auto& dcaId : source.dcaChain)
                    trim += dcas->trimOf (dcaId);

            levels.emplace_back (source.id, source.level * (source.dcaChain.empty() ? 1.0 : video::opacityForTrim (trim)));
        }

        /*  ON THE TICK ONE MOVES, and on the first: what the video side holds
            is the scene the engine holds (VC), and a level is part of it. */
        if (levels != canvasLevelsSent)
        {
            videoSink->canvasLevels (levels);
            canvasLevelsSent = std::move (levels);
        }
    }

    void Runner::advanceVideo (Engine& engine, std::int64_t tick)
    {
        juce::ignoreUnused (tick);

        prepareStandbyPictures();
        followCanvasLevels();

        for (auto& job : showing)
        {
            const auto* run = runs.find (job.self);

            /*  ENDED, HOWEVER IT ENDED - a stop cue, a kill, Doh! taking it
                back, Esc's fade run out - and the layer goes now, unless a
                fade-out already placed its going. */
            if (run == nullptr || run->isFinished())
            {
                if (job.placed && ! job.removed && videoSink != nullptr)
                    videoSink->remove (job.self, -1);

                job.removed = true;

                if (job.movie)
                    endLockedSounds (engine, job.self);

                continue;
            }

            /*  STOPPING - a stop cue, a hard stop - and its sounds with it, on
                the tick the movie was asked. Esc's own fade reaches them as it
                reaches every sound. */
            if (job.movie && run->state == runState::stopping && job.fadeOutTicks < 0 && job.endsAtTick < 0)
                endLockedSounds (engine, job.self);

            /*  TAKEN BACK BEFORE IT CAME UP - Doh! in the tick it was fired -
                and never seen at all. */
            if (! job.placed && run->takenBack)
            {
                job.removed = true;
                continue;
            }

            /*  UP, A HORIZON AHEAD: from nothing to the cue's opacity over its
                fade-in, or straight there. Reported started on the tick it is
                placed, as a clip's launch is - the record a replay reads. */
            if (! job.placed)
            {
                const auto at = videoSampleAhead();
                const auto rise = videoSamplesFor (job.fadeInSeconds);

                job.points.clear();

                if (rise > 0 && at >= 0)
                {
                    job.points.push_back ({ at, 0.0 });
                    job.points.push_back ({ at + rise, job.opacity });
                }
                else
                {
                    job.points.push_back ({ at, job.opacity });
                }

                if (videoSink != nullptr)
                {
                    videoSink->show (job.spec);

                    for (const auto& point : job.points)
                        videoSink->opacity (job.self, point);
                }

                /*  AND WHAT THE DCAS LEAVE OF IT, from the sample it comes up
                    on (namespace draft 37.5, WE). */
                job.dcaFactor = videoDcaFactorOf (*run);

                if (job.dcaFactor < 1.0)
                    placeVideoPoint (job, video::Property::dca, { at, job.dcaFactor });

                /*  A MOVIE STARTS WHERE ITS OFFSET SAYS, on the sample it comes
                    up on. */
                if (job.movie)
                {
                    job.movieAt = at;
                    placeVideoPoint (job, video::Property::time, { at, job.moviePosition });
                }

                job.placed = true;
                engine.submit (origin::engine, "run.started", one (job.self));
            }

            /*  A MOVIE PLAYS ON, through a fade-out too, until its run ends. */
            if (job.movie && job.placed && ! job.movieEnded)
                advanceMovie (engine, job);

            /*  A DCA RIDDEN WHILE IT IS UP, through Esc's fade-out too. */
            if (job.placed && ! job.removed)
                followVideoDcas (job, *run);

            /*  DOWN, AS ESC ASKED: from where it is at the horizon to black over
                the panic fade, and gone on the frame it gets there. */
            if (job.fadeOutTicks >= 0 && ! job.removed)
            {
                const auto at = videoSampleAhead();
                const auto from = at >= 0 ? video::opacityAt (job.points, at) : job.opacity;
                const auto end = at >= 0 ? at + static_cast<std::int64_t> (job.fadeOutTicks) * samplesPerTick
                                         : -1;

                job.points.push_back ({ at, from });
                job.points.push_back ({ end, 0.0 });

                if (videoSink != nullptr)
                {
                    videoSink->opacity (job.self, job.points[job.points.size() - 2]);
                    videoSink->opacity (job.self, job.points.back());
                    videoSink->remove (job.self, end);
                }

                job.fadeOutTicks = -1;
                job.removed = true;
            }
        }

        advanceVideoFades (engine, tick);

        std::erase_if (showing, [this] (const VideoJob& job)
                       {
                           const auto* run = runs.find (job.self);
                           return job.removed && (run == nullptr || run->isFinished());
                       });
    }

    double Runner::videoValueOf (const VideoJob& job, video::Property property, std::int64_t sample) const noexcept
    {
        if (property == video::Property::opacity)
            return video::opacityAt (job.points, sample);

        const auto base = property == video::Property::scale    ? job.spec.scale
                        : property == video::Property::offsetX  ? job.spec.offsetX
                        : property == video::Property::offsetY  ? job.spec.offsetY
                        : property == video::Property::rotation ? job.spec.rotation
                        : property == video::Property::dca      ? 1.0
                                                                : 0.0;

        return video::valueAt (job.moved[static_cast<std::size_t> (property)], sample, base,
                               [] (const video::Point&) { return true; });
    }

    void Runner::placeVideoPoint (VideoJob& job, video::Property property, const video::Point& point)
    {
        auto& points = property == video::Property::opacity ? job.points
                                                            : job.moved[static_cast<std::size_t> (property)];

        /*  KEPT SHORT: a long fade places fifty a second, and only the last
            few say where the value is now. */
        if (points.size() > 256)
            points.erase (points.begin(), points.begin() + 128);

        points.push_back (point);

        if (videoSink != nullptr)
            videoSink->move (job.self, property, point);
    }

    double Runner::videoDcaFactorOf (const Run& run)
    {
        if (dcas == nullptr)
            return 1.0;

        /*  THE DCA TERMS ALONE: a group's level and a strip's hand are
            decibels of sound, and stay with the sound (37.5, proposed). */
        const auto termsOf = [this] (const std::string& cueId)
        {
            auto total = 0.0;

            for (const auto& dcaId : dcaChainOf (cueId))
                total += dcas->trimOf (dcaId);

            return total;
        };

        auto total = termsOf (run.cue);
        auto parent = run.parent;

        /*  BOUNDED BY THE TABLE, as the level's walk is. */
        for (std::size_t guard = 0; guard <= runs.all().size() && ! parent.empty(); ++guard)
        {
            const auto* above = runs.find (parent);

            if (above == nullptr)
                break;

            total += termsOf (above->cue);
            parent = above->parent;
        }

        return video::opacityForTrim (total);
    }

    void Runner::followVideoDcas (VideoJob& job, const Run& run)
    {
        const auto factor = videoDcaFactorOf (run);

        if (std::abs (factor - job.dcaFactor) < 1e-6)
            return;

        const auto at = videoSampleAhead();
        const auto& placed = job.moved[static_cast<std::size_t> (video::Property::dca)];

        if (at >= 0)
        {
            const auto heldFrom = at - samplesPerTick;

            if (placed.empty() || placed.back().sample < heldFrom)
                placeVideoPoint (job, video::Property::dca, { heldFrom, job.dcaFactor });
        }

        placeVideoPoint (job, video::Property::dca, { at, factor });
        job.dcaFactor = factor;
    }

    bool Runner::fireVideoFade (const juce::ValueTree& fade, const std::string& runId, std::int64_t tick)
    {
        const auto targetCue = document.findById (textOf (fade, "target"));

        if (! targetCue.isValid() || ! targetCue.hasType ("Video"))
            return false;

        if (auto* selfRun = runs.find (runId))
            selfRun->state = runState::playing;

        /*  WHAT IT MOVES AND WHERE TO, in the rows' own units - the opacity in
            % (VR), which the picture side takes as 0..1. An entry it does not
            know, or a number that will not read, is passed over. */
        VideoFade job;
        job.self = runId;
        job.startTick = tick;
        job.ticks = ticksFor (numberOf (fade, "duration"));
        job.sCurve = fadeCurveFrom (textOf (fade, "curve")) == FadeCurve::sCurve;
        job.stopWhenDone = textOf (fade, "stopWhenDone") == "true";

        for (const auto& word : juce::StringArray::fromTokens (juce::String (textOf (fade, "video")), " ", ""))
        {
            const auto name = word.upToFirstOccurrenceOf (":", false, false).toStdString();
            const auto value = osc::parseDouble (word.fromFirstOccurrenceOf (":", false, false).toStdString());

            if (! value.has_value())
                continue;

            if (name == "rate")
            {
                job.movesRate = true;
                job.rateTo = std::clamp (*value, -20.0, 20.0);
            }
            else if (name == "opacity")  job.to.push_back ({ video::Property::opacity, std::clamp (*value / 100.0, 0.0, 1.0) });
            else if (name == "scale")    job.to.push_back ({ video::Property::scale, *value });
            else if (name == "offsetX")  job.to.push_back ({ video::Property::offsetX, *value });
            else if (name == "offsetY")  job.to.push_back ({ video::Property::offsetY, *value });
            else if (name == "rotation") job.to.push_back ({ video::Property::rotation, *value });
        }

        /*  THE RUN IT MOVES is the target cue's live one, found when the fade
            fires, as a sound's fade finds its run. None, or nothing to move,
            and the fade ends at once - the shape of a sound's fade that moves
            nothing. */
        const auto* live = runs.liveRunOf (targetCue[idProperty].toString().toStdString());

        if (live == nullptr || (job.to.empty() && ! job.movesRate))
        {
            FadeJob nothing;
            nothing.self = runId;
            running.push_back (nothing);
            return true;
        }

        job.target = live->id;
        videoFades.push_back (std::move (job));
        return true;
    }

    void Runner::advanceMovie (Engine& engine, VideoJob& job)
    {
        const auto at = videoSampleAhead();
        const auto rate = videoSampleRate();

        if (at < 0 || rate <= 0 || at <= job.movieAt)
            return;

        /*  ITS LENGTH, AS THE SHOW KNOWS IT: read when the show opened, or by
            the analyser since. Not known yet, and it plays on unwrapped. */
        double duration = 0.0;

        if (const auto* known = handlerDurations())
            if (const auto found = known->find (job.movieFile); found != known->end())
                duration = found->second;

        /*  THE WAY IT GOES (namespace draft §41): the speed's sign, turned
            again while a ping-pong range plays back from its out-point. */
        const auto velocity = job.rate * static_cast<double> (job.bounce);
        auto target = job.moviePosition + velocity * static_cast<double> (at - job.movieAt) / static_cast<double> (rate);

        /*  ITS RANGES AS THEY ARE NOW (WL, TZ): a range moved while it plays
            is heard at once, one taken away finishes where the playhead is. */
        const auto ranges = rangesOf (document.findById (job.cue));

        const auto indexOf = [&ranges] (const std::string& id)
        {
            for (std::size_t n = 0; n < ranges.size(); ++n)
                if (ranges[n].id == id)
                    return static_cast<int> (n);

            return -1;
        };

        /*  WHERE THIS STRETCH ENDS, the way it goes: the range's out point
            forwards, never past the file, and its in point backwards; with no
            range, the file's end forwards, its start offset backwards. Below
            nought when unknown. */
        const auto boundOf = [&ranges, &indexOf, &job, duration] (bool forwards)
        {
            if (job.rangeId.empty())
                return forwards ? (duration > 0.0 ? duration : -1.0) : job.pieceStart;

            const auto index = indexOf (job.rangeId);

            if (index < 0)
                return forwards ? job.moviePosition : job.moviePosition;

            const auto& playing = ranges[static_cast<std::size_t> (index)];

            if (! forwards)
                return playing.in;

            return duration > 0.0 ? std::min (playing.out, duration) : playing.out;
        };

        for (int guard = 0; guard < 10000; ++guard)
        {
            const auto way = job.rate * static_cast<double> (job.bounce);

            if (! (std::abs (way) > 0.0))
                break;

            const auto forwards = way > 0.0;
            const auto end = boundOf (forwards);

            if (! (end >= 0.0) || (forwards ? target < end : target > end))
                break;

            /*  THE SAMPLE THIS STRETCH ENDS ON, between the last point and
                this one. */
            const auto reaches = job.movieAt + static_cast<std::int64_t> (std::llround (std::max (0.0, std::abs (end - job.moviePosition))
                                                                                         / std::abs (way) * static_cast<double> (rate)));

            const auto index = job.rangeId.empty() ? -1 : indexOf (job.rangeId);
            const auto* playing = index >= 0 ? &ranges[static_cast<std::size_t> (index)] : nullptr;
            const auto another = playing != nullptr && (playing->loops == 0 || job.pass + 1 < playing->loops);

            /*  A BOUNCE (§41): the playhead turns at the point, a pass each way,
                with no step - the file runs on the other way from there. */
            if (playing != nullptr && playing->pingPong && another)
            {
                ++job.pass;
                job.bounce = -job.bounce;
                placeVideoPoint (job, video::Property::time, { reaches, end });
                target = end - (target - end);
                job.moviePosition = end;
                job.movieAt = reaches;
                continue;
            }

            /*  WHERE IT GOES NEXT: this range again for another pass, the next
                range for the first of its own, or nowhere - entered from its in
                point forwards, from its out point backwards. */
            std::optional<double> next;

            if (another && playing != nullptr)
            {
                ++job.pass;
                next = forwards ? playing->in : playing->out;
            }
            else if (index >= 0 && static_cast<std::size_t> (index) + 1 < ranges.size())
            {
                const auto& following = ranges[static_cast<std::size_t> (index) + 1];
                job.rangeId = following.id;
                job.pass = 0;
                job.bounce = 1;
                next = job.rate < 0.0 ? following.out : following.in;
            }

            if (next.has_value())
            {
                //  A STEP to where it goes, on that sample.
                const auto overshoot = target - end;
                placeVideoPoint (job, video::Property::time, { reaches, end });
                placeVideoPoint (job, video::Property::time, { reaches, *next });
                job.moviePosition = *next;
                job.movieAt = reaches;

                const auto nowWay = job.rate * static_cast<double> (job.bounce);
                target = *next + (nowWay > 0.0 ? std::abs (overshoot) : -std::abs (overshoot));
                continue;
            }

            /*  THE LAST PASS DONE (WB): the last frame held a tick, then the
                layer goes and the run ends - its footers run, as a sound's do
                when its file ends. */
            placeVideoPoint (job, video::Property::time, { reaches, end });
            job.moviePosition = end;
            job.movieAt = reaches;
            job.movieEnded = true;

            if (! job.removed)
            {
                if (videoSink != nullptr)
                    videoSink->remove (job.self, reaches + samplesPerTick);

                job.removed = true;
            }

            engine.submit (origin::engine, "run.ended", one (job.self));
            return;
        }

        placeVideoPoint (job, video::Property::time, { at, target });
        job.moviePosition = target;
        job.movieAt = at;
    }

    void Runner::advanceVideoFades (Engine& engine, std::int64_t tick)
    {
        for (auto& fade : videoFades)
        {
            if (fade.done)
                continue;

            const auto* self = runs.find (fade.self);

            /*  THE FADE'S OWN RUN STOPPED - Esc, a kill, Doh! - and the values
                stay where the fade had taken them. */
            if (self == nullptr || self->isFinished())
            {
                fade.done = true;
                continue;
            }

            auto job = std::find_if (showing.begin(), showing.end(),
                                     [&fade] (const VideoJob& held) { return held.self == fade.target; });
            auto* target = runs.find (fade.target);

            if (job == showing.end() || job->removed || target == nullptr || target->isFinished())
            {
                engine.submit (origin::engine, "run.ended", one (fade.self));
                fade.done = true;
                continue;
            }

            /*  A LAYER STILL TO COME UP is waited for: its first points are
                placed by `advanceVideo` before this. */
            if (! job->placed)
                continue;

            const auto at = videoSampleAhead();

            if (! fade.begun)
            {
                for (const auto& [property, value] : fade.to)
                    fade.from[static_cast<std::size_t> (property)] = videoValueOf (*job, property, at);

                fade.rateFrom = job->rate;
                fade.begun = true;
            }

            const auto progress = fade.ticks <= 0 ? 1.0
                                                  : std::clamp (static_cast<double> (tick - fade.startTick)
                                                                  / static_cast<double> (fade.ticks), 0.0, 1.0);

            for (const auto& [property, value] : fade.to)
                placeVideoPoint (*job, property, { at, moveValueAt (fade.from[static_cast<std::size_t> (property)],
                                                                   value, progress, fade.sCurve, MoveDomain::linear) });

            if (fade.movesRate)
                job->rate = moveValueAt (fade.rateFrom, fade.rateTo, progress, fade.sCurve, MoveDomain::linear);

            if (progress < 1.0)
                continue;

            /*  ARRIVED. Stopping, if it says so, takes the picture away where it
                arrived, and stops its run - its footers run then. */
            if (fade.stopWhenDone)
            {
                if (videoSink != nullptr)
                    videoSink->remove (job->self, at);

                job->removed = true;
                target->askStop (0);
            }

            engine.submit (origin::engine, "run.ended", one (fade.self));
            fade.done = true;
        }

        std::erase_if (videoFades, [] (const VideoFade& fade) { return fade.done; });
    }

    void Runner::takeInputMeters()
    {
        /*  THE SOUNDCHECK'S METER (Phase 9b, namespace draft §18.2): each
            logical input's loudest sample over the last tick, whether or not
            anything listens, TAKEN so each tick reads its own twenty
            milliseconds - the output meter's rule. Resized here on the tick
            thread when the interface changes width, never on the audio one. */
        const auto count = audio != nullptr ? std::max (0, audio->inputCount()) : 0;

        inputMeters.resize (static_cast<std::size_t> (count), Run::silentDb);

        for (int channel = 0; channel < count; ++channel)
        {
            const auto peak = static_cast<double> (audio->takeInputPeak (channel));

            inputMeters[static_cast<std::size_t> (channel)]
                = peak > 0.0 ? std::max (Run::silentDb, 20.0 * std::log10 (peak)) : Run::silentDb;
        }
    }

    void Runner::launchIfDue (Engine& engine, std::int64_t tick)
    {
        juce::ignoreUnused (tick);

        const auto ticksAhead = latencyTicks();

        if (ticksAhead <= 0 || samplesPerTick <= 0)
            return;

        const auto now = audio->samplesElapsed();
        const auto blockSize = static_cast<std::int64_t> (audio->blockSize());

        /*  ONCE PER TICK AT MOST, for the patch-settled record below. The row
            it reads only turns true when the command it submits is applied,
            which is the next tick - so a scene launching eight media cues in
            this one would otherwise write eight identical records into the
            log. They are harmless and idempotent, and a log a person reads
            should not make them wonder why. */
        auto saidSettled = false;

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || ! run->launchRequested || run->isFinished())
                continue;

            /*  NOR ONE A KILL HAS REACHED (2026-10-02, H4, namespace draft
                §23.10). The disk's answer is a record, and it can drain ahead
                of the press in the press's own tick: the run is ready in the
                drain that kills it, and this loop - before `enforceStops` cuts
                the voice - launched it, placed its speed and reported it
                started, a cue the press had dropped. */
            if (beingKilled (*run))
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

            /*  A MIC CUE'S LAUNCH IS ITS GATE (Phase 9b): opened at the same
                instant a media cue's clip would start, over its fade-in. */
            const auto launched = run->kind == "mic" ? audio->openLive (run->track, target, run->fadeIn)
                                                     : audio->launchAtSample (run->track, slot, target);

            if (launched)
            {
                run->launchRequested = false;
                run->launchedAtSample = target;

                /*  THE SPEED FROM THE LAUNCH (namespace draft §22.4): the run's
                    clock starts on the launch's sample at the run's speed, and
                    the voice is told the same breakpoint, so the two begin
                    together. A voice at one told one stays the identity, and a
                    cue at one plays exactly as it did before there was a speed. */
                if (run->kind == "media")
                {
                    /*  THE CLOCK COUNTS THE SPEED'S SIZE (namespace draft §41):
                        the reader goes on forwards whichever way the file goes,
                        and the way is `direction`. */
                    run->rateClock.start (static_cast<double> (target), std::abs (run->ownRate));
                    run->launchSource = static_cast<double> (target);
                    run->rangeSource = run->launchSource;
                    run->ratePlaced = run->ownRate;
                    run->rateNow = run->ownRate;
                    audio->placeRate (run->track, target, std::abs (run->ownRate));

                    /*  A CUE WITH NO RANGES plays between its start offset and
                        its file's end; launched backwards, from that end - its
                        piece the other way, which is what the waveform shows
                        it would sound like turned round. */
                    run->pieceStart = run->positionOrigin;
                    run->pieceEnd = run->positionOrigin;

                    if (durations != nullptr)
                        if (const auto length = durations->find (textOf (document.findById (run->cue), "file"));
                            length != durations->end() && length->second > run->positionOrigin)
                            run->pieceEnd = length->second;

                    /*  A FILE WHOSE LENGTH THE SHOW DOES NOT KNOW has no end to
                        start from: it plays forwards, and turns when its speed
                        crosses nought. */
                    if (run->direction < 0 && ! (run->pieceEnd > run->pieceStart)
                          && rangesOf (document.findById (run->cue)).empty())
                        run->direction = 1;

                    if (run->direction < 0 && rangesOf (document.findById (run->cue)).empty())
                    {
                        run->turned = true;
                        run->bent = true;
                        run->turnSource = run->launchSource;
                        run->turnFile = run->pieceEnd;
                        audio->endsOutside (run->track, true);
                        audio->placeLoop (run->track, 0, { run->positionOrigin, std::max (run->pieceStart, run->pieceEnd - 1.0 / static_cast<double> (audio->sampleRate())),
                                                           0.0, 0.0, 0.0, -1, false });
                    }
                }

                engine.submit (origin::engine, "run.started", one (run->id));

                /*  AND THE SHOW HAS NOW BEEN HEARD, which is what stops the
                    output patch following the output list (PRD §6.2,
                    `document/OutputLayout.h`).

                    Until something plays, the interface patch is empty and the
                    outputs simply follow the order somebody is arranging, so
                    adding a stereo mix at the top of the list moves everything
                    below it and that is exactly what the designer wants. The
                    moment a cue has come out of a speaker, what each output is
                    plugged into has become a fact about the building: a rig
                    that was sound-checked on Tuesday must not be re-patched by
                    an edit to the list on Wednesday. So the first launch says
                    so, once, and every later edit moves rows instead.

                    A `state` row, so it costs no undo entry, does not mark the
                    show unsaved, and is allowed while the show is locked -
                    which it will be, because a locked show is exactly the one
                    that is being played. Submitted rather than written, so the
                    log carries it and `wfg replay` reproduces it from the
                    record rather than from a launch a replay never performs.

                    Asked before it is sent: the row reads false only once in
                    the life of a show, so this is one string compare per launch
                    and nothing at all afterwards. */
                if (! saidSettled && document.getAttribute ("/godot/audio/patchSettled") == "false")
                {
                    engine.submit (origin::engine, "node.set",
                                   { osc::Value::string ("/godot/audio/patchSettled"),
                                     osc::Value::boolean (true) });
                    saidSettled = true;
                }

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

                    /*  THE POINTS THE SLOT HOLDS, which are the arm's and can
                        be behind the document by an edit the arm has not caught
                        up with - `applySlices` moves them once the launch is
                        crossed (namespace draft §33). The loop count is the
                        document's, as it is at every boundary (decision L). */
                    auto slice = ranges[at];

                    if (at < run->playingSlices.size())
                    {
                        slice.in = run->playingSlices[at].in;
                        slice.out = run->playingSlices[at].out;
                    }

                    run->rangeStartedAtSample = target;
                    run->positionOrigin = slice.in;   // and the playhead starts at its in-point
                    run->passesWanted = slice.loops;
                    run->passSamples = samplesForRange (slice, audio->sampleRate());
                    run->boundaryPlacedAt = -1;
                    run->rangesFinished = false;
                    run->firstPassSamples = 0;
                    run->passesBefore = 0;
                    run->loopMove = 0;
                    run->readerAtSliceStart = at < run->armedSlices.size() ? run->armedSlices[at].in : slice.in;
                    run->soundingSlice = slice.id;

                    /*  BACKWARDS OR BOUNCING FROM ITS FIRST FRAME (§41): the slot
                        told before it launches. */
                    if (at < run->playingSlices.size()
                          && (run->direction < 0 || run->playingSlices[at].pingPong))
                        anchorSlice (*run, static_cast<int> (at), static_cast<double> (target),
                                     run->readerAtSliceStart, run->playingSlices[at]);

                    /*  A BED CARRIED ON INSIDE ITS SLICE (K8's review): the clip
                        was armed that far into its loop, and the slice's start
                        is dated back by all it had played, passes and all - so
                        the pass count, the playhead, the lane and the boundary
                        read on from the pass it was in, by the arithmetic every
                        slice uses. In samples of the file, which are the
                        clock's at the launch. A session younger than that -
                        the device rebuilt since - keeps the point and starts
                        the passes again; younger still, the in-point's count. */
                    if (run->sliceFrom > 0.0 && run->passSamples > 0
                          && static_cast<int> (at) == run->startRange)
                    {
                        const auto rate = static_cast<double> (audio->sampleRate());
                        auto back = static_cast<std::int64_t> (std::llround (run->sliceFrom * rate));

                        if (target - back < 1)
                            back = static_cast<std::int64_t> (std::llround (
                                       std::fmod (run->sliceFrom, static_cast<double> (run->passSamples) / rate) * rate));

                        if (target - back >= 1)
                        {
                            run->rangeStartedAtSample = target - back;
                            run->rangeSource = run->launchSource - static_cast<double> (back);

                            /*  The slot's reader starts part-way into the loop,
                                at the in-point and the arm's offset, which is
                                not where the dated-back start would put it. */
                            if (const auto length = slice.out - slice.in; length > 0.0)
                                run->readerAtSliceStart += std::fmod (run->sliceFrom, length)
                                                             - static_cast<double> (back) / rate;
                        }
                    }

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
            /*  AT A SPEED (namespace draft §22.4) a pass is the slice's length
                of the FILE, and how far the file has got is the run's clock's
                business; at exactly one from the launch on, the count below. */
            /*  NOT BEFORE THE LAUNCH, for the reason `updatePositions` gives: a
                slice carried on inside its loop is dated back before it. */
            const auto heardTo = std::max (now, run->launchedAtSample);
            const auto atSpeed = ! run->rateClock.isIdentityFrom (static_cast<double> (run->launchedAtSample));
            const auto pass = static_cast<double> (run->passSamples);
            const auto played = atSpeed ? std::max (0.0, run->rateClock.sourceAt (static_cast<double> (heardTo)) - run->rangeSource)
                                        : 0.0;

            /*  A SLICE MOVED UNDER ITSELF (namespace draft §33) counts its
                first pass from where the move landed, and the passes it had
                played before the move still count. Unmoved, the first pass is
                an ordinary one and these are the integers they always were. */
            const auto moved = run->firstPassSamples > 0;
            const auto first = static_cast<double> (run->firstPass());

            if (atSpeed)
            {
                run->rangeIteration = moved ? run->passesBefore + static_cast<int> (passesDoneIn (played, first, pass)) + 1
                                            : static_cast<int> (played / pass) + 1;
            }
            else
            {
                const auto elapsed = std::max<std::int64_t> (0, heardTo - run->rangeStartedAtSample);
                run->rangeIteration = moved ? run->passesBefore + static_cast<int> (passesDoneIn (static_cast<double> (elapsed), first, pass)) + 1
                                            : static_cast<int> (elapsed / run->passSamples) + 1;
            }

            /*  The sample at which the file will have played `passes` of the
                slice, at the speeds placed so far - nothing while a speed held
                at nought never gets there, and a boundary that is never reached
                is not placed. */
            const auto sampleAfterPasses = [&] (double passes) -> std::optional<std::int64_t>
            {
                const auto when = run->rateClock.whenSourceReaches (moved ? run->rangeSource + first + (passes - 1.0) * pass
                                                                          : run->rangeSource + passes * pass);

                if (! when.has_value())
                    return std::nullopt;

                return static_cast<std::int64_t> (std::llround (*when));
            };

            if (run->rangesFinished)
                continue;

            /*  A RUN THE AUDIO WAS TOLD TO STOP PLACES NOTHING MORE (found by
                the author on 2026-09-25: Esc, panic and the run's cross did
                not stop a looping cue). A boundary placed after the stop
                would launch the next range into a voice that was just
                silenced - the sound coming back after Esc, which §4.4 does
                not allow. A fade-and-stop still advances while it fades: its
                stop is not issued until the fade is done. */
            if (run->stopIssued)
                continue;

            /*  RE-READ AT EVERY BOUNDARY, which is decision L: a `loops` an
                operator changed while the range played is honoured from here,
                and a range deleted while it played is not entered again. The
                pass length of the range playing now is the run's own clock -
                an in or out moved while it plays is heard at once, moved by
                `applySlices` (namespace draft §33, the author's ruling of
                2026-10-06), and never by a re-arm. */
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
                if (atSpeed)
                {
                    const auto when = sampleAfterPasses (moved ? static_cast<double> (passesDoneIn (played, first, pass)) + 1.0
                                                               : std::floor (played / pass) + 1.0);

                    if (! when.has_value())
                        continue;               // held at nought: the advance waits for the file to move

                    endsAt = *when;
                }
                else if (moved)
                {
                    const auto gone = passesDoneIn (static_cast<double> (heardTo - run->rangeStartedAtSample), first, pass);

                    endsAt = run->rangeStartedAtSample + run->firstPassSamples + gone * run->passSamples;
                }
                else
                {
                    const auto passesGone = (heardTo - run->rangeStartedAtSample) / run->passSamples;

                    endsAt = run->rangeStartedAtSample + (passesGone + 1) * run->passSamples;
                }
            }
            else if (wanted > 0)
            {
                /*  A moved slice owes what is left of its count after the
                    passes it played before the move - at least the one it is
                    on, which is the pass a count already reached ends with. */
                const auto passesOwed = moved ? std::max (1, wanted - run->passesBefore) : wanted;

                if (atSpeed)
                {
                    const auto when = sampleAfterPasses (static_cast<double> (passesOwed));

                    if (! when.has_value())
                        continue;

                    endsAt = *when;
                }
                else if (moved)
                {
                    endsAt = run->rangeStartedAtSample + run->firstPassSamples
                               + static_cast<std::int64_t> (passesOwed - 1) * run->passSamples;
                }
                else
                {
                    endsAt = run->rangeStartedAtSample
                               + static_cast<std::int64_t> (wanted) * run->passSamples;
                }
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

            /*  WHERE IT GOES: the slice the transport cue named, or the one
                after this. `advanceTo` is already known to be a slice of this
                cue and inside the graph's slots - `advanceTargetOf` refuses
                anything else and leaves it at -1 - so there is nothing left to
                judge here. */
            const auto next = run->advanceTo >= 0 ? run->advanceTo : run->range + 1;
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
            run->advanceTo = -1;

            if (! hasNext)
            {
                /*  THE PLAYLIST IS OVER, and saying so here is what stops
                    `observeEdges` ending the run at every boundary before this
                    one - see `rangesFinished`. */
                run->rangesFinished = true;
                continue;
            }

            /*  THE OUTGOING SLICE'S CLOCK, kept for the lane before it is
                overwritten below: the sound is in that slice until `placeAt`,
                and a level read from the incoming one's would arrive early by
                however far ahead the boundary was placed (§20.4). */
            run->laneOutgoingOrigin = run->positionOrigin;
            run->laneOutgoingAt = run->rangeStartedAtSample;
            run->laneOutgoingPass = run->passSamples;
            run->laneOutgoingSource = run->rangeSource;
            run->laneOutgoingFirst = run->firstPassSamples;
            run->laneOutgoingFirstFrom = run->firstPassFrom;

            /*  The next range's clock starts at the boundary, so its first pass
                is measured from where it will actually begin rather than from
                the tick that decided it. */
            run->rangeStartedAtSample = placeAt;
            run->rangeSource = run->rateClock.sourceAt (static_cast<double> (placeAt));

            /*  THE PLAYHEAD'S ORIGIN MOVES WITH THAT CLOCK, and both move when
                the boundary is PLACED rather than when it is crossed. They have
                to move together or the file position would be measured from one
                range's in-point against the other range's clock, which is a
                playhead that jumps somewhere neither range covers. Being early
                by the placement horizon is the same lateness `rangeIteration`
                already carries here, and it reads as the playhead waiting at
                the incoming range's in-point until the sound reaches it. */
            /*  THE INCOMING SLICE AS ITS SLOT PLAYS IT (namespace draft §33):
                the clip's own points, or where an edit since the arm has put
                them - placed on its slot now, before its launch, as a move from
                the clip's first frame, so the slot reads the new points from
                its first sample and nothing is rebuilt. */
            auto incoming = ranges[static_cast<std::size_t> (next)];
            const auto nextSlot = static_cast<std::size_t> (next);

            if (nextSlot < run->playingSlices.size() && nextSlot < run->armedSlices.size())
            {
                auto& playing = run->playingSlices[nextSlot];
                const auto& armed = run->armedSlices[nextSlot];

                if (incoming.out > incoming.in && incoming.in >= 0.0)
                    playing = { incoming.in, incoming.out, incoming.pingPong };

                incoming.in = playing.in;
                incoming.out = playing.out;

                /*  EVERY TIME, and not only when the points differ from the
                    arm's: a slot entered before and moved then still holds
                    that move, read against a reader that starts again at the
                    clip's first frame. Placed from that frame, this one is in
                    force from the launch on. Never on the slot sounding now -
                    a transport cue naming the slice it is in - whose reader is
                    still reading. */
                if (next != run->range)
                    audio->placeLoop (run->track, next, { armed.in, playing.in, playing.in, playing.out, 0.0, 1, false });

                run->readerAtSliceStart = armed.in;

                /*  BACKWARDS, OR BOUNCING (§41): from its out-point, or out and
                    back - the move above replaced, and the file read from the
                    boundary the way the run goes. */
                if (next != run->range && (run->direction < 0 || playing.pingPong))
                {
                    run->range = next;
                    anchorSlice (*run, next, static_cast<double> (placeAt), armed.in, playing);
                }
                else if (run->bent && ! playing.pingPong && run->direction > 0)
                {
                    run->bent = false;
                }
            }
            else
            {
                run->readerAtSliceStart = incoming.in;
            }

            run->soundingSlice = incoming.id;
            run->positionOrigin = incoming.in;
            run->passesWanted = incoming.loops;
            run->passSamples = samplesForRange (incoming, rate);
            run->firstPassSamples = 0;
            run->passesBefore = 0;
            run->loopMove = 0;

            engine.submit (origin::engine, "run.range",
                           { osc::Value::string (run->id),
                             osc::Value::int32 (static_cast<std::int32_t> (next)) });
        }
    }

    void Runner::updatePositions (std::int64_t tick)
    {
        /*  WHERE THE PLAYHEAD IS IN THE FILE, in seconds, which is what a
            client draws over a waveform (§3.30) and what has been the literal
            nought since the row was published in Phase 2: `Run::position` was
            declared, published and assigned by nothing at all, so the console
            gates its playhead on `position > 0` and draws none.

            ITS OWN PASS, AND NOT A LINE IN `advanceRanges`. That loop continues
            on `run->range < 0`, and `range` is -1 for every kind but media and
            for a media cue with NO ranges - which is the ordinary media cue,
            and the only run the bar is ever drawn over. Paid there, this would
            have been paid for every case except the one that needed it.

            The arithmetic is an origin plus a count: where in the file the
            thing playing now began, plus how long it has been playing. The
            count is the sample clock's, which is the same reason
            `rangeIteration` is computed rather than incremented - a counter
            would be right during a show and nought through every replay. The
            origin is the run's `positionOrigin`, cached when it changes rather
            than read here, because reading it here means `rangesOf` - a walk of
            the cue's children and a document read per range - for every run on
            every tick, and §4.1's GO path shares this thread.

            Nothing is logged: §3.15 keeps continuous readouts out of the record,
            and a replay - which has no counter to have counted - leaves this
            where it leaves `rangeIteration` and `phase`. */
        const auto rate = static_cast<double> (audio->sampleRate());

        /*  A rate of nought is a graph that has not been prepared yet, and the
            window between `setPlayer` and the device opening is real. Dividing
            by it would publish an infinity as a playhead, which a client draws
            as a bar off the end of the world. */
        if (! (rate > 0.0))
            return;

        const auto now = audio->samplesElapsed();

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            /*  A RUN THAT HAS ENDED KEEPS THE PLAYHEAD IT STOPPED AT, which is
                the more useful of the two honest answers: a finished run stays
                on the tree for its retention (`retentionTicks`) so that what
                happened can still be read, and where it got to is part of what
                happened. Its meter is silence: nothing leaves a voice it no
                longer has. */
            if (run == nullptr)
                continue;

            if (run->isFinished())
            {
                run->meter = Run::silentDb;
                continue;
            }

            /*  THE GUARD THAT MAKES THIS A READOUT RATHER THAN A LIE.
                `launchedAtSample` is nought until the launch has been PLACED,
                so a run that is armed and waiting for its GO - which is every
                run parked on a standby, for as long as the operator takes -
                would otherwise report the session's whole elapsed time and be
                drawn as a cue that has been playing since the show opened. */
            if (run->launchedAtSample <= 0)
            {
                /*  A RUN WITH NO VOICE STILL GETS OLDER, and a fade is the
                    case that matters: it has a duration the document declares
                    and a bar a client draws from it, and `launchedAtSample` is
                    nought for it for ever because nothing was ever placed on a
                    track. So it is measured in TICKS from the GO that started
                    it - the clock such a run actually runs on - and the sample
                    clock is kept for the runs that have one, where it is the
                    finer answer and the only one a range can wrap.

                    THE GUARD ABOVE STILL HOLDS FOR WHAT IT WAS WRITTEN FOR. A
                    run that has not been LAUNCHED reports nothing, so a cue
                    parked on a standby does not read as having played since
                    the show opened; what changes is only that having no VOICE
                    is no longer read as having no PLAYHEAD. */
                /*  NOR AN OSC CUE WHOSE CURVES ARE PLAYING (namespace draft 45):
                    `advanceCurves` sets its playhead on its own clock, looped
                    and sought, which ticks since the GO would overwrite. */
                if (run->launchRequestedAtTick > 0 && ! run->isWaiting()
                      && run->state != runState::armed
                      && run->state != runState::preparing
                      && ! (run->kind == "osc" && isCurving (run->id)))
                    run->position = static_cast<double> (tick - run->launchRequestedAtTick)
                                      / static_cast<double> (TickClock::rateHz);

                continue;
            }

            /*  HOW LOUD IT LEFT ITS TRACK since the last tick, after the
                fader (author, 2026-09-25: "On the sampler fader displays of the
                D700 can we have a post fader level meter too?"). TAKEN, so each
                tick reads its own twenty milliseconds and a surface can keep
                the loudest of those it has not drawn yet. */
            if (run->track >= 0)
            {
                const auto peak = static_cast<double> (audio->takeOutputPeak (run->track));

                run->meter = peak > 0.0 ? std::max (Run::silentDb, 20.0 * std::log10 (peak))
                                        : Run::silentDb;
            }

            /*  AT A SPEED (namespace draft §22.4) the file moves by the run's
                clock - the breakpoints the voice was given - rather than one
                second a second: how far it has moved since the launch, or since
                the slice began, wrapped by the slice's pass as below. A run at
                exactly one from its launch on takes the count below instead,
                the same integers as before there was a speed. */
            /*  NOT BEFORE THE LAUNCH: between its placement and its instant the
                file is where the launch will start it. Said by clamping the
                sample rather than the count, because a slice carried on inside
                its loop (K8's review) has a start dated back before the launch,
                and the count from it is already part-way through a pass. */
            const auto heardTo = std::max (now, run->launchedAtSample);

            /*  BACKWARDS OR BOUNCING (namespace draft §41): from the last turn. */
            if (run->bent)
            {
                run->position = bentSecondAt (*run, static_cast<double> (heardTo), rate);

                const auto inRange = run->range >= 0 && run->rangeStartedAtSample > 0;
                run->slicePlayed = inRange ? std::max (0.0, run->rateClock.sourceAt (static_cast<double> (heardTo))
                                                              - run->rangeSource) / rate
                                           : 0.0;
                continue;
            }

            if (! run->rateClock.isIdentityFrom (static_cast<double> (run->launchedAtSample)))
            {
                const auto inRange = run->range >= 0 && run->rangeStartedAtSample > 0;
                const auto origin = inRange ? run->rangeSource : run->launchSource;
                auto played = std::max (0.0, run->rateClock.sourceAt (static_cast<double> (heardTo)) - origin);

                /*  A SLICE MOVED UNDER ITSELF (namespace draft §33): its first
                    pass from where the move landed. */
                if (inRange && run->firstPassSamples > 0)
                {
                    const auto first = static_cast<double> (run->firstPassSamples);
                    const auto pass = static_cast<double> (run->passSamples);

                    run->position = secondInSlice (played, run->positionOrigin, run->firstPassFrom, first, pass, rate);
                    run->slicePlayed = static_cast<double> (run->passesBefore + passesDoneIn (played, first, pass))
                                         * pass / rate
                                       + std::max (0.0, run->position - run->positionOrigin);
                    continue;
                }

                /*  HOW FAR THE SLICE HAS GOT, passes and all (K8's review): what
                    an Esc pausing a looping bed carries on from. */
                run->slicePlayed = inRange ? played / rate : 0.0;

                if (inRange && run->passSamples > 0)
                    played = std::fmod (played, static_cast<double> (run->passSamples));

                run->position = run->positionOrigin + played / rate;
                continue;
            }

            /*  MEASURED FROM THE LAUNCH, and clamped at nought because the
                launch is PLACED a few ticks into the future: between the
                placement and the instant itself the difference is negative, and
                what that window means is a playhead sitting at the start offset
                waiting for the sound, which is what nought gives. */
            auto elapsed = std::max<std::int64_t> (0, now - run->launchedAtSample);
            run->slicePlayed = 0.0;

            if (run->range >= 0 && run->rangeStartedAtSample > 0)
            {
                /*  A RANGE IS MEASURED FROM ITS OWN ENTRY AND WRAPS AT EVERY
                    PASS. Go.dot places nothing inside a range - M12 measured
                    the clip's own wrap winning by thousands of times in damage
                    energy - so a looping range's playhead is back at its
                    in-point on every pass, and what the file position wants is
                    the remainder rather than the total. */
                elapsed = std::max<std::int64_t> (0, heardTo - run->rangeStartedAtSample);
                run->slicePlayed = static_cast<double> (elapsed) / rate;

                /*  A SLICE MOVED UNDER ITSELF (namespace draft §33): its first
                    pass from where the move landed, then the loop. */
                if (run->firstPassSamples > 0)
                {
                    run->position = secondInSlice (elapsed, run->positionOrigin, run->firstPassFrom,
                                                   run->firstPassSamples, run->passSamples, rate);
                    run->slicePlayed = static_cast<double> (run->passesBefore
                                                            + passesDoneIn (static_cast<double> (elapsed),
                                                                            static_cast<double> (run->firstPassSamples),
                                                                            static_cast<double> (run->passSamples)))
                                         * static_cast<double> (run->passSamples) / rate
                                       + std::max (0.0, run->position - run->positionOrigin);
                    continue;
                }

                if (run->passSamples > 0)
                    elapsed %= run->passSamples;
            }

            run->position = run->positionOrigin + static_cast<double> (elapsed) / rate;
        }
    }

    namespace
    {
        /*  THE SECOND OF THE FILE A VOICE IS AT, AT A SAMPLE - the playhead's
            arithmetic (`updatePositions`), answered for a level rather than a
            readout. Two answers differ from the playhead's, both where the
            voice is not yet where the playhead says (§20.4):

            - before the launch, the second the voice will START at, since
              that is the level it has to be at when it does;
            - between a slice boundary's placement and its crossing, the
              OUTGOING slice, whose clock the placement overwrote early. */
        double lanePositionAt (const Run& run, std::int64_t sample, double rate) noexcept
        {
            if (run.launchedAtSample <= 0 || sample < run.launchedAtSample)
                return run.laneStart;

            /*  AT A SPEED (namespace draft §22.4), the file by the run's clock:
                the same origins and wraps as below, measured in the file's
                samples rather than the clock's. A lane is a property of the
                recording, so a slowed cue hears its lane slowed with it. */
            if (! run.rateClock.isIdentityFrom (static_cast<double> (run.launchedAtSample)))
            {
                const auto source = run.rateClock.sourceAt (static_cast<double> (sample));
                const auto inSlice = run.range >= 0 && run.rangeStartedAtSample > 0;
                const auto outgoing = run.range >= 0 && run.laneOutgoingAt > 0 && sample < run.rangeStartedAtSample;

                const auto originSource = outgoing ? run.laneOutgoingSource
                                        : inSlice ? run.rangeSource
                                                  : run.launchSource;
                const auto pass = static_cast<double> (outgoing ? run.laneOutgoingPass : run.passSamples);
                const auto secondOrigin = outgoing ? run.laneOutgoingOrigin : run.positionOrigin;

                auto played = std::max (0.0, source - originSource);

                /*  A slice moved under itself (§33): its first pass from where
                    the move landed. */
                const auto movedFirst = outgoing ? run.laneOutgoingFirst : (inSlice ? run.firstPassSamples : 0);

                if (movedFirst > 0)
                    return secondInSlice (played, secondOrigin,
                                          outgoing ? run.laneOutgoingFirstFrom : run.firstPassFrom,
                                          static_cast<double> (movedFirst), pass, rate);

                if ((outgoing || inSlice) && pass > 0.0)
                    played = std::fmod (played, pass);

                return secondOrigin + played / rate;
            }

            if (run.range >= 0 && run.laneOutgoingAt > 0 && sample < run.rangeStartedAtSample)
            {
                auto elapsed = std::max<std::int64_t> (0, sample - run.laneOutgoingAt);

                if (run.laneOutgoingFirst > 0)
                    return secondInSlice (elapsed, run.laneOutgoingOrigin, run.laneOutgoingFirstFrom,
                                          run.laneOutgoingFirst, run.laneOutgoingPass, rate);

                if (run.laneOutgoingPass > 0)
                    elapsed %= run.laneOutgoingPass;

                return run.laneOutgoingOrigin + static_cast<double> (elapsed) / rate;
            }

            auto elapsed = sample - run.launchedAtSample;

            /*  A SLICE WRAPS AT EVERY PASS: the clip loops inside Tracktion, so
                the file is back at the in-point each time round and the lane
                with it - the same stretch of the curve on every pass (DA). */
            if (run.range >= 0 && run.rangeStartedAtSample > 0)
            {
                elapsed = std::max<std::int64_t> (0, sample - run.rangeStartedAtSample);

                if (run.firstPassSamples > 0)
                    return secondInSlice (elapsed, run.positionOrigin, run.firstPassFrom,
                                          run.firstPassSamples, run.passSamples, rate);

                if (run.passSamples > 0)
                    elapsed %= run.passSamples;
            }

            return run.positionOrigin + static_cast<double> (elapsed) / rate;
        }
    }

    void Runner::applyLanes()
    {
        if (audio == nullptr)
            return;

        const auto rate = static_cast<double> (audio->sampleRate());

        if (! (rate > 0.0))
            return;

        /*  THE POINTS FOLLOW THE DOCUMENT, gated as `applyEq` is: a tick with
            nobody editing compares one number, and an edit - or an undo -
            reaches every sounding run on the next tick (DB). */
        const auto revision = document.showRevision();
        const auto reread = revision != laneRevision;
        laneRevision = revision;

        /*  ONE SLEW AHEAD (DC). The voice arrives at a level one slew after it
            is given it, so the lane is read where the file will be by then,
            and a corner the designer drew lands on the second it was drawn at
            rather than a slew after it. The slew is one tick by design
            (`audio::CueMatrix::levelSlewSeconds`: "exactly one control tick"),
            so one tick of samples is what this reads ahead. */
        const auto at = audio->samplesElapsed() + static_cast<std::int64_t> (samplesPerTick);

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.isFinished() || snapshot.kind != "media" || snapshot.track < 0)
                continue;

            auto* run = runs.find (snapshot.id);

            if (run == nullptr)
                continue;

            if (reread)
            {
                const auto cue = document.findById (run->cue);

                if (cue.isValid())
                {
                    run->lane = doc::readLevelLane (textOf (cue, "levelLane")).points;
                    run->sendLanes = sendLanesOf (cue);
                }
            }

            run->laneDb = run->lane.empty() ? 0.0
                                            : doc::laneLevelDb (run->lane, lanePositionAt (*run, at, rate));

            /*  ITS SENDS' LANES, ONE COEFFICIENT SLEW AHEAD (namespace draft
                §28, PZ). A send reaches the voice as a matrix coefficient,
                and a coefficient glides over `sendLaneLeadSeconds` rather
                than one tick, so a lane read only a tick ahead would land a
                glide late on every ramp. Only a change worth hearing moves the
                routing: the matrix is rebuilt for the runs it concerns, in
                `applyRouting`, and a tick with every lane flat costs a lookup
                a send. */
            if (run->sendLanes.empty() && run->sendLaneDb.empty())
                continue;

            const auto sendAt = audio->samplesElapsed()
                              + static_cast<std::int64_t> (sendLaneLeadSeconds * rate);
            auto offsets = sendLaneOffsets (run->sendLanes, lanePositionAt (*run, sendAt, rate));

            /*  BUT NOT A MIX A HAND HOLDS in a pass (namespace draft §34): the
                hand's term, set by `recordLane` after this, is the one heard,
                and a lane and a hand taking turns would rebuild the routing on
                every tick. */
            if (lanes != nullptr && lanes->recording && run->id == lanes->run)
                for (const auto& bus : heldBuses)
                    if (const auto hand = run->sendLaneDb.find (bus); hand != run->sendLaneDb.end())
                        offsets[bus] = hand->second;

            auto changed = offsets.size() != run->sendLaneDb.size();

            for (const auto& [bus, db] : offsets)
            {
                const auto was = run->sendLaneDb.find (bus);

                if (was == run->sendLaneDb.end() || std::abs (was->second - db) > 0.01)
                    changed = true;
            }

            if (changed)
            {
                run->sendLaneDb = std::move (offsets);
                ++sendLaneRevision;
            }
        }
    }

    void Runner::applyRates()
    {
        if (audio == nullptr || samplesPerTick <= 0)
            return;

        /*  THE CUE'S SPEED FOLLOWS THE DOCUMENT, gated as `applyLanes` is: a
            tick with nobody editing compares one number, and an edit - or an
            undo - reaches every sounding run on the next tick (DW). Only the
            cue whose `rate` moved is moved: an edit of anything else reads the
            same number and places nothing. A speed fade holds its run, and the
            document waits until it lets go. */
        const auto revision = document.showRevision();
        const auto reread = revision != rateRevision;
        rateRevision = revision;

        const auto now = audio->samplesElapsed();
        const auto lead = static_cast<std::int64_t> (latencyTicks()) * samplesPerTick;
        const auto stretchLimit = audio->stretchSpeedLimit();

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.isFinished() || snapshot.kind != "media" || snapshot.track < 0)
                continue;

            auto* run = runs.find (snapshot.id);

            if (run == nullptr)
                continue;

            /*  HELD WHILE A SPEED FADE MOVES IT (DW), asked of the fades
                themselves, so every way one ends - arriving, taken over,
                killed, dropped by a double Esc - lets go the same way. And
                read again as it lets go: an edit of the cue's speed made under
                the fade lands then, not at whatever edit comes next. */
            const auto held = std::any_of (running.begin(), running.end(),
                                           [&snapshot] (const FadeJob& job)
                                           {
                                               return job.movesRate && job.heldRun() == snapshot.id;
                                           });
            const auto letGo = run->rateHeld && ! held;
            run->rateHeld = held;

            if ((reread || letGo) && ! held)
            {
                if (const auto cue = document.findById (run->cue); cue.isValid())
                {
                    const auto decided = osc::parseDouble (textOf (cue, "rate")).value_or (1.0);

                    if (std::bit_cast<std::uint64_t> (decided) != std::bit_cast<std::uint64_t> (run->rateSeen))
                    {
                        run->rateSeen = decided;
                        run->ownRate = run->stretch && stretchLimit > 0.0
                                         ? std::copysign (std::min (std::abs (decided), stretchLimit), decided)
                                         : decided;
                    }
                }
            }

            /*  Before the launch the launch places it (`launchIfDue`). */
            if (run->launchedAtSample <= 0)
            {
                run->rateNow = run->ownRate;
                continue;
            }

            /*  A CHANGE IS HELD UNTIL ONE HORIZON FROM NOW, then a straight line
                to it over a tick - the same two breakpoints on the run's clock
                and on the voice, so the playhead is read off the arithmetic the
                audio thread plays by. A speed fade moves `ownRate` every tick,
                and its ramps join end to end: each starts where the last one
                ended. */
            if (std::bit_cast<std::uint64_t> (run->ownRate) != std::bit_cast<std::uint64_t> (run->ratePlaced))
            {
                const auto last = run->rateClock.size() > 0 ? run->rateClock.back().at : 0.0;
                const auto from = std::max (static_cast<double> (now + lead), last);
                const auto to = from + static_cast<double> (samplesPerTick);
                const auto before = std::abs (run->ratePlaced);
                const auto after = std::abs (run->ownRate);

                if (from > last)
                {
                    run->rateClock.place (from, before);
                    audio->placeRate (run->track, static_cast<std::int64_t> (from), before);
                }

                /*  THROUGH NOUGHT (namespace draft §41, WX): the speed's size
                    comes down to nought where the line crosses it, and the
                    file turns round there - where the reader stands still, so
                    the turn costs no frame. */
                const auto way = run->ownRate < 0.0 ? -1 : run->ownRate > 0.0 ? 1 : 0;

                if (way != 0 && way != run->direction)
                {
                    const auto crossing = before + after > 0.0
                                            ? from + static_cast<double> (samplesPerTick) * before / (before + after)
                                            : from;

                    if (crossing > from)
                    {
                        run->rateClock.place (crossing, 0.0);
                        audio->placeRate (run->track, static_cast<std::int64_t> (std::llround (crossing)), 0.0);
                    }

                    turnRun (*run, crossing, way);
                }

                run->rateClock.place (to, after);
                audio->placeRate (run->track, static_cast<std::int64_t> (to), after);
                run->ratePlaced = run->ownRate;
            }

            /*  The past the clock no longer needs: what the playhead reads is
                now and after, and where the file was at the launch and at the
                slice's start is kept on the run. */
            run->rateClock.forgetBefore (static_cast<double> (now) - static_cast<double> (samplesPerTick));
            run->rateNow = static_cast<double> (run->direction) * run->rateClock.rateAt (static_cast<double> (now));
        }
    }

    void Runner::applySlices (Engine& engine, std::int64_t tick)
    {
        /*  A SLICE MOVED WHILE IT SOUNDS IS HEARD AT ONCE (namespace draft §33,
            the author's ruling of 2026-10-06). It used to be heard only at the
            next fire: a slice's in and out are its clip's loop range, which is
            on Tracktion's restart list, and nothing re-read them while the cue
            played. Now the points are moved under the clip rather than in it -
            a move placed on the slot's reader (Player::placeLoop) - and the
            run's clock moves with it, so the playhead, the lane, the pass count
            and the boundary all read what the voice plays.

            Gated per run on the show's revision, as the other passes are on the
            Runner's: a tick with nobody editing compares one number a run. */
        if (audio == nullptr || samplesPerTick <= 0)
            return;

        const auto rate = static_cast<double> (audio->sampleRate());

        if (! (rate > 0.0))
            return;

        const auto revision = document.showRevision();
        const auto now = audio->samplesElapsed();
        const auto lead = static_cast<std::int64_t> (latencyTicks()) * samplesPerTick;

        /*  HOW LONG AN ARMED CUE'S EDITS MUST STOP BEFORE IT IS ARMED AGAIN: a
            drag is a write a frame, and an arm is a graph rebuild. A fifth of a
            second, so a hand that lets go is heard by the time it reaches GO. */
        constexpr std::int64_t rearmSettleTicks = TickClock::rateHz / 5;

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.isFinished() || snapshot.kind != "media" || snapshot.track < 0)
                continue;

            auto* run = runs.find (snapshot.id);

            if (run == nullptr)
                continue;

            /*  THE READER'S WORD ON THE LAST MOVE. A reader that met it after it
                had read past its start - the horizon shorter than how far ahead
                a stretcher reads - applied it from where it was, and the clock
                starts there instead. A second after the move with no word, the
                reader took it where it was placed. */
            if (run->loopMove != 0 && run->range >= 0)
            {
                const auto took = audio->loopTaken (run->track, run->range);

                if (took.has_value() && took->move == run->loopMove)
                {
                    const auto late = took->from - run->loopMoveFrom;

                    if (late > 0.0 && static_cast<std::size_t> (run->range) < run->playingSlices.size())
                    {
                        const auto lateSamples = late * rate;
                        const auto atSpeed = ! run->rateClock.isIdentityFrom (static_cast<double> (run->launchedAtSample));

                        run->rangeSource += lateSamples;

                        if (atSpeed)
                        {
                            if (const auto when = run->rateClock.whenSourceReaches (run->rangeSource))
                                run->rangeStartedAtSample = static_cast<std::int64_t> (std::llround (*when));
                        }
                        else
                        {
                            run->rangeStartedAtSample += static_cast<std::int64_t> (std::llround (lateSamples));
                        }

                        const auto& playing = run->playingSlices[static_cast<std::size_t> (run->range)];

                        run->readerAtSliceStart = took->from;
                        run->firstPassFrom = took->fileAt;
                        run->firstPassSamples = std::max<std::int64_t> (1, std::llround ((playing.out - took->fileAt) * rate));
                    }

                    run->loopMove = 0;
                }
                else if (now > run->loopMovePlacedAt + static_cast<std::int64_t> (rate))
                {
                    run->loopMove = 0;
                }
            }

            /*  AN ARMED CUE NOT YET LAUNCHED holds its slots' clips, written at
                its arm: an edit to its points, or to its start offset, is heard
                by arming it again, which with nothing sounding costs nothing
                but a rebuild. Once the edits stop, so a drag is one arm. A GO
                in the meantime launches what was armed and the pass below moves
                it on the next tick, which is a move like any other. */
            if (run->launchedAtSample <= 0)
            {
                if (run->state != runState::armed || run->launchRequested || ! run->armConfirmed)
                    continue;

                const auto cue = document.findById (run->cue);

                if (! cue.isValid())
                    continue;

                if (run->slicesRevision != revision)
                {
                    run->slicesRevision = revision;

                    const auto ranges = rangesOf (cue);
                    auto differs = ranges.size() != run->armedSlices.size();

                    for (std::size_t i = 0; ! differs && i < ranges.size(); ++i)
                        differs = ! juce::exactlyEqual (ranges[i].in, run->armedSlices[i].in)
                                  || ! juce::exactlyEqual (ranges[i].out, run->armedSlices[i].out);

                    /*  The offset only where the run's own does not win: a
                        seek's is a decision about this run (§4.10). */
                    if (! differs && run->startOffset <= 0.0 && ranges.empty())
                        differs = ! juce::exactlyEqual (numberOf (cue, "startOffset"), run->armedStartOffset);

                    if (differs)
                        run->rearmEditedAt = tick;
                }

                if (run->rearmEditedAt >= 0 && tick - run->rearmEditedAt >= rearmSettleTicks)
                {
                    run->rearmEditedAt = -1;
                    run->armConfirmed = false;
                    requestArmOn (engine, cue, *run, run->ownLevel);
                }

                continue;
            }

            if (run->slicesRevision == revision)
                continue;

            /*  A SLICE ON ITS WAY IN - a launch or a boundary placed and not yet
                crossed, or the last move not yet reached - is left until it is
                there: the clock it would be measured on is not the sound's yet.
                The revision is not marked, so the edit is read when it is. */
            if (run->range < 0 || run->rangeStartedAtSample <= 0 || now < run->rangeStartedAtSample)
                continue;

            run->slicesRevision = revision;

            if (run->stopIssued || run->rangesFinished)
                continue;

            const auto cue = document.findById (run->cue);

            if (! cue.isValid())
                continue;

            const auto ranges = rangesOf (cue);
            const auto slot = static_cast<std::size_t> (run->range);

            /*  The slices not sounding take their new points when they are
                entered (`advanceRanges`). Only the one sounding moves now, and
                it is found by its identifier: deleted, it finishes its pass and
                is not entered again (decision L); and an edit elsewhere in the
                list that renumbers it does not make it another range. */
            if (slot >= run->playingSlices.size())
                continue;

            const auto found = std::find_if (ranges.begin(), ranges.end(), [run] (const RangeSpec& range)
                                             { return ! range.id.empty() && range.id == run->soundingSlice; });

            if (found == ranges.end())
                continue;

            const auto& wanted = *found;
            const auto& playing = run->playingSlices[slot];

            if (juce::exactlyEqual (wanted.in, playing.in) && juce::exactlyEqual (wanted.out, playing.out))
                continue;

            if (! (wanted.out > wanted.in) || wanted.in < 0.0)
                continue;

            moveSoundingSlice (*run, wanted, now + lead);
        }
    }

    //==========================================================================
    /*  BACKWARDS AND BOUNCING (namespace draft §41, WX, the author's pick).

        THE READER GOES ON FORWARDS. Its position counts the speed's size on the
        run's clock, so Tracktion's resampler and stretcher read one unbroken
        stream and nothing above the reader moves back; which way the file goes
        under it is a segment on the slot's loop source - from this reader
        position on, the file is here and goes this way - which patch 0002's
        `UnrolledLoopReader` reads backwards a run at a time.

        Where the reader is: a cue with no ranges reads the file at its own
        position, from its start offset at the launch; a slice from where its
        slot's clip starts (`readerAtSliceStart`). */
    double Runner::readerSecondAt (const Run& run, double sample) const
    {
        const auto rate = static_cast<double> (std::max (1, audio != nullptr ? audio->sampleRate() : 48000));
        const auto inRange = run.range >= 0 && run.rangeStartedAtSample > 0;
        const auto origin = inRange ? run.rangeSource : run.launchSource;
        const auto played = std::max (0.0, run.rateClock.sourceAt (sample) - origin) / rate;

        return (inRange ? run.readerAtSliceStart : run.pieceStart) + played;
    }

    /*  Where its file is: from its last turn when it is bent, else by the
        playhead's forward arithmetic. */
    double Runner::fileSecondAt (const Run& run, double sample) const
    {
        const auto rate = static_cast<double> (std::max (1, audio != nullptr ? audio->sampleRate() : 48000));

        if (run.bent)
            return bentSecondAt (run, sample, rate);

        const auto inRange = run.range >= 0 && run.rangeStartedAtSample > 0;
        const auto origin = inRange ? run.rangeSource : run.launchSource;
        auto played = std::max (0.0, run.rateClock.sourceAt (sample) - origin);

        if (inRange && run.firstPassSamples > 0)
            return secondInSlice (played, run.positionOrigin, run.firstPassFrom,
                                  static_cast<double> (run.firstPassSamples), static_cast<double> (run.passSamples), rate);

        if (inRange && run.passSamples > 0)
            played = std::fmod (played, static_cast<double> (run.passSamples));

        return run.positionOrigin + played / rate;
    }

    /*  THE FILE TURNS ROUND at `sample`, where the speed's size is nought: the
        run reads on from where its file is there, the other way, and its slot
        is told from the reader's position there. Once a run has played
        backwards Go.dot ends it (`endTurnedRuns`), its clip's own end no longer
        meaning where the file is. */
    void Runner::turnRun (Run& run, double sample, int direction)
    {
        if (audio == nullptr)
            return;

        const auto fileThere = fileSecondAt (run, sample);
        const auto readerThere = readerSecondAt (run, sample);
        const auto* slice = sliceOf (run);
        const auto slot = std::max (0, run.range);

        run.turnSource = run.rateClock.sourceAt (sample);
        run.turnFile = fileThere;
        run.direction = direction;
        run.bent = true;
        run.turnEndPlaced = 0;

        if (! run.turned)
        {
            run.turned = true;
            audio->endsOutside (run.track, true);
        }

        audio->placeLoop (run.track, slot, { readerThere, fileThere,
                                             slice != nullptr ? slice->in : 0.0,
                                             slice != nullptr ? slice->out : 0.0,
                                             0.0, direction, slice != nullptr && slice->pingPong });
    }

    /*  A SLICE ENTERED BACKWARDS OR BOUNCING: its slot told before it sounds -
        from its clip's first frame, the file at its out-point going back, or at
        its in-point going out and back - and the run's file read from there. */
    void Runner::anchorSlice (Run& run, int slot, double sample, double readerAt, const Run::SlicePoints& slice)
    {
        if (audio == nullptr)
            return;

        const auto rate = static_cast<double> (std::max (1, audio->sampleRate()));
        const auto fileAt = run.direction < 0 ? std::max (slice.in, slice.out - 1.0 / rate) : slice.in;

        run.turnSource = run.rateClock.sourceAt (sample);
        run.turnFile = fileAt;
        run.bent = true;

        audio->placeLoop (run.track, slot, { readerAt, fileAt, slice.in, slice.out, 0.0, run.direction, slice.pingPong });
    }

    /*  THE END OF A PIECE THAT HAS PLAYED BACKWARDS, with no ranges: where its
        file reaches its start offset going back, or its file's end going
        forwards again - placed once it is within a launch horizon, as a range's
        boundary is. A range's own end is `advanceRanges`', by its passes. */
    void Runner::endTurnedRuns()
    {
        if (audio == nullptr || samplesPerTick <= 0)
            return;

        const auto now = audio->samplesElapsed();
        const auto lead = static_cast<std::int64_t> (latencyTicks() + 1) * samplesPerTick;
        const auto rate = static_cast<double> (std::max (1, audio->sampleRate()));

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.isFinished() || snapshot.kind != "media" || snapshot.track < 0)
                continue;

            auto* run = runs.find (snapshot.id);

            if (run == nullptr || ! run->turned || run->range >= 0 || run->launchedAtSample <= 0
                  || run->turnEndPlaced != 0)
                continue;

            //  Forwards to an end the show does not know: nothing to place (§41).
            if (run->direction > 0 && ! (run->pieceEnd > run->pieceStart))
                continue;

            const auto bound = run->direction < 0 ? run->pieceStart : run->pieceEnd;
            const auto distance = std::abs (bound - run->turnFile) * rate;
            const auto when = run->rateClock.whenSourceReaches (run->turnSource + distance);

            if (! when.has_value() || *when > static_cast<double> (now + lead))
                continue;

            const auto at = std::max<std::int64_t> (static_cast<std::int64_t> (std::llround (*when)),
                                                    now + 2 * audio->blockSize());
            audio->stopAtSample (run->track, 0, at);
            run->turnEndPlaced = at;
        }
    }

    void Runner::moveSoundingSlice (Run& run, const RangeSpec& wanted, std::int64_t at)
    {
        /*  WHERE THE MOVE LANDS: one launch horizon ahead, as a boundary is
            placed, so the slot's reader has it before it gets there. The file
            is wherever the slice's clock has it at that sample. */
        const auto rate = static_cast<double> (audio->sampleRate());
        const auto atSpeed = ! run.rateClock.isIdentityFrom (static_cast<double> (run.launchedAtSample));
        const auto played = atSpeed ? run.rateClock.sourceAt (static_cast<double> (at)) - run.rangeSource
                                    : static_cast<double> (at - run.rangeStartedAtSample);

        const auto first = static_cast<double> (run.firstPass());
        const auto pass = static_cast<double> (run.passSamples);
        const auto fileThere = secondInSlice (played, run.positionOrigin, run.firstFrom(), first, pass, rate);
        const auto passesDone = passesDoneIn (played, first, pass);

        /*  THE THREE CASES. Inside the new loop, the file carries on and the
            next wrap is at the new out. Past the new out, there is nothing of
            the new loop ahead of it: it jumps to the new in-point now, over the
            looper's ten milliseconds of equal-power fade (decision CD). Before
            the new in-point, it plays on into it and loops from there. */
        const auto jump = fileThere >= wanted.out - 0.5 / rate;
        const auto fileAt = jump ? wanted.in : fileThere;
        const auto readerAt = run.readerAtSliceStart + played / rate;

        const auto move = audio->placeLoop (run.track, run.range,
                                            { readerAt, fileAt, wanted.in, wanted.out, jump ? 0.010 : 0.0, 1, false });

        /*  A player that does not move loops - a replay's, a test's - leaves
            the voice where it was, and the clock stays with the voice. */
        if (move == 0)
            return;

        /*  THE OUTGOING CLOCK, kept for the lane until the move is reached, as
            a boundary keeps it (§20.4). */
        run.laneOutgoingOrigin = run.positionOrigin;
        run.laneOutgoingAt = run.rangeStartedAtSample;
        run.laneOutgoingPass = run.passSamples;
        run.laneOutgoingSource = run.rangeSource;
        run.laneOutgoingFirst = run.firstPassSamples;
        run.laneOutgoingFirstFrom = run.firstPassFrom;

        /*  AND THE NEW ONE, from the sample the move lands on: its first pass
            from where the file is to the new out, then the new loop. The passes
            already played still count towards `loops`, and a jump begins one. */
        run.rangeStartedAtSample = at;
        run.rangeSource = run.rateClock.sourceAt (static_cast<double> (at));
        run.positionOrigin = wanted.in;
        run.passSamples = samplesForRange (wanted, audio->sampleRate());
        run.firstPassFrom = fileAt;
        run.firstPassSamples = std::max<std::int64_t> (1, std::llround ((wanted.out - fileAt) * rate));
        run.passesBefore += static_cast<int> (passesDone) + (jump ? 1 : 0);
        run.readerAtSliceStart = readerAt;
        run.boundaryPlacedAt = -1;

        run.loopMove = move;
        run.loopMoveFrom = readerAt;
        run.loopMovePlacedAt = at;

        run.playingSlices[static_cast<std::size_t> (run.range)] = { wanted.in, wanted.out, wanted.pingPong };
    }

    void Runner::freeLane() noexcept
    {
        if (lanes == nullptr)
            return;

        lanes->free();
        laneBooks.clear();
        heldBuses.clear();
    }

    void Runner::recordLane (Engine& engine)
    {
        if (lanes == nullptr)
            return;

        /*  THE LOCK FLIPS THE FADERS BACK (DN, namespace draft §30.4, §34).
            Under the lock no lane can be armed or recorded, so faders flipped
            to a cue would be faders that play nothing for as long as the show
            stays locked. Let go as `lane.free` lets them go, by that command,
            so the log says why and a replay frees them in the same record. A
            pass already running is left to end, and the faders with it. */
        if (document.isLocked() && ! lanes->recording && lanes->flipped())
        {
            laneBooks.clear();
            heldBuses.clear();
            engine.submit (origin::engine, "lane.free", {});
            return;
        }

        if (! lanes->flipped())
        {
            laneBooks.clear();
            heldBuses.clear();
            return;
        }

        const auto cue = document.findById (lanes->cue());

        /*  EVERY LANE AS THE SHOW HAS IT, re-read when the show's revision moves
            or the faders flip to another cue - the curves the faders follow, the
            ones a pass is spliced into, and the numbers they are offsets on
            (UK). A ride under way is kept: an edit beside a pass does not lose
            it. A mix the cue does not send to has a lane of silence (UQ). */
        if (rideLaneCue != lanes->cue() || rideLaneRevision != document.showRevision())
        {
            if (rideLaneCue != lanes->cue())
                laneBooks.clear();

            rideLaneCue = lanes->cue();
            rideLaneRevision = document.showRevision();

            std::map<std::string, LaneBook> read;

            for (const auto& key : flippedLanes (document))
            {
                auto book = std::move (laneBooks[key]);

                book.sendId.clear();
                book.sends = false;
                book.written = 0.0;
                book.lane = { doc::LanePoint { 0.0, silenceDb } };

                if (key == levelLaneKey)
                {
                    book.sends = true;

                    if (cue.isValid())
                    {
                        book.written = numberOf (cue, "level");
                        book.lane = doc::readLevelLane (textOf (cue, "levelLane")).points;
                    }
                }
                else if (cue.isValid())
                {
                    for (const auto& child : cue)
                    {
                        if (! child.hasType ("Send") || child[juce::Identifier ("bus")].toString().toStdString() != key)
                            continue;

                        const auto base = "/godot/send/" + child[idProperty].toString().toStdString() + "/";

                        book.sendId = child[idProperty].toString().toStdString();
                        book.sends = true;
                        book.written = osc::parseDouble (document.getAttribute (base + "level").value_or ("0"))
                                           .value_or (0.0);
                        book.lane = doc::readLevelLane (document.getAttribute (base + "levelLane")
                                                            .value_or (std::string {})).points;
                        break;
                    }
                }

                read[key] = std::move (book);
            }

            laneBooks = std::move (read);
        }

        /*  WHAT A LANE SOUNDS AT, a second of the file: the number as written
            and its lane on it, as a flipped fader shows it (UK). */
        const auto heardAt = [] (const LaneBook& book, double seconds)
        {
            return std::clamp (book.written + doc::laneLevelDb (book.lane, seconds), silenceDb, 12.0);
        };

        /*  OUTSIDE A PASS, EACH FADER SITS WHERE ITS LANE STARTS (DG): at the
            cue's start offset, or at the in-point of its first slice. */
        if (! lanes->recording)
        {
            heldBuses.clear();
            rideWritten.clear();

            auto start = 0.0;

            if (cue.isValid())
            {
                const auto ranges = rangesOf (cue);
                start = ranges.empty() ? numberOf (cue, "startOffset") : ranges.front().in;
            }

            for (auto& [key, book] : laneBooks)
            {
                book.ride.clear();
                book.wasTouched = false;
                lanes->rideOf (key).rideDb = heardAt (book, start);
            }

            return;
        }

        //  Written already, and waiting for the `lane.stop` that says so.
        if (lanes->run == rideWritten)
            return;

        auto* run = runs.find (lanes->run);

        /*  HOW THE PASS ENDS (DM). A hand asking - Rec again, the window's stop
            - keeps the rides and stops the cue; the cue ending on its own, a
            stop cue or Esc keeps them and leaves the stop to what started it; a
            KILL - the pane's, or Doh!'s taking back - drops them. A double Esc
            never reaches here since K4: its handler flips the faders back and
            drops the pass with them (`freeLane`). Every one of these ends in
            `lane.stop`, and the faders stay flipped (UM). */
        const auto handAsked = lanes->stopping;
        const auto gone = run == nullptr || run->isFinished();
        /*  AND A CUE DOH! TOOK BACK DROPS ITS RIDES as a kill does (§24): the
            pass belongs to a GO that did not happen. */
        const auto killed = run != nullptr && (run->skipFooter || run->takenBack);
        const auto stopped = run != nullptr && run->state == runState::stopping;

        if (handAsked || gone || stopped)
        {
            rideWritten = lanes->run;
            heldBuses.clear();

            const auto forget = [this]
            {
                for (auto& [key, book] : laneBooks)
                {
                    book.ride.clear();
                    book.wasTouched = false;
                }
            };

            if (killed || ! cue.isValid())
            {
                forget();
                engine.submit (origin::engine, "lane.stop", one ("dropped"));
                return;
            }

            const auto rode = [] (const LaneBook& book)
            {
                return std::any_of (book.ride.begin(), book.ride.end(),
                                    [] (const RideSegment& segment) { return ! segment.empty(); });
            };

            const auto anyRidden = std::any_of (laneBooks.begin(), laneBooks.end(),
                                                [&rode] (const auto& entry) { return rode (entry.second); });

            /*  WHAT THE PASS ENDS IN, SAID (namespace draft §30.4, §34): the
                points it wrote, the seconds they span - the rides' own
                stretches, their joins included, which is what the window
                frames - and the lanes; or that nobody rode an armed fader
                while the cue sounded; or that the show was locked under the
                pass and the lock keeps the lanes as they were (the write it
                would refuse is not sent). A pass that ends in nothing says so
                rather than ending in silence. */
            std::vector<osc::Value> said { osc::Value::string ("untouched") };

            if (anyRidden && document.isLocked())
            {
                said = { osc::Value::string ("locked") };
            }
            else if (anyRidden)
            {
                std::vector<osc::Value> write { osc::Value::string (lanes->cue()) };
                std::string names;
                std::int32_t points = 0;
                auto from = std::numeric_limits<double>::max();
                auto to = std::numeric_limits<double>::lowest();
                auto judged = true;

                for (const auto& key : flippedLanes (document))
                {
                    const auto found = laneBooks.find (key);

                    if (found == laneBooks.end() || ! rode (found->second))
                        continue;

                    const auto& book = found->second;
                    const auto text = laneText (spliceRide (book.lane, book.ride, 0.05, 0.1));

                    /*  JUDGED BEFORE IT IS SENT: a lane the door would refuse is a
                        ride lost with nothing to show for it, and the refusal is
                        worth a record of its own rather than a surprise - the
                        pass ends `dropped`, nothing written, every lane or none. */
                    const auto written = doc::readLevelLane (text);

                    if (! written.problem.empty())
                    {
                        judged = false;
                        break;
                    }

                    auto first = std::numeric_limits<double>::max();
                    auto last = std::numeric_limits<double>::lowest();

                    for (const auto& segment : book.ride)
                        if (! segment.empty())
                        {
                            first = std::min (first, segment.front().seconds);
                            last = std::max (last, segment.back().seconds);
                        }

                    /*  THE JOINS ARE POINTS TOO, a dot each on the lane, and the
                        splice put nothing else between them: every point from
                        the first join to the last is the pass's. */
                    points += static_cast<std::int32_t> (
                        std::count_if (written.points.begin(), written.points.end(),
                                       [lo = first - 0.05, hi = last + 0.05] (const doc::LanePoint& point)
                                       {
                                           return point.seconds >= lo - 1.0e-4 && point.seconds <= hi + 1.0e-4;
                                       }));

                    from = std::min (from, first - 0.05);
                    to = std::max (to, last + 0.05);
                    names += (names.empty() ? "" : " ") + key;

                    write.push_back (osc::Value::string (key));
                    write.push_back (osc::Value::string (text));
                    write.push_back (osc::Value::string (book.sendId));
                }

                if (judged)
                {
                    engine.submit (origin::engine, "lane.write", std::move (write));

                    said = { osc::Value::string ("kept"),
                             osc::Value::int32 (points),
                             osc::Value::float64 (std::max (0.0, from)),
                             osc::Value::float64 (to),
                             osc::Value::string (names) };
                }
                else
                {
                    said = { osc::Value::string ("dropped") };
                }
            }

            forget();

            /*  A STOP, NOT A KILL (2026-10-02, K4, namespace draft §23.15, the
                author's "graceful stop", overruling GB of §23.6): the pane's
                stop - an abort, which owes no post-wait (§23.13, JX) - so the
                cue's EQ and insert tail rings out as any stopped cue's does,
                and the run is not marked killed. */
            if (handAsked && run != nullptr && ! gone && ! stopped)
                engine.submit (origin::engine, "run.stop", one (run->id));

            engine.submit (origin::engine, "lane.stop", std::move (said));
            return;
        }

        /*  EACH LANE, EVERY TICK. Until its lane is armed and a hand touches its
            fader, a fader reads its lane where the file is - the run's own
            level term, or its send's offset - and the motor follows it. From
            an armed lane's first touch it is LATCHED (DH): the hand's level,
            less the number it is an offset on, is that lane's term, heard at
            once, and it stays the hand's after the hand lets go. A touch with
            no move yet has written nothing, so the latch starts from where the
            fader was. A REC pressed off lets the lane go back to its curve
            (UP); pressed again, the next touch starts a new segment. */
        const auto rate = audio != nullptr ? static_cast<double> (audio->sampleRate()) : 0.0;
        const auto now = audio != nullptr ? audio->samplesElapsed() : std::int64_t { 0 };
        const auto sounding = audio != nullptr && rate > 0.0 && run->state == runState::playing
                                && run->launchedAtSample > 0 && now >= run->launchedAtSample;

        /*  AND A SAMPLE WHERE THE VOICE IS NOW - not one slew ahead, as the lane
            is read: a hand answers what it hears. Only once the voice sounds;
            before its launch the second does not move. */
        const auto second = sounding ? lanePositionAt (*run, now, rate) : 0.0;

        for (auto& [key, book] : laneBooks)
        {
            auto& ride = lanes->rideOf (key);
            const auto isLevel = key == levelLaneKey;
            const auto address = isLevel ? std::string ("/godot/surface/laneRide")
                                         : "/godot/bus/" + key + "/laneRide";
            const auto armed = lanes->isArmed (key);
            const auto held = touches != nullptr && ! touches->holdersOf (address).empty();

            if (armed && held && ! ride.touched)
            {
                ride.touched = true;

                if (! ride.handSeen)
                    ride.handDb = ride.rideDb;
            }

            const auto latched = armed && ride.touched;

            if (latched && ! book.wasTouched)
                book.ride.emplace_back();

            book.wasTouched = latched;

            if (! latched)
            {
                if (isLevel)
                {
                    ride.rideDb = std::clamp (book.written + run->laneDb, silenceDb, 12.0);
                    continue;
                }

                heldBuses.erase (key);

                const auto offset = run->sendLaneDb.find (key);
                ride.rideDb = book.sends ? std::clamp (book.written + (offset != run->sendLaneDb.end() ? offset->second
                                                                                                       : 0.0),
                                                       silenceDb, 12.0)
                                         : silenceDb;
                continue;
            }

            const auto offset = std::clamp (ride.handDb - book.written, silenceDb, 12.0);
            ride.rideDb = ride.handDb;

            if (isLevel)
            {
                run->laneDb = offset;
            }
            else
            {
                /*  A SEND'S TERM is its offset in the run's matrix, rebuilt when
                    it moves (§28.3); a mix with none is sent to by the run alone
                    until the pass gives it a send (UQ, `resolveRouting`). */
                heldBuses.insert (key);

                const auto was = run->sendLaneDb.find (key);

                if (was == run->sendLaneDb.end() || std::abs (was->second - offset) > 0.01)
                {
                    run->sendLaneDb[key] = offset;
                    ++sendLaneRevision;
                }
            }

            if (sounding)
                appendRide (book.ride, second, offset);
        }
    }

    void Runner::applyLevels()
    {
        /*  EFFECTIVE = OWN + EVERY ANCESTOR'S OWN, walked rather than cached.

            The chain is at most as deep as the show's nesting and a show is a
            handful of levels, so the walk is cheaper than any bookkeeping that
            would have to be invalidated - and bookkeeping is where a trim gets
            left behind after the group that owned it has gone. */
        /*  AND SINCE PHASE 6, TWO MORE TERMS OF THE SAME SUM: what a hand is
            adding (`trim`, a strip's fader or pad) and what every DCA marked on
            the cue is trimming by, nested DCAs included (PRD §3.28). Sums are
            order-independent, which is the property that matters - cues arrive
            in whatever order the operator pressed GO - and a DCA trims a group
            the way a group trims its members, through the group run's own
            terms reaching every member underneath it. */
        const auto dcaTermsOf = [this] (const std::string& cueId)
        {
            auto total = 0.0;

            if (dcas != nullptr)
                for (const auto& dcaId : dcaChainOf (cueId))
                    total += dcas->trimOf (dcaId);

            return total;
        };

        const auto effectiveOf = [this, &dcaTermsOf] (const Run& run)
        {
            auto total = run.ownLevel + run.trim + dcaTermsOf (run.cue);
            auto parent = run.parent;

            /*  BOUNDED BY THE TABLE, not by the tree, because a `parent` that
                pointed at itself would otherwise be a show that hangs on its
                first tick. The table cannot be longer than it is. */
            for (std::size_t guard = 0; guard <= runs.all().size() && ! parent.empty(); ++guard)
            {
                const auto* above = runs.find (parent);

                if (above == nullptr)
                    break;

                total += above->ownLevel + above->trim + dcaTermsOf (above->cue);
                parent = above->parent;
            }

            return total;
        };

        /*  A VOICE THAT IS CUT KEEPS ITS SILENCE (2026-10-01, namespace draft
            §23.6). A kill takes the voice's output to silence at the audio
            side, and a double Esc's sweep takes every voice it does not leave
            ready there in the press's own tick, a tick or two before each
            run's kill reaches it; but a lane, a DCA ridden or a fade goes on
            moving the run's level every tick until `run.ended`, and writing it
            brought the voice back - a member of a killed scene for the ticks
            before its kill, and any killed cue for good, its level written once
            more after the stop and never taken away. What the voice still
            gives then is what a kill cannot empty: the stop's own few samples
            through an emptied chain, an AU's or an LV2's tail. So a run under
            a kill, killed, or whose kill has gone out is given no level; a seek
            that brings one back gives it its own again. And it is never left so
            for longer than its kill takes to land, a tick or two: a stop cue's
            fade holding the run does not hold its kill back (GC, in
            `enforceStops`), or the cue would hang at the level the kill found
            it at until the fade's end. */
        const auto cut = [this] (const Run& candidate)
        {
            return candidate.killIssued
                     || (candidate.skipFooter && candidate.state == runState::stopping)
                     || underAKill (candidate);
        };

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->isFinished())
                continue;

            /*  AND THE CUE'S OWN LANE (namespace draft §20.4), added to this
                run's sum and walked into nobody else's: a group has none, and
                a member's lane is the member's. Nought for every run without. */
            const auto effective = effectiveOf (*run) + run->laneDb;

            if (juce::approximatelyEqual (effective, run->level))
                continue;

            run->level = effective;

            /*  A GROUP RUN HAS NO VOICE, which is what makes its level a trim
                rather than a level: the number reaches the outputs through its
                members, each of which has just had it added to its own. */
            if (audio != nullptr && run->track >= 0 && ! cut (*run))
                audio->setLevelDb (run->track, effective);
        }
    }

    void Runner::applyRouting()
    {
        /*  WHERE A SOUNDING CUE GOES, kept up with the document (author,
            2026-09-22: a send fader and a direct-out menu are things you move
            while listening, or they are things you guess at).

            GATED ON THE SHOW'S REVISION AND NOT ON A PER-RUN CACHE. A cache of
            what was last pushed would be bookkeeping to invalidate, and
            `applyLevels` above says in its own comment why that is where a
            stale value gets left behind. The revision is one number: while
            nobody edits, this does nothing at all; on the tick after an edit it
            re-resolves the handful of runs that are actually sounding, which is
            a document read each and no allocation the tick thread was not
            already making.

            `showRevision` and not `revision`, because the second moves for
            state rows as well - a fold, a standby - and routing is decided by
            show rows alone.

            NOTHING IS REPORTED WHEN IT FAILS. A cue whose direct out has just
            been deleted resolves to a problem, and the honest thing is to leave
            the sound exactly as it is: the run is already playing, GO has
            already happened, and `bad-route` is an answer to "can this cue
            start", not to "should this cue stop". `wfg validate` and the
            document's warnings are where that is said. */
        if (audio == nullptr)
            return;

        const auto revision = document.showRevision();
        const auto layer = liveLayer != nullptr ? liveLayer->revision() : 0;

        /*  AND THE PLUGIN TABLE (2026-09-26): a plugin coming up says what it
            takes, which can make a sounding cue wider - its routing follows. */
        const auto plugins = pluginTable != nullptr ? pluginTable->revision() : 0;

        if (revision == routingRevision && layer == routingLiveRevision && plugins == routingPluginRevision
              && movedRevision == routingMovedRevision && sendLaneRevision == routingSendLaneRevision)
            return;

        routingRevision = revision;
        routingLiveRevision = layer;
        routingPluginRevision = plugins;
        routingMovedRevision = movedRevision;
        routingSendLaneRevision = sendLaneRevision;

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->isFinished() || run->track < 0)
                continue;

            const auto cue = document.findById (run->cue);

            if (! cue.isValid() || (! cue.hasType ("Media") && ! cue.hasType ("Mic")))
                continue;

            std::string problem;
            const auto routing = resolveRouting (cue, audio->channelsPerTrack(), problem, chainChannelsOf (cue),
                                                 &run->moved, &run->sendLaneDb);

            if (problem.empty())
                audio->setRouting (run->track, routing);
        }
    }

    std::vector<FxSetting> Runner::fxOf (const juce::ValueTree& cue,
                                         const std::map<std::string, double>* moved) const
    {
        static const Reader schema;
        std::vector<FxSetting> out;

        /*  THE SLOTS ARE THE GRAPH'S (2026-09-26): one setting for every slot
            it was built with, in slot order, whatever the set says now. An
            entry added since has no slot and is not sent; one taken out since
            keeps its slot, switched out, so the last cue's setting on that
            voice is not left playing. With no graph to ask, the set's own
            order is the slots. */
        std::vector<std::string> inSet;
        std::vector<std::string> slots;

        if (cue.hasType ("Mic"))
        {
            /*  A MIC CUE'S ARE ITS CHANNEL'S (Phase 9b): the channel's track as
                the graph built it, its declared chain with no graph. */
            const auto channelId = textOf (cue, "channel");

            for (const auto rack : document.root().getChildWithName ("Audio"))
                if (rack.hasType ("Rack"))
                    for (const auto channel : rack)
                        if (channel.hasType ("Channel") && channel.getProperty ("id").toString().toStdString() == channelId)
                            for (const auto entry : channel)
                                if (entry.hasType ("Plugin"))
                                    inSet.push_back (entry.getProperty ("id").toString().toStdString());

            slots = pluginTable != nullptr && pluginTable->rackBuilt (channelId) ? pluginTable->builtRackOf (channelId)
                                                                                  : inSet;
        }
        else
        {
            for (const auto entry : document.root().getChildWithName ("Audio").getChildWithName ("Plugins"))
                if (entry.hasType ("Plugin"))
                    inSet.push_back (entry.getProperty ("id").toString().toStdString());

            slots = pluginTable != nullptr && pluginTable->hasGraph() ? pluginTable->built() : inSet;
        }

        for (std::size_t slot = 0; slot < slots.size(); ++slot)
        {
            FxSetting setting;
            setting.slot = static_cast<int> (slot);
            const auto& entryId = slots[slot];
            const auto stillInSet = std::find (inSet.begin(), inSet.end(), entryId) != inSet.end();

            for (const auto child : cue)
            {
                if (! stillInSet)
                    break;

                if (! child.hasType ("Fx") || child.getProperty ("plugin").toString().toStdString() != entryId)
                    continue;

                setting.fxId = child.getProperty ("id").toString().toStdString();
                setting.enabled = schema.flag (child, "fx", "enabled");
                setting.stateFile = schema.text (child, "fx", "stateFile");
                setting.statePath = statePathOf (setting.stateFile);

                /*  And what rides live over them, under the lock (2026-09-26). */
                auto values = parseFxValues (schema.text (child, "fx", "values"));

                if (liveLayer != nullptr)
                    if (const auto* riding = liveLayer->fxValuesOf (setting.fxId))
                        for (const auto& [index, value] : *riding)
                            values[index] = value;

                /*  And what a fade moved the run's to (namespace draft §26),
                    newer than the ride: keyed by the set entry, `fx/<entry>/<n>`. */
                if (moved != nullptr)
                {
                    const auto head = "fx/" + entryId + "/";

                    for (auto it = moved->lower_bound (head); it != moved->end() && it->first.rfind (head, 0) == 0; ++it)
                    {
                        const auto digits = it->first.substr (head.size());

                        if (! digits.empty() && std::all_of (digits.begin(), digits.end(),
                                                             [] (char c) { return c >= '0' && c <= '9'; }))
                            values[std::atoi (digits.c_str())] = it->second;
                    }
                }

                for (const auto& [index, value] : values)
                    setting.values.emplace_back (index, static_cast<float> (value));

                break;
            }

            out.push_back (std::move (setting));
        }

        /*  AND HOW WIDE THE CUE IS AT EACH (2026-09-26, cue/InsertChain.h):
            what the voice sends each insert and takes back. */
        const auto chain = chainOfCue (cue, pluginTable, audio != nullptr ? audio->channelsPerTrack() : 2);

        for (auto& setting : out)
            if (setting.slot >= 0 && setting.slot < static_cast<int> (chain.steps.size()))
            {
                const auto& step = chain.steps[static_cast<std::size_t> (setting.slot)];
                setting.feed = step.switchedIn ? step.feed : 0;
                setting.back = step.switchedIn ? step.back : 0;
            }

        return out;
    }

    int Runner::chainChannelsOf (const juce::ValueTree& cue) const
    {
        return chainOfCue (cue, pluginTable, audio != nullptr ? audio->channelsPerTrack() : 2).channels;
    }

    void Runner::applyFx()
    {
        /*  applyEq's shape, for its reasons: gated on the show's revision, only
            the sounding runs, only what moved - one entry switched, one value
            changed, one value withdrawn (pushed as -1, "back to the preset").
            A set that changed under a running show is not re-shaped here
            (§3.25): the slots the voice was built with are the ones pushed. */
        if (audio == nullptr)
            return;

        const auto revision = document.showRevision();
        const auto plugins = pluginTable != nullptr ? pluginTable->revision() : 0;
        const auto layer = liveLayer != nullptr ? liveLayer->revision() : 0;

        if (revision == fxRevision && plugins == fxPluginRevision && layer == fxLiveRevision
              && movedRevision == fxMovedRevision)
            return;

        fxRevision = revision;
        fxPluginRevision = plugins;
        fxLiveRevision = layer;
        fxMovedRevision = movedRevision;

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->isFinished() || run->track < 0)
                continue;

            const auto cue = document.findById (run->cue);

            if (! cue.isValid() || (! cue.hasType ("Media") && ! cue.hasType ("Mic")))
                continue;

            auto wanted = fxOf (cue, &run->moved);
            const auto slots = std::min (wanted.size(), run->fx.size());

            for (std::size_t k = 0; k < slots; ++k)
            {
                const auto& next = wanted[k];
                const auto& last = run->fx[k];

                if (next.sameAs (last))
                    continue;

                if (next.enabled != last.enabled)
                    audio->setFxEnabled (run->track, next.slot, next.enabled);

                /*  THE CUE'S WIDTH AT THIS INSERT, when it moved - switched in
                    or out, or a plugin that came up with its buses (2026-09-26). */
                if (next.feed != last.feed || next.back != last.back)
                    audio->setFxShape (run->track, next.slot, next.feed, next.back);

                /*  A NEW STATE ON A CUE THAT HAS NOT LAUNCHED is loaded before
                    it may (an undo in standby, a capture while it waits); on
                    one already sounding it is not - the knobs follow live, and
                    the rest applies the next time the cue plays.

                    AND AN INSERT SWITCHED IN BEFORE GO (found 2026-09-28): the
                    arm asked for no state while it was out, so the instance
                    still held the last cue's - which the arm's own rule says
                    must never be heard under this one. So switching in asks for
                    the cue's state, the preset's own when it has none, whether
                    or not the row changed; and the launch waits for it, as it
                    waits for an arm's. */
                const auto switchedIn = next.enabled && ! last.enabled;

                if (next.enabled && run->launchedAtSample == 0 && (switchedIn || next.stateFile != last.stateFile))
                    audio->requestFxState (run->track, next.slot, next.statePath);

                /*  Both sorted by index: one walk finds what moved, what
                    appeared and what went. */
                std::size_t a = 0, b = 0;

                while (a < next.values.size() || b < last.values.size())
                {
                    if (b >= last.values.size() || (a < next.values.size() && next.values[a].first < last.values[b].first))
                    {
                        audio->setFxParameter (run->track, next.slot, next.values[a].first, next.values[a].second);
                        ++a;
                    }
                    else if (a >= next.values.size() || last.values[b].first < next.values[a].first)
                    {
                        audio->setFxParameter (run->track, next.slot, last.values[b].first, -1.0f);
                        ++b;
                    }
                    else
                    {
                        if (std::bit_cast<std::uint32_t> (next.values[a].second)
                              != std::bit_cast<std::uint32_t> (last.values[b].second))
                            audio->setFxParameter (run->track, next.slot, next.values[a].first, next.values[a].second);

                        ++a;
                        ++b;
                    }
                }
            }

            run->fx = std::move (wanted);
        }
    }

    void Runner::applyEq()
    {
        /*  A SOUNDING CUE'S EQ, kept up with the document (Phase 9a): a
            rotary or a panel writes the cue - a decision, saved and undoable -
            and the voice playing it follows on the next tick. The routing
            pass's shape exactly, and its reasons: gated on the show's revision
            so a tick with nobody editing costs one comparison; only the runs
            that are sounding; only what differs from what the voice was last
            given, so an edit to one band pushes one run's settings once and an
            edit to a memo pushes nothing. An undo moves the revision too. */
        if (audio == nullptr)
            return;

        const auto revision = document.showRevision();
        const auto layer = liveLayer != nullptr ? liveLayer->revision() : 0;

        if (revision == eqRevision && layer == eqLiveRevision && movedRevision == eqMovedRevision)
            return;

        eqRevision = revision;
        eqLiveRevision = layer;
        eqMovedRevision = movedRevision;

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->isFinished() || run->track < 0)
                continue;

            const auto cue = document.findById (run->cue);

            if (! cue.isValid() || (! cue.hasType ("Media") && ! cue.hasType ("Mic")))
                continue;

            const auto wanted = eqOf (cue, &run->moved);

            if (wanted.sameAs (run->eq))
                continue;

            run->eq = wanted;
            audio->setEq (run->track, wanted);
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
            verbs. So a run that some fade job is holding is left alone - unless
            it is killed (see below). */
        if (audio == nullptr)
            return;

        for (const auto& snapshot : runs.all())
        {
            if (snapshot.state != runState::stopping || snapshot.track < 0)
                continue;

            const auto cut = snapshot.skipFooter || underAKill (snapshot);

            /*  A STOP ALREADY ISSUED IS NOT THE LAST WORD WHEN A KILL COMES
                AFTER IT (2026-09-30, namespace draft §23.2). With the panic
                fade at nought - or any fade shorter than the gap between the
                two presses - Esc's stop reaches a mic cue before the second
                press does: its input shut and its reverb ringing out, as Esc
                promises (CG). This loop used to pass over a run whose stop was
                issued, for good, so the kill that followed never reached the
                audio side and the tail rang through the press that promises
                everything is cut (CN). Now it goes through, once, and cuts the
                tail: a rack channel's input stage and plugins, a voice's EQ and
                its inserts, unless a double Esc's sweep has already silenced
                them (since 2026-10-01, §23.6 - it was the stop again). */
            if (snapshot.stopIssued)
            {
                if (cut && ! snapshot.killIssued)
                {
                    if (auto* run = runs.find (snapshot.id))
                        run->killIssued = true;

                    audio->kill (snapshot.track);
                }

                continue;
            }

            const auto held = std::any_of (running.begin(), running.end(),
                                           [&snapshot] (const FadeJob& job)
                                           {
                                               return job.stopWhenDone
                                                        && job.heldRun() == snapshot.id;
                                           });

            /*  BUT A KILL IS NOT HELD (2026-10-01, namespace draft §23.6, GC).
                The hold is what a fade-and-stop is; a kill asks nothing of the
                cue, its fade included, and lands now. Held, the pane's kill of
                a cue ten seconds into a twenty-second fade-out reached the
                voice when the fade would have ended - and as a run that is cut
                is given no level (`applyLevels`), the cue hung at the level the
                kill found it at all that while, then was stopped there, its tail
                ringing. The same for a kill during Esc's own panic fade, a
                MUTE on a sampler clip in its release, and the members a killed
                group is ending. A double Esc never met it: it lets every such
                job go first (`dropStopFades`). The fade runs on to its own end
                with nothing left to stop, since its stop passes over a run
                that has finished. */
            if (held && ! cut)
                continue;

            if (auto* run = runs.find (snapshot.id))
            {
                run->stopIssued = true;
                run->killIssued = cut;
            }

            /*  A KILL IS NOT A STOP (Phase 9b, decision CN): a double Esc, or
                the pane's own kill, silences a mic cue at once with nothing
                left ringing, where Esc lets its tail ring out - and, since
                2026-10-01 (namespace draft §23.6), a media cue too: its output
                silenced and its voice's EQ and inserts emptied - the inserts
                only if the voice is still heard (GE) - where a stop leaves them
                ringing. The two were the same stop on a voice.

                AND A RUN UNDER A KILLED GROUP IS CUT, however it was asked to
                stop (2026-09-30, namespace draft §23.2). Every double Esc is two
                presses, so its kill lands on members Esc has already stopped and
                a panic fade is holding: the kill lets the fade go, this issues
                the stop now due - on the member's own mark, a tick before its
                group's kill reaches it - and the reverb rang on through the
                press that promises everything is cut. */
            if (cut)
                audio->kill (snapshot.track);
            else
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
                    placed, so the silence after that one is the cue finishing.

                    AND A STOPPED RUN'S SILENCE IS ITS END, boundary or not
                    (found by the author on 2026-09-25): a range that loops for
                    ever never finishes, so a killed run on one stayed
                    `stopping` - holding its voice, and still on the running
                    pane after Esc, panic and its own cross - until somebody
                    gave the range a loop count. Once the audio has been told to
                    stop, there is no boundary left to wait for. */
                if (run->range >= 0 && ! run->rangesFinished && ! run->stopIssued)
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

        /*  The one write a fire makes on the document (namespace draft §27). */
        runner.setOverrideDocument (document);

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
        /*  ESC AND DOUBLE ESC, SPECIALISED WITH THE RUNNER (2026-09-28).

            `registerRunCommands` gave them their meaning on the run table: every
            root asked to stop, gracefully or at once. What that cannot do is
            move a level - which is what the author asked Esc to do ("a 'Panic'
            fade duration that fades out all playing cues") - because the fades
            are the Runner's. So the two are taken over here, the registry's own
            "last registration wins", and each does its Runner half FIRST and
            then exactly what it did before: `run.stopAll` fades what sounds over
            `audio/panicFade` and then stops every root, `run.killAll` lets go of
            every stop still to come and then drops every root.

            AND A DOUBLE ESC SWEEPS GO.DOT'S OWN EFFECTS, ONCE (2026-10-01,
            namespace draft §23.6): every voice silenced and every EQ in the
            graph emptied, and the inserts of every rack channel reset - but
            nothing this press leaves armed for the next GO - from here, because
            this is the one place that runs once a press. A voice's inserts are
            covered by its silence and reset by its next arm, not here: a burst
            of resets would fail a plugin every voice shares (GE). A kill
            reaches a run's voice a tick later, through `enforceStops`; the
            sweep is what reaches a tail whose run has already ended. A Player
            call, no record: a replay, with no Player, does the same without
            it.

            WRAPPED, NOT REWRITTEN, so what the earlier registration carries -
            the output test it stops, since 2026-09-21 - comes with it; and a
            rig that registered the run commands alone keeps the plain ones.

            AND WHAT THE STANDBY MADE READY IS LEFT READY by both (2026-09-30,
            namespace draft §23): the plain handlers pass `spareHorizon`, so the
            standby's arm and its prepared block are not among the roots
            stopped - unless somebody reached into the block and something in
            it sounds, which makes it a root like any other. Nothing here needs
            to know: the panic fade takes whatever is sounding, wherever it
            is. */
        for (const auto* level : { "run.stopAll", "run.killAll" })
        {
            const auto* plain = registry.find (level);

            if (plain == nullptr)
                continue;

            auto specialised = *plain;
            const auto graceful = std::string (level) == "run.stopAll";

            if (graceful)
                specialised.description = "Stops every run now, gracefully: Esc. What is sounding fades to"
                                          " silence over audio/panicFade first; members come down in order"
                                          " and every footer runs, but no post-wait; the standby's preparation"
                                          " is left ready. A persistent media cue is paused: the next step"
                                          " carries it on from where it was.";
            else
                specialised.description = "Drops every run now: double Esc. No footer runs, and the world is left"
                                          " as it was; every voice in Go.dot's own graph is silenced and every EQ"
                                          " and rack channel's insert emptied, but the standby's, whose preparation"
                                          " is left ready. What is still waiting to leave - a value a rate cap holds"
                                          " back, a cue's MIDI message - is dropped, and every note a cue started"
                                          " gets its note-off.";

            /*  AND THE LAST GO HEARS IT (2026-10-01, namespace draft §24): after
                an Esc, Doh! moves the pointer back and leaves the runs to Esc,
                whose footers have run.

                AND A DOUBLE ESC DROPS WHAT IS STILL WAITING TO LEAVE (2026-10-02,
                H4, namespace draft §23.10): the network sender's queue - a value
                a rate cap holds back included - and the cues' MIDI messages, with
                a note-off for each note a cue started; a start cue's fire still
                to come; and the osc runs it kills in the very drain that
                launched them are marked `sendDropped` (§24, HQ, L31), their
                message gone with the queue. HERE, IN THE HANDLER, and not in a
                hook: the flush that ends this tick would otherwise send a held
                value whose turn fell on it, and while the clock is down the
                flush still runs and no hook does. AFTER the run table's half, so
                the marks say which roots the press killed and which it spared.
                Nothing is submitted and the applied record is the plain one, so
                a replay - no sender, no sink - writes the same log and marks the
                same runs. Esc drops nothing: it is normal completion entered
                early, and what was queued goes.

                AND IT LETS THE LANE'S FADER GO (2026-10-02, K4, namespace draft
                §23.15): the author's "double Esc would throw away the fader
                association", where Esc keeps it. In the handler too, so the
                fader is the strip's own again from the press's own drain, and a
                replay frees it in the same record. */
            specialised.handler = [&runner, graceful, before = plain->handler]
                                  (CommandContext& context, const std::vector<osc::Value>& args)
            {
                runner.noteEscape();

                /*  AND ESC PAUSES THE PERSISTENT SECTION'S BEDS (2026-10-02, K8,
                    namespace draft §23.17, the author's ruling): their second is
                    remembered before anything is brought down, and the pass a
                    step before the press opened is taken back. */
                if (graceful)
                {
                    runner.pausePersistent (context.tick);
                    runner.beginPanicFade (context.tick);
                    return before (context, args);
                }

                runner.dropStopFades();
                runner.resetEffects();

                auto outcome = before (context, args);

                if (outcome.applied)
                {
                    runner.dropOutputs (context.tick);
                    runner.freeLane();
                }

                return outcome;
            };

            registry.add (std::move (specialised));
        }

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
                            if (cue.getType().toString() != "Media" && cue.getType().toString() != "Mic")
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

                            /*  A JUMP REWRITES THE HISTORY THE LAST GO LIVED IN
                                (2026-10-01, namespace draft §24): that GO is no
                                longer one Doh! can take back, and what a Doh
                                left with devices' operators on this list is
                                forgotten - the jump puts the show somewhere
                                else, and what is due there goes out. And a
                                resume is dropped, its arm revoked (D2). */
                            runner.forgetGoOnJump (engine, context.tick, listId);

                            const auto made = runner.loadToTime (engine, document, context.tick,
                                                                 listId, supplied);

                            std::vector<osc::Value> applied { osc::Value::string (listId) };

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        /*  A SEEK (author, 2026-09-18: "I'd like to be able to scrub active
            cues and groups"). One record per position the hand settles on -
            a client scrubbing sends a handful of these a second and one when
            it lets go - and the same record for both kinds: a media run
            moves to that second of its file, a group run to that second of
            its own timeline with its members re-seated around it, which is
            what brings a member already over back when the hand goes before
            it. The identifiers a group seek draws ride on the applied
            arguments as a jump's do, because a replay never draws its own.

            A fade, a wait, a message have no material to seek in: refused
            `bad-value`, since the row was never one to scrub. */
        registry.add ({ "run.seek",
                        "Moves a run to a second of its own material: a media run to that second"
                        " of its file, a group run to that second of its timeline, its members"
                        " re-seated around it.",
                        { { "run", 's', false }, { "seconds", 'd', false },
                          { "made", 's', true, true } },
                        true,
                        [&engine, &runner] (CommandContext& context,
                                            const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();
                            const auto seconds = args[1].asDouble();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            if (! (seconds >= 0.0))
                                return Outcome::rejected (reason::badValue);

                            const auto* run = runner.runTable().find (runId);

                            if (run == nullptr)
                                return Outcome::rejected (reason::unknownId);

                            std::vector<osc::Value> applied { osc::Value::string (runId),
                                                              osc::Value::float64 (seconds) };

                            /*  Applied and nothing, once it is over: a hand
                                still dragging when the sound ends is not a
                                mistake worth a rejection. */
                            if (run->isFinished())
                                return Outcome::ok (applied);

                            if (run->kind == "media")
                            {
                                /*  A SEEK OF THE ARM A DOH LEFT AT THE POINT
                                    spends the resume on it (D2, GN). */
                                runner.seekingRun (engine, context.tick, runId);
                                runner.seekMedia (engine, context.tick, runId, seconds, true);
                                return Outcome::ok (applied);
                            }

                            /*  AN OSC CUE'S CURVES (namespace draft 45): the
                                clock reads that second from this tick. */
                            if (run->kind == "osc" && runner.isCurving (runId))
                            {
                                runner.seekCurves (runId, seconds, context.tick);
                                return Outcome::ok (applied);
                            }

                            if (run->kind != "group")
                                return Outcome::rejected (reason::badValue);

                            std::vector<std::string> supplied;

                            for (std::size_t n = 2; n < args.size(); ++n)
                                supplied.push_back (args[n].getString());

                            const auto made = runner.seekGroup (engine, context.tick, runId,
                                                                seconds, supplied);

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        /*  THE LIVE RECORDER (author, 2026-09-18: "a 'Live recorder' ... will
            create a sequence/sequential group in a new 'Live recorder'
            playlist that will record all the cue starts ... This can be used
            to store timings triggered once by hand and then automated"), and
            the reframing that made it small: "4 is like dumping the load to
            time history to a group for replay." So it is two commands and no
            machinery of its own: `record.start` turns the history's keeping
            on, unbounded; `record.stop` writes what was kept into a take - a
            TIMELINE group, since a sequence advances on completion and could
            not hold the seconds between two presses - of one start cue per
            step, at the second it was pressed, in a list named "Live
            recorder" that is made the first time. Every identifier the take
            draws rides on the applied arguments in the order it was drawn,
            as a jump's do, and a replay hands them back. */
        registry.add ({ "record.start",
                        "Turns the live recorder on: from now every cue start on every list is"
                        " kept, for record.stop to write into a take.",
                        {},
                        true,
                        [&runner] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            runner.listState().startRecording (context.tick);
                            return Outcome::ok (args);
                        } });

        registry.add ({ "record.stop",
                        "Turns the live recorder off and writes what it kept into a take: a"
                        " timeline group of start cues, one per cue start at the second it"
                        " was pressed, in a list named Live recorder.",
                        { { "made", 's', true, true } },
                        true,
                        [&runner, &document] (CommandContext&,
                                              const std::vector<osc::Value>& args)
                        {
                            if (! runner.listState().isRecording())
                                return Outcome::rejected (reason::badValue);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto since = runner.listState().recordingSinceTick();
                            const auto steps = runner.listState().stopRecording();

                            std::vector<std::string> supplied;

                            for (const auto& value : args)
                                supplied.push_back (value.getString());

                            std::size_t taken = 0;
                            std::vector<osc::Value> applied;

                            /*  An identifier the record supplied, or none -
                                the document draws one - and whichever it was
                                goes on the applied arguments. */
                            const auto next = [&]
                            {
                                const auto id = taken < supplied.size() ? supplied[taken] : std::string {};
                                ++taken;
                                return id;
                            };

                            //  The list, found by name or made.
                            std::string listId;
                            const auto lists = document.root().getChildWithName (juce::Identifier ("Lists"));

                            for (const auto& candidate : lists)
                            {
                                if (candidate.getType().toString() != "List")
                                    continue;

                                const auto id = candidate[idProperty].toString().toStdString();

                                if (document.getAttribute ("/godot/list/" + id + "/name")
                                        .value_or (std::string {}) == "Live recorder")
                                {
                                    listId = id;
                                    break;
                                }
                            }

                            if (listId.empty())
                            {
                                const auto made = document.createList ("Live recorder", next());

                                if (! made.ok)
                                    return Outcome::rejected (made.reason);

                                listId = made.id;
                                applied.push_back (osc::Value::string (listId));
                            }

                            //  The take, after the ones there are.
                            const auto list = document.findById (listId);
                            auto takes = 0;
                            auto members = 0;

                            for (const auto& child : list)
                            {
                                if (child.getType().toString() == "Group")
                                    ++takes;

                                if (isCueElement (child.getType().toString()))
                                    ++members;
                            }

                            const auto take = document.createCue (listId, members, "group",
                                                                  "Take " + std::to_string (takes + 1),
                                                                  next());

                            if (! take.ok)
                                return Outcome::rejected (take.reason);

                            applied.push_back (osc::Value::string (take.id));
                            document.setAttribute ("/godot/cue/" + take.id + "/mode", "timeline");

                            //  One start cue per step, at the second it was pressed.
                            auto at = 0;

                            for (const auto& step : steps)
                            {
                                /*  A DOH! FIRED NOTHING (2026-10-01): the GO it
                                    took back is already out of the take, and the
                                    `d` that says so is not a start to replay. */
                                if (step.origin == 'd')
                                    continue;

                                const auto name = document.getAttribute ("/godot/cue/" + step.cue + "/name")
                                                      .value_or (std::string {});
                                const auto made = document.createCue (take.id, at++, "start",
                                                                      "Start " + (name.empty() ? step.cue : name),
                                                                      next());

                                if (! made.ok)
                                    return Outcome::rejected (made.reason);

                                applied.push_back (osc::Value::string (made.id));
                                document.setAttribute ("/godot/cue/" + made.id + "/target", step.cue);
                                document.setAttribute ("/godot/cue/" + made.id + "/preWait",
                                                       osc::formatDouble (static_cast<double> (std::max<std::int64_t> (0, step.tick - since))
                                                                            / static_cast<double> (TickClock::rateHz)));
                            }

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "run.revoke",
                        "A scene that was only made ready is given back: the pointer moved away"
                        " before a GO, or the scene was stopped before it began. Gives back"
                        " everything the horizon was holding for this run, and finishes it.",
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
        /*  THE PERSISTENT SECTION PUTTING SOMETHING BACK (§3.29, decision S).

            A machine action, and logged as one: the section says a bed should
            be playing and its file ran out, or says the desk should hold a
            value and somebody moved it - so the engine fires the cue again,
            after an applied trigger and never on its own clock.

            IT IS `fire` AND NOT `go`. Standby does not move (§3.5), focus does
            not move, and the cue is fired by name wherever it sits - which is
            the whole difference between a section that asserts and a list that
            runs. The run says `asserted`, so an operator can tell a sound the
            machine put back from one they started. */
        registry.add ({ "run.assert",
                        "The persistent section found a cue not as it declares, and put it back:"
                        " a bed Esc paused carries on from the second of its file its playhead had"
                        " reached, or from as far into the slice it was in - passes and all - as it"
                        " had got.",
                        { { "cue", 's', false }, { "run", 's', true },
                          { "from", 'd', true }, { "range", 'i', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            /*  NOR THE PASS'S RECORD BEHIND A DOUBLE ESC (the
                                review of H4, 2026-10-02, namespace draft §23.10):
                                decided by the hook in the press's tick, it drains
                                after the press, and the next GO is what restores
                                the section (PRD §3.29). Applied and nothing.

                                NOR BEHIND AN ESC (K8, namespace draft §23.17): Esc
                                takes the section down with everything else, and
                                the next step puts it back. */
                            if (context.origin != nullptr && *context.origin == origin::engine
                                  && (runner.killedInDrain (context.tick)
                                        || runner.escapedInDrain (context.tick)))
                                return Outcome::ok (args);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            /*  WHERE A PAUSED BED CARRIES ON FROM (K8): decided by
                                the hook, which knows the file's length and read
                                the playhead (K8's review), and carried here so a
                                replay arms the same second. A record without it -
                                every one before K8 - starts the cue from its top,
                                as it did; one from K8 itself names a slice with
                                nought, its in-point, as it did. */
                            std::optional<Runner::ResumePoint> resume;

                            if (args.size() > 2)
                            {
                                Runner::ResumePoint point;
                                point.from = args[2].asDouble();
                                point.range = args.size() > 3 ? args[3].getInt32() : -1;
                                resume = point;
                            }

                            return Outcome::ok (withRun (args, 1,
                                                         runner.assertCue (engine, context.tick,
                                                                           cueId, id, resume)));
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
                        [&engine, &runner, &document, &focus]
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

                            /*  TOO SOON AFTER THE LAST ONE (PRD §3.7's GO
                                debounce, a show setting since 2026-09-28): a
                                hand that bounced, or two people on two GO
                                buttons, and the second press would fire the
                                cue after the one the operator meant. Refused
                                rather than applied-and-ignored, so the error
                                line says a GO was eaten and the log says whose;
                                the pointer does not move, so the next press
                                fires what this one would have. Half a second
                                unless the show says otherwise (the author's
                                default, 2026-09-28); nought is off. */
                            if (runner.goTooSoon (context.tick))
                                return Outcome::rejected (reason::tooSoon);

                            /*  THE GO RECORD, OPENED BEFORE THE FIRE (2026-10-01,
                                namespace draft §24): what Doh! would take back
                                - the list, the cue, where the pointer and the
                                debounce stood, whether the list had run out -
                                read before anything below writes over it. It
                                notes the GO for the debounce, as `noteGo` did. */
                            const auto finishedBefore = static_cast<bool> (list[juce::Identifier ("finished")]);
                            runner.beginGo (context.tick, listId, standby, finishedBefore);

                            /*  STANDBY MOVES FIRST, and unconditionally (§3.5).
                                Whether the cue makes a sound, fails to find a
                                voice, or is a memo, the pointer has advanced -
                                which is what lets an operator press GO down a
                                list at speed without waiting to see what each
                                one did. */
                            /*  The run table, so that a manual group with
                                rounds left keeps the pointer instead of letting
                                it out on the last member of round one.

                                `standbyAfterFiring` and NOT `nextStandby`:
                                firing the last cue leaves the pointer nowhere,
                                which is the resting state and is what makes the
                                do-nothing GO above honest. The arrows keep the
                                other answer, because looking is not firing. */
                            const auto next = standbyAfterFiring (list, standby,
                                                                  &runner.runTable());
                            document.setAttribute (standbyAddressOf (listId), next);

                            /*  AND WHY IT IS EMPTY, WHEN IT IS. An empty
                                pointer means two opposite things - a list
                                nobody armed, and a list that has been all the
                                way through - and the persistent solver reads
                                one of them as the top of the list. Without
                                this, a show run to its end would re-assert
                                every bed a Stop had suspended. */
                            document.setAttribute (finishedAddressOf (listId),
                                                   next.empty() ? "true" : "false");

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

                            /*  A CUE A DOH PAUSED IS CARRIED ON (2026-10-02, D2,
                                namespace draft §24): from where it was at the
                                press, over a 0.1 s de-click - in place inside
                                the Doh fade, through the arm the standby made at
                                the point after it, or seated there; a scene
                                re-seated where it was. Otherwise the GO it always
                                was. Every identifier either road draws is on the
                                record, as ever. */
                            Runner::Resumed outcome;
                            auto backTo = context.tick;

                            if (const auto* mark = runner.markFor (listId, standby))
                            {
                                const auto paused = *mark;
                                backTo = paused.stepTick + (context.tick - paused.dohTick);
                                outcome = runner.resumeStandby (engine, context.tick, list, paused, supplied);
                            }
                            else
                            {
                                outcome.made = runner.fireStandby (engine, context.tick, list, standby, supplied);
                            }

                            const auto& made = outcome.made;

                            /*  AND CLOSED AFTER IT: what the GO made, and what it
                                reached; the list's resume spent or dropped. The
                                serial is read first, for the step. */
                            const auto serial = runner.goInHand();
                            runner.endGo (engine, context.tick, made, outcome.carriedOn);

                            /*  A STEP, WRITTEN BY THE HANDLER. §3.13's manual
                                waypoints, kept for the operator rather than by
                                them (decision R): every applied GO is a place to
                                go back to, and it is model state a replay
                                reproduces precisely because the handler writes
                                it and no hook has to notice it. It carries its
                                GO, which is how Doh! finds it again wherever a
                                seek has moved it (§24).

                                A GO THAT CARRIED A PAUSED CUE ON is back-dated
                                (D2): the step the Doh erased, moved forward by
                                the time the cue spent paused - so a later jump
                                places the cue where it plays. Only when it really
                                was carried on: a fall-back to the top starts now. */
                            if (outcome.resumed)
                                runner.listState().steppedAt (listId, { backTo, standby, 'g', serial });
                            else
                                runner.listState().stepped (listId, { context.tick, standby, 'g', serial });

                            std::vector<osc::Value> applied;

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        /*  DOH! (PRD §3.32, namespace draft §24; the author, 2026-09-30): the
            third level of stop, and the recovery one - the last GO taken back,
            inside the show's `list/dohWindow`. Its own command, its own button
            and its own key: Undo never touches a GO.

            A COMMAND IN THE `go` FAMILY (§4.11), and like GO it is accepted
            under the lock - it writes only the pointer, the list's `finished`
            and the history, where the operator is standing - and through an
            audio outage, as Esc is. Refused with a word, never applied-and-
            ignored, so the line at the foot of the window says why: too soon
            after the last Doh!, nothing to take back, a trigger fired after the
            GO, too late, or the pointer's own refusal when the cue is gone.

            VARIADIC, like `go` and `list.loadToTime`: from D3 the put-back draws
            identifiers - a cue the GO stopped, started again - and the record
            carries them. In D1 it draws none, and a replay re-runs the handler
            on the same state to the same answer. */
        registry.add ({ "go.doh",
                        "Doh!: takes back the last GO, within list/dohWindow of it - the pointer, the"
                        " list's finished flag, the GO debounce and the history go back, what it started"
                        " comes down with no footer, and what it sent to a device left to its operator"
                        " is not sent again. Undo never touches a GO.",
                        { { "run", 's', true, true } },
                        true,
                        [&engine, &runner, &document]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            /*  THE IDENTIFIERS ITS PUT-BACK DRAWS (D3): supplied on a
                                replay, drawn live, and carried on the applied record. */
                            std::vector<std::string> supplied;

                            for (const auto& value : args)
                                supplied.push_back (value.getString());

                            std::vector<std::string> drawn;
                            const auto refusal = runner.goDoh (engine, document, context.tick, supplied, drawn);

                            if (! refusal.empty())
                                return Outcome::rejected (refusal);

                            std::vector<osc::Value> applied;

                            for (const auto& id : drawn)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        /*  A SCENE THE GO STOPPED, PUT BACK ONCE IT HAS ENDED (2026-10-03, Doh!
            D3, namespace draft §24.13). Doh! found it still ending - its members
            down, its footer running - and a relaunch then would have seated the
            scene under the release of the old one. So the engine waits for the
            scene's run to end and submits this; the handler relaunches it at
            the second it would have reached now, and the record carries the
            identifiers it drew, so a replay - which runs no hook, and re-injects
            this record after the same `run.ended` - makes the same. Applied and
            nothing when no put-back names the run any more: a GO on its list, a
            jump or Esc came first. */
        registry.add ({ "go.dohRelaunch",
                        "A scene the GO a Doh! took back had stopped, put back where it would be now once"
                        " it has ended - submitted by the engine, with the identifiers the relaunch drew.",
                        { { "run", 's', false }, { "made", 's', true, true } },
                        true,
                        [&engine, &runner] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            std::vector<std::string> supplied;

                            for (std::size_t at = 1; at < args.size(); ++at)
                                supplied.push_back (args[at].getString());

                            std::vector<std::string> drawn;
                            runner.dohRelaunch (engine, context.tick, runId, supplied, drawn);

                            std::vector<osc::Value> applied { osc::Value::string (runId) };

                            for (const auto& id : drawn)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

        //----------------------------------------------------------------------
        /*  WHAT THE LAST DOH! PUT BACK, AND WHAT IT LEFT (2026-10-03, D3): one
            sentence, composed by the engine on the tick after the press - or
            after the relaunch of a scene that followed it - and set on the
            runner-wide readout `/godot/list/dohReport` by this handler. A
            record, so a replay rebuilds the same readout. */
        /*  (2026-10-03, D4's review, OJ, OK: `append`, the relaunch's, adds its
            sentence after what the readout holds - "...; then: ..." - where the
            Doh's own replaces it; and an empty sentence that does not append
            clears the readout: a Doh with nothing to say.) */
        registry.add ({ "list.dohReport",
                        "What the last Doh! put back and what it left alone, in one sentence - submitted by"
                        " the engine on the tick after the press, and read on /godot/list/dohReport. With"
                        " append, a relaunch's sentence follows the one that stands; an empty one clears it.",
                        { { "list", 's', false }, { "text", 's', false }, { "append", 'T', true } },
                        false,
                        [&runner] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto append = args.size() > 2 && args[2].getBool();
                            const auto& standing = runner.listState().dohReport();
                            const auto& text = args[1].getString();

                            if (append && ! standing.text.empty())
                            {
                                if (! text.empty())
                                    runner.listState().setDohReport ({ standing.list, context.tick,
                                                                       standing.text + "; then: " + text });
                            }
                            else if (text.empty())
                                runner.listState().setDohReport ({});
                            else
                                runner.listState().setDohReport ({ args[0].getString(), context.tick, text });

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  WHERE A SOUND DOH! PAUSED HAD GOT TO (2026-10-02, D2, namespace draft
            §24; the author's playhead rule of K8, LQ): read off the run's own
            playhead by the engine on the tick after the press - before the Doh
            fade had moved it - and written here into the resume the next GO
            carries on. A record, so a replay, which has no playhead, carries the
            same second on. `range` is the slice it was in, with `from` how far
            into it, passes and all; -1 for a cue with no slices, `from` a second
            of its file. A run no resume names is applied and changes nothing. */
        registry.add ({ "go.dohPlayhead",
                        "Where a sound Doh! paused had got to at the press, read off its playhead by the"
                        " engine on the next tick: where the next GO carries it on from.",
                        { { "run", 's', false }, { "from", 'd', false }, { "range", 'i', true } },
                        true,
                        [&engine, &runner] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            const auto from = args[1].asDouble();

                            if (! (from >= 0.0))
                                return Outcome::rejected (reason::badValue);

                            runner.notePlayheadOf (engine, context.tick, runId, from,
                                                   args.size() > 2 ? args[2].getInt32() : -1);
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "cue.fire",
                        "Fires a named cue without touching standby - what a button on a surface"
                        " does. A start cue's fire carries the GO it fires under as its cause.",
                        /*  THE CAUSE (2026-10-01, namespace draft §24): the serial
                            of the GO whose start cue fired this one, carried in
                            the record so a replay - which runs no hook - reads
                            it; read only from the engine's own fires. */
                        { { "cue", 's', false }, { "run", 's', true }, { "cause", 'h', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto from = context.origin != nullptr ? *context.origin : std::string {};
                            const auto cause = from == origin::engine && args.size() > 2 && args[2].isInt64()
                                                 ? static_cast<std::uint64_t> (std::max<std::int64_t> (0, args[2].getInt64()))
                                                 : std::uint64_t { 0 };

                            /*  A START CUE'S TARGET, FIRED UNDER A GO THAT DOH! HAS
                                TAKEN BACK (§24): the fire was queued for the next
                                tick before the Doh, and fires nothing - no run,
                                no step, no press. First of all, before any other
                                answer, so nothing below acts on it. */
                            if (cause != 0 && runner.causeTakenBack (cause))
                                return Outcome::ok (args);

                            /*  NOR THE ENGINE'S OWN FIRE BEHIND A DOUBLE ESC (the
                                review of H4, 2026-10-02, namespace draft §23.10):
                                a start cue's fire the hook submitted in this tick,
                                before the press, drains after it - and would make
                                a fresh run nothing has marked, sounding after the
                                press that drops every action. Applied and nothing.
                                A fire by name - a hand - still fires. */
                            if (from == origin::engine && runner.killedInDrain (context.tick))
                                return Outcome::ok (args);

                            /*  A CUE THAT IS OFF FOR THIS RUN IS NOT FIRED
                                (namespace draft §27, PP): its file has it
                                disabled, or a disable cue has switched it off.
                                GO could never reach one, since the pointer does
                                not stand on it; a name, a trigger and a start
                                cue could, until 2026-10-05. */
                            if (! runsNow (cue))
                                return Outcome::rejected (reason::disabled);

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

                            /*  A SAMPLER MEMBER FIRED BY NAME IS A PRESS on the
                                strip it holds (plan decision 10) - which is what
                                a take from the live recorder replays - and with
                                no strip under it there is nowhere to play it
                                from. The press records its own step. */
                            if (runner.isSamplerMember (cueId))
                            {
                                const auto refusal = runner.pressMember (engine, context.tick, cueId, from);

                                /*  A PAD FIRED BY NAME IS A FIRE BY NAME (§24):
                                    when the press was taken, the last GO on its
                                    list is being played on. */
                                if (refusal.empty())
                                    runner.noteFireOnList (cueId, from, cause);

                                return refusal.empty() ? Outcome::ok ({ args[0] })
                                                       : Outcome::rejected (refusal);
                            }

                            /*  A FIRE ON THE LAST GO'S LIST, BY NAME OR BY A START
                                CUE, AFTER IT (the author, 2026-09-30): Doh! then
                                refuses with a sentence rather than undo a GO the
                                show has moved on from - unless the cause is that
                                GO itself, whose start cue this is. And what a Doh
                                left with a device's operator is let go, or filed
                                under the GO that reached it (§24, HP). */
                            runner.noteFireOnList (cueId, from, cause);
                            runner.markFire (engine, context.tick, cueId, from, cause);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            /*  A cue fired by name is a step too, on the list
                                that holds it: going back to "before the shot"
                                is as much a place as going back to a GO. A start
                                cue's carries the GO that fired the start cue. */
                            if (const auto listId = runner.listOfCue (cueId); ! listId.empty())
                                runner.listState().stepped (listId,
                                                            { context.tick, cueId, 'f', cause });

                            runner.setFireCause (cause);
                            const auto made = runner.fire (engine, context.tick, cueId, id);
                            runner.setFireCause (0);

                            return Outcome::ok (withRun (args, 1, made));
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
        /*  A JUMP CUE'S MOVE (2026-10-05, namespace draft §27): submitted by the
            hook on the tick after the jump cue fired, as a start cue's `cue.fire`
            is, and never `list.loadToTime`, which forgets the GO and would leave
            Doh! nothing to take back. Standby onto the target on the jump cue's
            own list; with andGo, that cue fired as GO fires it, under the GO
            the jump cue belongs to. The identifiers its fire draws ride on the
            applied record after the four arguments, as GO's do. */
        registry.add ({ "standby.jump",
                        "A jump cue moves its list's standby onto its target, and with andGo fires"
                        " it as GO would, under the GO that fired the jump cue. Sent by the engine.",
                        { { "list", 's', false }, { "target", 's', false }, { "andGo", 'T', false },
                          { "cause", 'h', false }, { "run", 's', true, true } },
                        true,
                        [&engine, &runner, &document]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto from = context.origin != nullptr ? *context.origin : std::string {};
                            const auto cause = args[3].isInt64()
                                                 ? static_cast<std::uint64_t> (std::max<std::int64_t> (0, args[3].getInt64()))
                                                 : std::uint64_t { 0 };

                            /*  UNDER A GO DOH! HAS TAKEN BACK, or behind a double
                                Esc in this tick: queued before the press, and
                                moving nothing after it - the start cue's two
                                answers, for its two reasons. */
                            if ((cause != 0 && runner.causeTakenBack (cause))
                                  || (from == origin::engine && runner.killedInDrain (context.tick)))
                                return Outcome::ok (args);

                            std::vector<std::string> supplied;

                            for (std::size_t at = 4; at < args.size(); ++at)
                                supplied.push_back (args[at].getString());

                            std::vector<std::string> made;
                            const auto refusal = runner.jumpStandby (engine, document, context.tick,
                                                                     args[0].getString(), args[1].getString(),
                                                                     args[2].isBool() && args[2].getBool(), cause,
                                                                     supplied, made);

                            if (! refusal.empty())
                                return Outcome::rejected (refusal);

                            std::vector<osc::Value> applied (args.begin(), args.begin() + 4);

                            for (const auto& id : made)
                                applied.push_back (osc::Value::string (id));

                            return Outcome::ok (applied);
                        } });

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

                            /*  NOR A CUE THAT IS OFF FOR THIS RUN, as `cue.fire`
                                refuses it (namespace draft §27, PP). */
                            if (! runsNow (cue))
                                return Outcome::rejected (reason::disabled);

                            /*  A MANUAL SEQUENCE GROUP HAS NOBODY TO BE ITS
                                PARENT, the same refusal `cue.fire` gives and for
                                the same reason: its members start on GO, one
                                press at a time, and fired from here it would run
                                its header, start its first member and wait for a
                                press that is never coming. */
                            if (cue.getType().toString() == "Group"
                                  && runner.isManualGroup (cue))
                                return Outcome::rejected (reason::needsGo);

                            /*  A trigger on a sampler member presses its strip,
                                as `cue.fire` does. A trigger has no release, so
                                a hold clip it starts plays out (§3.27). */
                            const auto from = context.origin != nullptr ? *context.origin : std::string {};

                            if (runner.isSamplerMember (cueId))
                            {
                                const auto refusal = runner.pressMember (engine, context.tick, cueId, from);

                                if (refusal.empty())
                                    runner.noteFireOnList (cueId, from, 0);

                                return refusal.empty() ? Outcome::ok ({ args[0] })
                                                       : Outcome::rejected (refusal);
                            }

                            /*  A TRIGGER ON THE LAST GO'S LIST, AFTER IT, makes
                                Doh! refuse (the author, 2026-09-30), and fires
                                whatever a Doh left with a device's operator: a
                                trigger is a deliberate send (§24). */
                            runner.noteFireOnList (cueId, from, 0);
                            runner.markFire (engine, context.tick, cueId, from, 0);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            /*  And a trigger. The letter is the only difference,
                                and it is worth keeping: a history that said
                                which steps nobody pressed is a history that can
                                explain a scene starting on its own. */
                            if (const auto listId = runner.listOfCue (cueId); ! listId.empty())
                                runner.listState().stepped (listId,
                                                            { context.tick, cueId, 't' });

                            return Outcome::ok (withRun (args, 1,
                                                         runner.fire (engine, context.tick,
                                                                      cueId, id)));
                        } });

        //----------------------------------------------------------------------
        /*  A HAND ON A SAMPLER STRIP (PRD §3.27, Phase 6): a pad hit, a button
            pressed, a fader lifted from the bottom - and let go. Sent by a
            surface, the virtual panel, the page, and by the engine itself for
            a fader's edges; the origin is who owns a held clip, so it is read
            from the context rather than trusted from an argument. Neither
            moves the standby (§3.5): a sampler is played beside the list, not
            through it. */
        registry.add ({ "strip.press",
                        "A hand on a sampler strip: the clip on it starts, at a level its velocity"
                        " sets when the clip asks for that.",
                        { { "strip", 's', false }, { "velocity", 'i', true } },
                        true,
                        [&engine, &runner] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto velocity = args.size() > 1 ? args[1].getInt32() : -1;

                            if (args.size() > 1 && (velocity < 0 || velocity > 127))
                                return Outcome::rejected (reason::badValue);

                            const auto refusal = runner.pressStrip (engine, context.tick,
                                                                    args[0].getString(), velocity,
                                                                    context.origin != nullptr
                                                                      ? *context.origin
                                                                      : std::string {});

                            return refusal.empty() ? Outcome::ok (args) : Outcome::rejected (refusal);
                        } });

        registry.add ({ "strip.release",
                        "The hand lets go of a sampler strip: a hold clip stops, after a short fade.",
                        { { "strip", 's', false } },
                        true,
                        [&engine, &runner] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto refusal = runner.releaseStrip (engine, context.tick,
                                                                      args[0].getString(),
                                                                      context.origin != nullptr
                                                                        ? *context.origin
                                                                        : std::string {});

                            return refusal.empty() ? Outcome::ok (args) : Outcome::rejected (refusal);
                        } });

        /*  A VOICE FREED, AND A SAMPLER MEMBER WAITING FOR ONE TAKES IT
            (decision Z). What the scheduler sends itself when a track frees;
            anyone may, as with every engine-origin command. AND A RACK CHANNEL
            FREED (Phase 9b): a mic cue waiting for it takes it by the same
            door. */
        registry.add ({ "run.arm",
                        "A sampler member waiting for a voice takes the track that has come free;"
                        " a mic cue waiting for its rack channel takes the channel.",
                        { { "run", 's', false } },
                        true,
                        [&engine, &runner] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto runId = args[0].getString();

                            if (! runner.knowsRun (runId))
                                return Outcome::rejected (reason::unknownId);

                            runner.armAgain (engine, runId);
                            return Outcome::ok (args);
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
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            const auto parentRun = args[0].getString();
                            const auto cueId = args[1].getString();

                            if (! runner.knowsRun (parentRun))
                                return Outcome::rejected (reason::unknownId);

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto id = args.size() > 2 ? args[2].getString()
                                                            : std::string {};

                            /*  The record's own spawn: one a group's job decided
                                before a Doh drained in this tick is born done. */
                            constexpr auto fromItsRecord = true;

                            return Outcome::ok (withRun (args, 2,
                                                         runner.spawnChild (engine, parentRun,
                                                                            cueId, id, context.tick,
                                                                            fromItsRecord)));
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

                            /*  DECIDED BEFORE A DOH, ON A STATE IT HANDED BACK in
                                this very tick (2026-10-02, D2, GZ): applied, and
                                nothing - the run is a preparation again. */
                            if (runner.handedBackIn (runId, context.tick))
                                return Outcome::ok (args);

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

                            /*  A PRE-WAIT'S END THE HOOK DECIDED BEFORE A DOH
                                HANDED THE RUN BACK in this very tick (D2, GZ):
                                applied, and nothing - its pre-wait is whole
                                again. */
                            if (runner.handedBackIn (runId, context.tick))
                                return Outcome::ok (args);

                            runner.fireNow (engine, context.tick, runId);
                            return Outcome::ok (args);
                        } });
    }
}
