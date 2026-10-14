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
    A FOLLOWER SEATED AT ITS LEADER'S POSITION, AND WHETHER IT STAYS THERE
    (namespace draft §58, RD.8).

    What is pinned here: a position made from an engine with lists, a bed
    looping in a range, a timeline scene and a sequence going, DCA trims, the
    faders flipped to a cue and the run-name salt reads back as it was written,
    its numbers unmoved by a French locale; seated into an engine that was
    elsewhere, it ends that engine's own runs with no footer, builds the
    leader's under the leader's names at their playheads, writes the standby,
    draws the leader's names from then on, and is one `pair.seat` record a
    replay seats alike; two engines joined by the real stream and receiver, a
    follower that joins late is seated, its run table the leader's on every
    tick for two hundred ticks, in step by the summaries - and a run stopped on
    it by hand puts it out of step, said in a sentence, until a realign seats
    it again; the summary's wire and its sentences; a position of two hundred
    runs in parts and whole again; what it all costs. And (namespace draft
    §60, TG.5) each incoming code's map as a `code` line of its own, moved
    with the ticks, the format bumped and an older one refused in a sentence,
    and handed over by a seat.

    Every case runs under fr_FR as well as C: a position's numbers are text.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/TextFields.h>
#include <wfg/engine/cue/UndoStep.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/log/Replay.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/pair/Follow.h>
#include <wfg/engine/pair/PairSettings.h>
#include <wfg/engine/pair/PeerLink.h>
#include <wfg/engine/pair/Position.h>
#include <wfg/engine/pair/Summary.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace wfg;

namespace
{
    using Clock = pair::LinkClock;
    using namespace std::chrono_literals;

    osc::Value s (std::string text) { return osc::Value::string (std::move (text)); }

    const Clock::time_point t0 = Clock::time_point {} + std::chrono::hours { 1000 };

    /*  THE AUDIO SIDE WITH NOBODY LISTENING: an arm completes the tick after it
        is asked, a launched voice plays until it is stopped - so a stop, a kill
        and a seat's ending are seen ending, and a bed plays for ever. */
    struct QuietPlayer final : cue::Player
    {
        int trackCount() const override                     { return 64; }
        void requestArm (const cue::ArmRequest& request) override { arms.push_back (request); }
        int slotCount() const override                      { return 2; }
        bool launchAtSample (int track, int, std::int64_t) override { playing.insert (track); return true; }
        bool stop (int track) override                      { playing.erase (track); return true; }
        bool stopAtSample (int track, int, std::int64_t) override { playing.erase (track); return true; }
        void setLevelDb (int, double) override              {}
        void setRouting (int, const std::vector<cue::Coefficient>&) override {}
        bool isPlaying (int track) const override           { return playing.count (track) > 0; }
        bool isArmReady (int track) const override          { return ready.count (track) > 0; }
        std::int64_t samplesElapsed() const override        { return samples; }
        int blockSize() const override                      { return 128; }
        int sampleRate() const override                     { return 48000; }
        int channelsPerTrack() const override               { return 2; }

        void completeArms (Engine& engine)
        {
            for (const auto& arm : arms)
            {
                engine.submit (origin::engine, "audio.armed", { s (arm.runId), osc::Value::int32 (arm.track) });
                ready.insert (arm.track);
            }

            arms.clear();
        }

        /*  A LINE FOR LTC (namespace draft §59, TC.7): every code started on
            it, its program, its first sample and its first frame. */
        struct Code
        {
            std::uint64_t program = 0;
            std::int64_t zero = 0;
            std::int64_t frame = 0;
        };

        bool placeTimecode (std::uint64_t program, std::int64_t zero, std::int64_t frame, timecode::Rate) override
        {
            codes.push_back ({ program, zero, frame });
            return true;
        }

        std::vector<cue::ArmRequest> arms;
        std::set<int> ready, playing;
        std::vector<Code> codes;
        std::int64_t samples = 0;
    };

    /*  ONE MACHINE: its own engine, show, run table, sound, DCAs and faders,
        the show read from one text, the undo rule serve's. With no sound, a
        machine as `wfg replay` is. */
    struct Machine
    {
        explicit Machine (const std::string& showText, bool withSound = true)
        {
            engine.log().openInMemory ({});
            REQUIRE (doc::CanonicalXml::read (showText, document).ok);

            doc::registerDocumentCommands (engine.commands(), document);
            cue::registerCueCommands (engine.commands(), document, focus);
            cue::registerRunCommands (engine.commands(), runs);
            cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);
            pair::registerPairCommands (engine.commands(), pairing);

            if (withSound)
                runner.setPlayer (&audio);

            runner.setSamplesPerTick (960);
            runner.setDcas (&dcas);
            runner.setLanes (&lanes);

            engine.setBeforeApply ([this] (const Command& command, const Event& event,
                                           const std::vector<osc::Value>& args, std::int64_t tick)
                                   { cue::beginUndoStep (document, nullptr, command, event, args, tick); });
        }

        /*  One tick, as serve runs one - without the Runner's hooks on the tick
            a seat goes in, as serve leaves them out then. */
        void tickOnce (std::int64_t tick, bool hooks = true)
        {
            audio.samples = tick * 960;

            if (hooks)
                runner.beforeTick (engine, tick);

            engine.processTick (tick);
            audio.completeArms (engine);
        }

        void go (std::int64_t tick, const std::string& listId, const std::string& cueId)
        {
            REQUIRE (engine.submit (std::string (origin::cli), "node.set", { s (cue::standbyAddressOf (listId)), s (cueId) }));
            tickOnce (tick);
            REQUIRE (engine.submit (std::string (origin::cli), "go"));
            tickOnce (tick + 1);
        }

        /** The live run of a cue, or empty. */
        std::string runOf (const std::string& cueId) const
        {
            for (const auto& run : runs.all())
                if (run.cue == cueId && ! run.isFinished() && ! run.onlyPrepared())
                    return run.id;

            return {};
        }

        std::string standbyOf (const std::string& listId) const
        {
            return document.getAttribute (cue::standbyAddressOf (listId)).value_or (std::string {});
        }

        Engine engine;
        doc::ShowDocument document;
        cue::RunTable runs;
        cue::Focus focus;
        doc::IdRegistry runIds { doc::IdRegistry::withSeed (11) };
        cue::Runner runner { document, runs, runIds, focus };
        QuietPlayer audio;
        cue::DcaTable dcas;
        cue::LaneTable lanes;
        pair::Pairing pairing;
    };

    /*  A SHOW WITH EVERYTHING A POSITION CARRIES: a bed looping in a range for
        ever; a timeline scene - a sound at once, another two seconds in, two
        memos at three and six; a sequence of memos that steps on by itself,
        each holding its post-wait, so the engine names its members as it
        goes; a scene with a long member and a footer; a memo after; a DCA. */
    struct Show
    {
        Show()
        {
            doc::ShowDocument show;
            list = show.createList ("Sound").id;

            bed = show.createCue (list, 0, "media", "Bed").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + bed + "/file", "bed.wav").ok);
            const auto range = show.createRange (bed, 2.0, 2.5);
            REQUIRE (range.ok);
            REQUIRE (show.setAttribute ("/godot/range/" + range.id + "/loops", "0").ok);

            scene = show.createCue (list, 1, "group", "Scene").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + scene + "/mode", "timeline").ok);
            rain = show.createCue (scene, 0, "media", "Rain").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav").ok);
            thunder = show.createCue (scene, 1, "media", "Thunder").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + thunder + "/file", "thunder.wav").ok);
            REQUIRE (show.setAttribute ("/godot/cue/" + thunder + "/preWait", "2").ok);
            lightsA = show.createCue (scene, 2, "memo", "Lights A").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + lightsA + "/preWait", "3").ok);
            lightsB = show.createCue (scene, 3, "memo", "Lights B").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + lightsB + "/preWait", "6").ok);

            walk = show.createCue (list, 2, "group", "Walk").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + walk + "/advance", "auto").ok);

            for (int n = 0; n < 6; ++n)
            {
                const auto step = show.createCue (walk, n, "memo", "Step " + std::to_string (n + 1)).id;
                REQUIRE (show.setAttribute ("/godot/cue/" + step + "/postWait", "1").ok);
                steps.push_back (step);
            }

            other = show.createCue (list, 3, "group", "Other").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + other + "/advance", "auto").ok);
            hold = show.createCue (other, 0, "memo", "Hold").id;
            REQUIRE (show.setAttribute ("/godot/cue/" + hold + "/preWait", "60").ok);
            const auto footer = show.createRole (other, "footer");
            REQUIRE (footer.ok);
            release = show.createCue (footer.id, 0, "memo", "Release").id;

            next = show.createCue (list, 4, "memo", "Next").id;
            after = show.createCue (list, 5, "memo", "After").id;

            dca = show.createDca ("Band").id;
            REQUIRE (! dca.empty());

            REQUIRE (show.setAttribute ("/godot/list/goDebounce", "0").ok);
            text = doc::CanonicalXml::write (show);
        }

        std::string text, list, bed, scene, rain, thunder, lightsA, lightsB, walk, other, hold, release, next, after, dca;
        std::vector<std::string> steps;
    };

    /*  THE LEADER'S EVENING: the bed, the scene, the walk; a DCA ridden and the
        faders flipped to the bed. */
    void playEvening (Machine& leader, const Show& show, std::int64_t& tick)
    {
        leader.go (tick, show.list, show.bed);
        tick += 20;
        leader.go (tick, show.list, show.scene);
        tick += 20;
        leader.go (tick, show.list, show.walk);
        tick += 2;

        leader.dcas.set (show.dca, -4.5);
        leader.lanes.flip (show.bed);
        leader.lanes.setArmed (cue::levelLaneKey, true);
    }

    struct RunLine
    {
        std::string id, cue, state, parent;

        bool operator== (const RunLine& other) const
        {
            return id == other.id && cue == other.cue && state == other.state && parent == other.parent;
        }
    };

    /** The runs a summary would say: live, and not only made ready. */
    std::map<std::string, RunLine> liveLines (const cue::RunTable& runs)
    {
        std::map<std::string, RunLine> out;

        for (const auto& run : runs.all())
            if (! run.isFinished() && ! run.onlyPrepared())
                out[run.id] = { run.id, run.cue, run.state, run.parent };

        return out;
    }

    std::string spelled (const std::map<std::string, RunLine>& lines)
    {
        std::string out;

        for (const auto& [id, line] : lines)
            out += id + " " + line.cue + " " + line.state + " under " + (line.parent.empty() ? "-" : line.parent) + "\n";

        return out;
    }

    std::vector<LogRecord> recordsOf (Engine& engine, const std::string& command)
    {
        std::vector<LogRecord> out;

        for (auto& record : LogFile::parse (engine.log().contents()).records)
            if (record.command == command)
                out.push_back (std::move (record));

        return out;
    }
}

//==============================================================================
TEST_CASE ("seat: a position reads back as it was written - lists, runs, rounds, fades, trims, faders, names - in any locale")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const Show show;
    Machine leader { show.text };
    std::int64_t tick = 50;
    playEvening (leader, show, tick);

    for (; tick < 200; ++tick)
        leader.tickOnce (tick);

    const auto at = leader.runner.positionNow (tick, 41);
    const auto text = pair::toText (at);

    //  What it carried: the rows, a list's history, the bed in its slice, the scene, the walk, the trims, the faders.
    CHECK (at.counter == 41u);
    CHECK (at.tick == tick);
    CHECK (std::any_of (at.rows.begin(), at.rows.end(), [&show] (const pair::Position::Row& row)
                        { return row.address == cue::standbyAddressOf (show.list); }));
    CHECK (std::none_of (at.rows.begin(), at.rows.end(), [] (const pair::Position::Row& row)
                         { return row.address.find ("DeviceId") != std::string::npos
                                    || row.address.find ("Settled") != std::string::npos; }));
    REQUIRE (at.lists.size() == 1u);
    CHECK (at.lists.front().history.size() >= 3u);

    const auto* bed = at.runNamed (leader.runOf (show.bed));
    REQUIRE (bed != nullptr);
    CHECK (bed->state == cue::runState::playing);
    CHECK (bed->range == 0);
    CHECK (bed->slicePlayed > 1.0);              // looped round its half second more than twice

    const auto* scene = at.runNamed (leader.runOf (show.scene));
    REQUIRE (scene != nullptr);
    CHECK (scene->kind == "group");
    CHECK (scene->iteration == 1);
    CHECK (! scene->round.empty());
    CHECK (std::any_of (at.groups.begin(), at.groups.end(), [scene] (const cue::GroupJob& job) { return job.run == scene->id; }));

    CHECK (at.dcas.count (show.dca) == 1u);
    CHECK (at.laneCue == show.bed);
    CHECK (at.laneArmed == std::vector<std::string> { cue::levelLaneKey });
    CHECK (at.names.salt == leader.runner.runNames().salt());
    CHECK (! at.names.counts.empty());

    //  AND READ BACK, every value: the same text written again, and the same values.
    std::string why;
    const auto read = pair::readPosition (text, &why);
    INFO (why);
    REQUIRE (read.has_value());
    CHECK (pair::toText (*read) == text);
    CHECK (read->names == at.names);
    CHECK (read->runs.size() == at.runs.size());
    CHECK (read->dcas == at.dcas);
    CHECK (read->runNamed (bed->id)->slicePlayed == doctest::Approx (bed->slicePlayed).epsilon (1e-12));
    CHECK (std::bit_cast<std::uint64_t> (read->dcas.at (show.dca)) == std::bit_cast<std::uint64_t> (-4.5));

    //  A NUMBER WITH A POINT, whatever the locale: never a comma inside a d: atom.
    CHECK (text.find ("d:-4.5") != std::string::npos);
    CHECK (text.find ("d:-4,5") == std::string::npos);

    //  REBASED: every tick moved, nothing else.
    auto moved = *read;
    moved.rebase (1000);
    CHECK (moved.tick == at.tick + 1000);
    CHECK (moved.runNamed (scene->id)->launchTick == scene->launchTick + 1000);
    CHECK (moved.lists.front().history.front().tick == at.lists.front().history.front().tick + 1000);

    //  REFUSED, never half read: a text cut short, a format this build does not write, a line that is not one.
    CHECK_FALSE (pair::readPosition (text.substr (0, text.size() / 2)).has_value());
    const auto format = std::to_string (pair::Position::format);
    CHECK_FALSE (pair::readPosition ("position i:" + std::to_string (pair::Position::format + 1) + " h:1 h:1\nend\n").has_value());
    CHECK_FALSE (pair::readPosition ("position i:" + format + " h:1 h:1\nrun s:\"A\"\nend\n").has_value());
    CHECK (pair::readPosition ("position i:" + format + " h:1 h:1\nend\n").has_value());
}

//==============================================================================
namespace
{
    /** Every preparation the horizon has made, by name and cue. */
    std::set<std::pair<std::string, std::string>> preparationsOf (const cue::RunTable& runs)
    {
        std::set<std::pair<std::string, std::string>> out;

        for (const auto& run : runs.all())
            if (! run.isFinished() && run.onlyPrepared())
                out.emplace (run.id, run.cue);

        return out;
    }

    std::string differenceAt (Machine& leader, std::int64_t leaderTick, Machine& follower, std::int64_t localTick)
    {
        pair::Labels labels;
        labels.cue = [&follower] (const std::string& id) { return follower.runner.labelOf (id); };

        auto theirs = leader.runner.summaryNow (leaderTick, 0);
        auto ours = follower.runner.summaryNow (localTick, 0);
        return pair::differenceOf (theirs, ours, labels);
    }
}

TEST_CASE ("seat: an engine that was elsewhere is seated where the leader is - its own runs ended with no footer, the leader's made under their names at their playheads, the standby written, the names drawn alike, one record a replay seats alike")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const Show show;
    Machine leader { show.text }, follower { show.text };

    //  Each machine's own randomness: a salt of its own.
    follower.runIds.drawSeed();
    follower.runner.runNames().setSalt (follower.runner.runNames().salt() ^ 0x5a5a5a5au);
    REQUIRE (follower.runner.runNames().salt() != leader.runner.runNames().salt());

    std::int64_t tick = 50;
    playEvening (leader, show, tick);

    for (; tick < 200; ++tick)
        leader.tickOnce (tick);

    /*  THE FOLLOWER WAS ELSEWHERE: a scene of its own running, its member
        holding and its footer still to come, the standby on another cue. */
    std::int64_t local = 3000;
    follower.go (local, show.list, show.other);
    local += 2;

    for (int n = 0; n < 10; ++n)
        follower.tickOnce (local++);

    const auto ownScene = follower.runOf (show.other);
    REQUIRE (! ownScene.empty());
    REQUIRE (follower.engine.submit (std::string (origin::cli), "node.set",
                                     { s (cue::standbyAddressOf (show.list)), s (show.after) }));
    follower.tickOnce (local++);

    /*  THE LEADER'S POSITION, as its last tick left it, read on the follower's
        clock and applied in the follower's next tick - the slot, so on time. */
    const auto leaderTick = tick - 1;
    auto at = leader.runner.positionNow (leaderTick, 7);
    at.rebase (local - leaderTick);

    REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (pair::toText (at)) }));
    follower.tickOnce (local, false);

    const auto seats = recordsOf (follower.engine, "pair.seat");
    REQUIRE (seats.size() == 1u);
    CHECK (seats.front().kind == LogRecord::Kind::applied);
    CHECK (follower.runner.seatNotes().empty());

    //  ITS OWN SCENE ENDED, no footer, and gone from the table; the standby the leader's.
    CHECK (follower.runs.find (ownScene) == nullptr);
    CHECK (follower.standbyOf (show.list) == leader.standbyOf (show.list));

    //  THE LEADER'S RUNS, UNDER THE LEADER'S NAMES, the scene in its round.
    {
        const auto theirs = liveLines (leader.runs);
        const auto ours = liveLines (follower.runs);
        INFO ("leader:\n" << spelled (theirs) << "follower:\n" << spelled (ours));
        REQUIRE (theirs.size() == ours.size());

        for (const auto& [id, line] : theirs)
        {
            REQUIRE (ours.count (id) == 1u);
            CHECK (ours.at (id).cue == line.cue);
            CHECK (ours.at (id).parent == line.parent);
        }

        const auto* scene = follower.runs.find (leader.runOf (show.scene));
        REQUIRE (scene != nullptr);
        CHECK (scene->iteration == leader.runs.find (scene->id)->iteration);
        CHECK (scene->round == leader.runs.find (scene->id)->round);
    }

    //  TONIGHT'S, AND THE NAMES.
    CHECK (follower.dcas.trimOf (show.dca) == doctest::Approx (-4.5));
    CHECK (follower.lanes.cue() == show.bed);
    CHECK (follower.lanes.isArmed (cue::levelLaneKey));
    CHECK (follower.runner.runNames().salt() == leader.runner.runNames().salt());
    CHECK (follower.runner.runNames().next ("root:" + show.next, show.next)
           == leader.runner.runNames().next ("root:" + show.next, show.next));

    /*  AND FROM THEN ON, TICK FOR TICK: the leader's tick and the follower's a
        seat apart - the scene's memos coming due, the walk stepping on and
        naming its members, the bed looping - and every summary the same, once
        the seated sounds have started. */
    std::string firstDifference;
    std::int64_t compared = 0;

    for (int n = 1; n <= 260; ++n)
    {
        leader.tickOnce (leaderTick + n);
        follower.tickOnce (local + n);

        if (n <= follower.runner.seatSettleTicks())
            continue;

        ++compared;

        if (! firstDifference.empty())
            continue;

        if (const auto difference = differenceAt (leader, leaderTick + n, follower, local + n); ! difference.empty())
            firstDifference = "after " + std::to_string (n) + " ticks: " + difference;
        else if (! (liveLines (leader.runs) == liveLines (follower.runs)))
            firstDifference = "after " + std::to_string (n) + " ticks, the run tables:\n" + spelled (liveLines (leader.runs))
                            + "against\n" + spelled (liveLines (follower.runs));
    }

    CHECK (firstDifference == "");
    CHECK (compared > 240);

    //  NO FOOTER EVER RAN for the scene the seat ended; the walk named the same members on both.
    CHECK (std::none_of (follower.runs.all().begin(), follower.runs.all().end(),
                         [&show] (const cue::Run& run) { return run.cue == show.release; }));
    CHECK (preparationsOf (follower.runs) == preparationsOf (leader.runs));

    //  ONE RECORD, AND A REPLAY OF THE FOLLOWER'S LOG IS SEATED ALIKE.
    Machine replayed { show.text, false };
    const auto result = wfg::replay (replayed.engine, LogFile::parse (follower.engine.log().contents()));

    std::string mismatches;
    for (const auto& mismatch : result.mismatches)
        mismatches += mismatch + "\n";

    INFO (mismatches);
    CHECK (result.ok);
    CHECK (spelled (liveLines (replayed.runs)) == spelled (liveLines (follower.runs)));
    CHECK (replayed.standbyOf (show.list) == follower.standbyOf (show.list));
    CHECK (replayed.runner.runNames().position() == follower.runner.runNames().position());
}

//==============================================================================
namespace
{
    /*  THE WIRE AS A TCP CONNECTION IS: in order, each frame arriving no sooner
        than the one before it, a random 0 to 15 ms after it left. */
    struct JitteryWire
    {
        struct Frame
        {
            Clock::time_point at;
            osc::Packet packet;
        };

        explicit JitteryWire (unsigned seed) : random (seed) {}

        pair::Stream::Send send()
        {
            return [this] (const std::string&, const std::vector<std::uint8_t>& bytes)
            {
                const auto decoded = osc::decode (bytes.data(), bytes.size());

                if (! decoded.ok)
                    return false;

                auto at = sentAt + std::chrono::microseconds (jitter (random));
                at = std::max (at, lastArrival);
                lastArrival = at;
                frames.push_back ({ at, decoded.packet });
                return true;
            };
        }

        void deliverUpTo (Clock::time_point now, pair::Receiver& receiver)
        {
            while (! frames.empty() && frames.front().at <= now)
            {
                receiver.deliver ("127.0.0.1:5010", frames.front().packet, frames.front().at);
                frames.pop_front();
            }
        }

        std::mt19937 random;
        std::uniform_int_distribution<int> jitter { 0, 15000 };
        Clock::time_point sentAt {}, lastArrival {};
        std::deque<Frame> frames;
    };

    osc::Packet packetOf (const std::vector<std::uint8_t>& bytes)
    {
        const auto decoded = osc::decode (bytes.data(), bytes.size());
        REQUIRE (decoded.ok);
        return decoded.packet;
    }
}

TEST_CASE ("seat: two engines on one show - a follower that joins late is seated, its runs the leader's for two hundred ticks and in step; a run stopped on it by hand puts it out of step until a realign")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const Show show;
    Machine main { show.text }, backup { show.text };
    backup.runIds.drawSeed();
    backup.runner.runNames().setSalt (backup.runner.runNames().salt() ^ 0x0f0f0f0fu);

    //  THE LEADER'S HALF, as serve wires it: the stream, its position and its summaries.
    pair::Stream stream { "S1" };
    stream.setLeading (true);
    main.engine.setAfterRecord ([&stream] (const LogRecord& record, const Command* command) { stream.keep (record, command); });
    stream.setPositionMaker ([&main] (std::int64_t at, std::uint64_t counter)
                             { return pair::toText (main.runner.positionNow (at, counter)); });
    stream.setSummaryMaker ([&main] (std::int64_t at, std::uint64_t counter)
                            { return pair::summaryBytes (main.runner.summaryNow (at, counter)); });

    unsigned seed = 11;
    SUBCASE ("one evening's jitter")    { seed = 11; }
    SUBCASE ("another evening's")       { seed = 2027; }

    JitteryWire wire { seed };
    stream.setSend (wire.send());

    //  THE FOLLOWER'S, as serve wires it.
    pair::Receiver receiver;
    pair::Follower following { [&backup] (Event event) { return backup.engine.submit (std::move (event)); } };
    pair::InStep inStep;
    pair::Labels labels;
    labels.cue = [&backup] (const std::string& id) { return backup.runner.labelOf (id); };
    labels.list = labels.cue;

    receiver.setOnRecord ([&following] (std::uint64_t, const LogRecord& record) { following.record (record); });
    receiver.setOnBeat ([&following] (std::int64_t mainTick, Clock::time_point at) { following.beat (mainTick, at); });
    receiver.setOnPosition ([&following] (std::uint64_t, std::int64_t mainTick, std::string text)
                            { following.position (mainTick, std::move (text)); });
    receiver.setOnSummary ([&inStep] (pair::Summary summary) { inStep.heard (std::move (summary)); });

    pair::FollowGate gate { backup.engine.commands(), [] (std::vector<std::uint8_t>) { return true; } };
    backup.engine.setGate ([&gate] (Event& event) { return gate.take (event); });
    backup.engine.setAfterRecord ([&following, &inStep, &backup] (const LogRecord& record, const Command*)
                                  {
                                      following.applied (record);

                                      if (record.command == "pair.seat" && record.kind == LogRecord::Kind::applied)
                                          inStep.seated (record.tick + backup.runner.seatSettleTicks());
                                  });

    /*  THE TWO CLOCKS, 7 ms out of phase: the backup's counter far ahead of
        the main's, or behind it - a machine started after its main, whose
        clock reads the main's first sounds as ticks before its own first. */
    std::int64_t clockBase = 2000;
    SUBCASE ("the backup's clock ahead of the main's")  { clockBase = 2000; }
    SUBCASE ("the backup's clock behind the main's")    { clockBase = -150; }

    const auto mainDue = [] (std::int64_t at) { return t0 + 20ms * at; };
    const auto backupDue = [clockBase] (std::int64_t local) { return t0 + 20ms * (local - clockBase) + 7ms; };

    //  THE MAIN'S EVENING, BEFORE THE BACKUP IS THERE.
    std::int64_t mainTick = 50;
    playEvening (main, show, mainTick);

    for (; mainTick < 200; ++mainTick)
    {
        main.tickOnce (mainTick);
        wire.sentAt = mainDue (mainTick) + 2ms;
        stream.afterTick (mainTick, 1.0);
    }

    //  THE BACKUP JOINS: it says hello from now, and follows.
    pair::wire::Hello hello;
    hello.machine = "backup";
    hello.role = "backup";
    hello.lastCounter = pair::wire::fromNow;
    hello.stream = "S1";
    hello.state = "following";
    hello.ownStream = "S2";
    hello.described = true;
    stream.received ("ws:127.0.0.1:50001", packetOf (pair::wire::helloBytes (hello)), mainDue (mainTick));
    receiver.startAfresh ("S1");
    following.start();
    backup.engine.setGated (true);

    /*  TICK BY TICK, each machine on its own clock. On the backup's, after the
        seat has settled, the summaries say whether it is in step, and its run
        table is laid against the main's of the same moment - the main's tick
        the follower's slot names. */
    std::map<std::int64_t, std::map<std::string, RunLine>> mainRuns;
    std::map<std::int64_t, pair::Summary> mainSummaries;
    std::int64_t localTick = clockBase + mainTick + 3;
    std::int64_t seatedAt = -1, compared = 0, inStepTicks = 0;
    std::string firstDifference;

    std::string plantedRun, outOfStep;
    std::int64_t plantedAt = -1, realignedAt = -1, backInStepAt = -1;

    const auto lastMainTick = mainTick + 520;

    while (mainTick <= lastMainTick)
    {
        if (mainDue (mainTick) <= backupDue (localTick))
        {
            main.tickOnce (mainTick);
            wire.sentAt = mainDue (mainTick) + 2ms;
            stream.afterTick (mainTick, 1.0);
            mainRuns[mainTick] = liveLines (main.runs);
            mainSummaries[mainTick] = main.runner.summaryNow (mainTick, 0);
            ++mainTick;
            continue;
        }

        const auto now = backupDue (localTick);
        wire.deliverUpTo (now, receiver);
        receiver.drain (now, 1.0);
        const auto seatsBefore = following.seats();
        following.beforeTick (localTick, now, 0.0004);
        backup.tickOnce (localTick, following.seats() == seatsBefore);

        inStep.afterTick (localTick, following.offset(), following.delayFixed() && ! following.seatHeld(),
                          [&backup, localTick] { return backup.runner.summaryNow (localTick, 0); }, labels);

        if (seatedAt < 0 && following.seats() > 0)
            seatedAt = localTick;

        if (inStep.inStep())
            ++inStepTicks;

        //  THE SEAT SETTLED: two hundred ticks of the main's runs, a delay later.
        const auto settle = backup.runner.seatSettleTicks();

        if (seatedAt >= 0 && plantedAt < 0 && localTick > seatedAt + settle && localTick <= seatedAt + settle + 200)
        {
            if (const auto theirs = mainRuns.find (localTick - following.offset()); theirs != mainRuns.end())
            {
                ++compared;

                if (firstDifference.empty() && ! (liveLines (backup.runs) == theirs->second))
                    firstDifference = "local tick " + std::to_string (localTick) + ", the main's "
                                    + std::to_string (theirs->first) + ":\n" + spelled (theirs->second)
                                    + "against\n" + spelled (liveLines (backup.runs));

                //  AND THE PLAYHEADS, every tick and not every fifth: within two ticks of the main's.
                if (firstDifference.empty())
                    if (const auto difference = pair::differenceOf (mainSummaries.at (theirs->first),
                                                                    backup.runner.summaryNow (localTick, 0), labels);
                        ! difference.empty())
                        firstDifference = "local tick " + std::to_string (localTick) + ": " + difference;
            }
        }

        /*  A DIFFERENCE PLANTED BY HAND: the bed killed on the backup alone, as
            its engine might have decided - the leader's plays on. */
        if (seatedAt >= 0 && plantedAt < 0 && localTick == seatedAt + settle + 210)
        {
            INFO ("differs: " << inStep.differs() << "; run tables: " << firstDifference
                  << "; compared " << inStep.compared() << ", mismatched " << inStep.mismatched());
            REQUIRE (inStep.inStep());
            plantedRun = backup.runOf (show.bed);
            REQUIRE (! plantedRun.empty());
            REQUIRE (backup.engine.submit (std::string (origin::engine), "run.kill", { s (plantedRun) }));
            plantedAt = localTick;
        }

        //  OUT OF STEP, SAID - then a realign asked of the leader, as `pair.realign` asks.
        if (plantedAt >= 0 && realignedAt < 0 && ! inStep.inStep() && ! inStep.differs().empty())
        {
            outOfStep = inStep.differs();
            stream.received ("ws:127.0.0.1:50001", osc::Packet::message (pair::wire::seat, {}), now);
            realignedAt = localTick;
        }

        if (realignedAt >= 0 && backInStepAt < 0 && following.seats() > 1 && inStep.inStep())
            backInStepAt = localTick;

        ++localTick;
    }

    MESSAGE ("offset " << following.offset() << ", delay " << following.delay() << ", seated at local tick "
             << seatedAt << ", seats " << following.seats() << ", run tables compared " << compared
             << ", summaries compared " << inStep.compared() << ", mismatched " << inStep.mismatched()
             << ", out of step: \"" << outOfStep << "\", in step again " << (backInStepAt - realignedAt)
             << " ticks after the realign was asked");

    //  SEATED ONCE ON JOINING, AND THE MAIN'S POSITION SENT FOR IT.
    REQUIRE (seatedAt >= 0);
    CHECK (stream.positionsSent() >= 1);
    CHECK (following.seatsDropped() == 0);

    //  ITS RUNS THE MAIN'S, UNDER THE MAIN'S NAMES, FOR TWO HUNDRED TICKS.
    CHECK (firstDifference == "");
    CHECK (compared >= 190);
    CHECK (inStepTicks > 150);

    //  A RUN STOPPED HERE ALONE: OUT OF STEP, IN WORDS - AND A REALIGN SEATS IT AGAIN.
    REQUIRE (plantedAt >= 0);
    CHECK (outOfStep == "cue Bed is running on the main, not here");
    REQUIRE (realignedAt >= 0);
    CHECK (stream.positionsSent() >= 2);
    CHECK (following.seats() >= 2);
    CHECK (backInStepAt >= 0);
    CHECK (backup.runOf (show.bed) == main.runOf (show.bed));
    CHECK (inStep.inStep());
    CHECK (inStep.differs() == "");

    //  THE SEATS ARE THE ENGINE'S RECORDS ON THE FOLLOWER: two, applied.
    const auto seats = recordsOf (backup.engine, "pair.seat");
    CHECK (seats.size() == static_cast<std::size_t> (following.seats()));
    CHECK (std::all_of (seats.begin(), seats.end(), [] (const LogRecord& record)
                        { return record.kind == LogRecord::Kind::applied && record.origin == origin::engine; }));
}

//==============================================================================
TEST_CASE ("in step: a summary's wire, and the one sentence for what differs")
{
    pair::Summary leader;
    leader.tick = 1234;
    leader.counter = 56;
    leader.standby = { { "7K2QM9X4", "B3N8R5TW" } };
    leader.runs = { { "R8M2K4Q1", "C1D2E3F4", "playing", 400, -1, 0 },
                    { "R8M2K4Q2", "G5H6J7K8", "playing", 90, 1, 0 },
                    { "R8M2K4Q3", "M9N8P7Q6", "waiting", -25, -1, 0 } };

    //  THE WIRE: what went is what comes.
    const auto bytes = pair::summaryBytes (leader);
    const auto decoded = osc::decode (bytes.data(), bytes.size());
    REQUIRE (decoded.ok);
    CHECK (pair::wire::boundFor (decoded.packet) == pair::wire::Bound::follower);

    const auto read = pair::readSummary (decoded.packet);
    REQUIRE (read.has_value());
    CHECK (read->tick == 1234);
    CHECK (read->counter == 56u);
    REQUIRE (read->runs.size() == 3u);
    CHECK (read->runs[1].range == 1);
    CHECK (read->runs[2].playhead == -25);
    CHECK (read->runs[0].loop == 0);
    CHECK_FALSE (pair::readSummary (osc::Packet::message (pair::wire::summary, { osc::Value::int64 (1) })).has_value());

    pair::Labels labels;
    labels.cue = [] (const std::string& id) { return id == "C1D2E3F4" ? std::string ("12 Thunder") : std::string {}; };
    labels.list = [] (const std::string& id) { return id == "7K2QM9X4" ? std::string ("Main") : std::string {}; };

    //  THE SAME: nothing to say - two ticks apart is in step.
    auto here = leader;
    here.runs[0].playhead = 402;
    CHECK (pair::differenceOf (leader, here, labels) == "");

    here.runs[0].playhead = 403;
    CHECK (pair::differenceOf (leader, here, labels) == "cue 12 Thunder's playhead is 60 ms apart");

    here = leader;
    here.standby[0].cue = "N6VRA3WB";
    CHECK (pair::differenceOf (leader, here, labels) == "standby differs on list Main");

    here = leader;
    here.runs.erase (here.runs.begin());
    CHECK (pair::differenceOf (leader, here, labels) == "cue 12 Thunder is running on the main, not here");

    here = leader;
    here.runs[0].state = "stopping";
    CHECK (pair::differenceOf (leader, here, labels) == "cue 12 Thunder is playing on the main and stopping here");

    here = leader;
    here.runs[1].range = 0;
    CHECK (pair::differenceOf (leader, here, labels) == "cue G5H6J7K8 plays range 2 on the main and range 1 here");

    /*  ROUND THE LOOP: a slice a follower counts again from the pass it is in
        is the same point of it - the same sound. */
    auto looping = leader;
    looping.runs[1].loop = 25;
    here = looping;
    here.runs[1].playhead = 90 - 3 * 25 + 1;
    CHECK (pair::differenceOf (looping, here, labels) == "");
    here.runs[1].playhead = 90 - 3 * 25 + 10;
    CHECK (pair::differenceOf (looping, here, labels) == "cue G5H6J7K8's playhead is 200 ms apart");

    here = leader;
    here.runs.push_back ({ "R8M2K4Q9", "C1D2E3F4", "playing", 3, -1, 0 });
    labels.leader = "the backup";
    CHECK (pair::differenceOf (leader, here, labels) == "cue 12 Thunder is running here, not on the backup");
}

TEST_CASE ("in step: a summary at its slot, a difference said the second time, a match at once, a record applied otherwise at once")
{
    pair::Labels labels;
    pair::InStep inStep;

    pair::Summary same;
    same.runs = { { "R8M2K4Q1", "C1D2E3F4", "playing", 100, -1, 0 } };

    const pair::Summary missing;

    std::int64_t localTick = 1000;
    const std::int64_t offset = 2;
    auto own = same;

    const auto leaderAt = [] (pair::Summary summary, std::int64_t tick)
    {
        summary.tick = tick;
        return summary;
    };

    const auto runTo = [&] (std::int64_t until)
    {
        for (; localTick <= until; ++localTick)
            inStep.afterTick (localTick, offset, true, [&own] { return own; }, labels);
    };

    CHECK_FALSE (inStep.inStep());

    //  Held to its slot: the leader's tick 1000 is compared at local 1002, not before.
    inStep.heard (leaderAt (same, 1000));
    runTo (1001);
    CHECK (inStep.compared() == 0);
    runTo (1002);
    CHECK (inStep.compared() == 1);
    CHECK (inStep.inStep());

    //  A difference once is a launch a tick apart; twice, it is said.
    own = missing;
    inStep.heard (leaderAt (same, 1005));
    runTo (1007);
    CHECK (inStep.inStep());
    CHECK (inStep.differs() == "");

    inStep.heard (leaderAt (same, 1010));
    runTo (1012);
    CHECK_FALSE (inStep.inStep());
    CHECK (inStep.differs() == "cue C1D2E3F4 is running on the main, not here");

    //  A match says in step at once, and clears the sentence.
    own = same;
    inStep.heard (leaderAt (same, 1015));
    runTo (1017);
    CHECK (inStep.inStep());
    CHECK (inStep.differs() == "");

    //  A record applied otherwise: said at once, cleared by the next match.
    inStep.recordDiffered ("1 record applied otherwise than on the main; the first, go at the main's tick 1016:"
                           " refused there (too-soon), applied here");
    CHECK_FALSE (inStep.inStep());
    CHECK (inStep.differs().rfind ("1 record applied otherwise", 0) == 0);
    inStep.heard (leaderAt (same, 1020));
    runTo (1022);
    CHECK (inStep.inStep());

    //  A summary whose slot has passed is dropped, unread; so is one in a seat's settling.
    inStep.heard (leaderAt (same, 1000));
    runTo (1023);
    CHECK (inStep.waiting() == 0u);

    const auto before = inStep.compared();
    inStep.seated (1040);
    own = missing;
    inStep.heard (leaderAt (same, 1030));
    inStep.heard (leaderAt (same, 1035));
    runTo (1038);
    CHECK (inStep.compared() == before);
    CHECK (inStep.inStep());
}

//==============================================================================
TEST_CASE ("seat: a position of two hundred runs goes in parts of whole lines, and comes back whole, once, whichever path brings it")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    pair::Position at;
    at.counter = 9001;
    at.tick = 77777;
    at.names.salt = 0xfedcba9876543210ull;

    for (int n = 0; n < 200; ++n)
    {
        pair::SeatedRun run;
        run.id = "R" + std::to_string (1000000 + n);
        run.cue = "C" + std::to_string (2000000 + n);
        run.kind = n % 3 == 0 ? "group" : "media";
        run.state = "playing";
        run.launchTick = 70000 + n;
        run.launched = true;
        run.position = 12.345678901234 + n;
        run.ownLevel = -6.5;
        run.round = { "A0000001", "A0000002", "A0000003" };
        run.moved["send/B0000001"] = -3.25;
        at.runs.push_back (std::move (run));
        at.names.counts[{ "root:C" + std::to_string (2000000 + n), "C" + std::to_string (2000000 + n) }] = 3;
    }

    const auto text = pair::toText (at);
    REQUIRE (text.size() > 2 * pair::wire::positionPartBytes);

    const auto parts = pair::wire::positionMessages (at.counter, at.tick, text);
    CHECK (parts.size() >= 3u);

    pair::Receiver receiver;
    std::vector<std::string> handed;
    receiver.setOnPosition ([&handed] (std::uint64_t counter, std::int64_t tick, std::string whole)
                            {
                                CHECK (counter == 9001u);
                                CHECK (tick == 77777);
                                handed.push_back (std::move (whole));
                            });

    //  ONE BUNDLE, AS A TICK SENDS IT; then the same again down a second path.
    std::vector<osc::Packet> elements;

    for (const auto& bytes : parts)
    {
        const auto decoded = osc::decode (bytes.data(), bytes.size());
        REQUIRE (decoded.ok);
        CHECK (pair::wire::boundFor (decoded.packet) == pair::wire::Bound::follower);
        elements.push_back (decoded.packet);
    }

    const auto bundle = osc::Packet::bundle (osc::TimeTag {}, elements);
    receiver.deliver ("10.0.0.1:5010", bundle, t0);
    receiver.deliver ("10.0.0.2:5010", bundle, t0 + 1ms);
    receiver.drain (t0 + 2ms, 1.0);

    REQUIRE (handed.size() == 1u);
    CHECK (handed.front() == text);

    const auto read = pair::readPosition (handed.front());
    REQUIRE (read.has_value());
    CHECK (read->runs.size() == 200u);
    CHECK (read->names == at.names);
    CHECK (read->runs[199].position == doctest::Approx (12.345678901234 + 199).epsilon (1e-15));

    //  A PART OUT OF ITS PLACE STARTS NOTHING: a position missing its middle is never handed on.
    pair::Receiver broken;
    auto brokenHanded = 0;
    broken.setOnPosition ([&brokenHanded] (std::uint64_t, std::int64_t, std::string) { ++brokenHanded; });

    for (std::size_t n = 0; n < elements.size(); ++n)
        if (n != 1)
            broken.deliver ("10.0.0.1:5010", elements[n], t0);

    broken.drain (t0 + 1ms, 1.0);
    CHECK (brokenHanded == 0);
}

//==============================================================================
TEST_CASE ("seat: the leader's position asked for - by a follow from now, by a realign - sent once, after the records it is true at")
{
    pair::Stream stream { "S1" };
    stream.setLeading (true);

    std::int64_t made = 0;
    stream.setPositionMaker ([&made] (std::int64_t tick, std::uint64_t counter)
                             {
                                 ++made;
                                 return "position i:" + std::to_string (pair::Position::format) + " h:" + std::to_string (counter) + " h:" + std::to_string (tick) + "\nend\n";
                             });

    std::vector<osc::Packet> frames;
    stream.setSend ([&frames] (const std::string&, const std::vector<std::uint8_t>& bytes)
                    {
                        const auto decoded = osc::decode (bytes.data(), bytes.size());
                        REQUIRE (decoded.ok);
                        frames.push_back (decoded.packet);
                        return true;
                    });

    const auto addresses = [] (const osc::Packet& frame)
    {
        std::vector<std::string> out;

        for (const auto& element : frame.elements)
            out.push_back (element.address);

        return out;
    };

    //  Three records kept before anybody follows.
    for (int n = 0; n < 3; ++n)
    {
        LogRecord record;
        record.kind = LogRecord::Kind::applied;
        record.origin = "cli";
        record.command = "go";
        stream.keep (record, nullptr);
    }

    //  A FOLLOW FROM NOW: welcomed after the last counter, and told where this machine is.
    pair::wire::Hello hello;
    hello.machine = "backup";
    hello.role = "backup";
    hello.lastCounter = pair::wire::fromNow;
    hello.stream = "S1";
    hello.state = "following";
    hello.described = true;
    const auto helloBytes = pair::wire::helloBytes (hello);
    const auto helloPacket = osc::decode (helloBytes.data(), helloBytes.size());
    REQUIRE (helloPacket.ok);
    stream.received ("ws:a", helloPacket.packet, t0);
    stream.afterTick (100, 1.0);

    REQUIRE (frames.size() == 1u);
    CHECK (addresses (frames[0]) == std::vector<std::string> { pair::wire::welcome, pair::wire::position, pair::wire::beat });
    CHECK (frames[0].elements[1].args[0].getInt64() == 3);
    CHECK (made == 1);

    //  Once: the next tick sends only the beat.
    stream.afterTick (101, 1.0);
    CHECK (addresses (frames[1]) == std::vector<std::string> { pair::wire::beat });

    //  A REALIGN, after a record: the record first, then the position true after it.
    LogRecord record;
    record.kind = LogRecord::Kind::applied;
    record.origin = "cli";
    record.command = "go";
    stream.keep (record, nullptr);
    stream.received ("ws:a", osc::Packet::message (pair::wire::seat, {}), t0 + 50ms);
    stream.afterTick (102, 1.0);
    CHECK (addresses (frames[2]) == std::vector<std::string> { pair::wire::record, pair::wire::position, pair::wire::beat });
    CHECK (frames[2].elements[1].args[0].getInt64() == 4);
    CHECK (stream.positionsSent() == 2);

    //  A RESUME THE RING ANSWERS IS WHERE IT WAS: no position.
    hello.lastCounter = 4;
    const auto resumeBytes = pair::wire::helloBytes (hello);
    const auto resume = osc::decode (resumeBytes.data(), resumeBytes.size());
    stream.received ("ws:b", resume.packet, t0 + 60ms);
    stream.afterTick (103, 1.0);
    CHECK (stream.positionsSent() == 2);

    //  A connection that does not follow asking is nobody's to seat.
    stream.received ("ws:c", osc::Packet::message (pair::wire::seat, {}), t0 + 70ms);
    stream.afterTick (104, 1.0);
    CHECK (stream.positionsSent() == 2);
}

//==============================================================================
TEST_CASE ("seat: what a position and a summary cost - fifty sounds running")
{
    /*  WHAT RD.8 ADDS TO A TICK. On the leader: a summary every fifth tick,
        and a position when a follower starts following or asks again. On the
        follower: the summary compared every fifth tick, and a seat when its
        position's slot comes - the text read, moved onto this machine's clock,
        and every run built. Fifty sounds playing, with a scene: the busy case.
        Loose checks, for a runner busy with other suites; the figures are the
        measurement. */
    std::string text, list;
    std::vector<std::string> sounds;

    {
        doc::ShowDocument show;
        list = show.createList ("Sound").id;

        for (int n = 0; n < 50; ++n)
        {
            const auto id = show.createCue (list, n, "media", "Sound " + std::to_string (n + 1)).id;
            REQUIRE (show.setAttribute ("/godot/cue/" + id + "/file", "sound" + std::to_string (n + 1) + ".wav").ok);
            sounds.push_back (id);
        }

        REQUIRE (show.setAttribute ("/godot/list/goDebounce", "0").ok);
        text = doc::CanonicalXml::write (show);
    }

    Machine leader { text };
    std::int64_t tick = 10;

    for (const auto& sound : sounds)
        REQUIRE (leader.engine.submit (std::string (origin::cli), "cue.fire", { s (sound) }));

    for (; tick < 40; ++tick)
        leader.tickOnce (tick);

    REQUIRE (leader.runner.summaryNow (tick, 0).runs.size() == 50u);

    std::vector<double> summaryMade, summaryCompared, positionMade, rebased, seatCold, seatWarm;
    std::size_t positionBytes = 0;

    pair::Labels labels;
    labels.cue = [&leader] (const std::string& id) { return leader.runner.labelOf (id); };

    for (int round = 0; round < 30; ++round)
    {
        const auto a = std::chrono::steady_clock::now();
        const auto bytes = pair::summaryBytes (leader.runner.summaryNow (tick, 7));
        const auto b = std::chrono::steady_clock::now();
        const auto decoded = osc::decode (bytes.data(), bytes.size());
        const auto theirs = pair::readSummary (decoded.packet);
        const auto difference = pair::differenceOf (*theirs, leader.runner.summaryNow (tick, 7), labels);
        const auto c = std::chrono::steady_clock::now();
        CHECK (difference == "");

        const auto position = pair::toText (leader.runner.positionNow (tick, 7));
        const auto d = std::chrono::steady_clock::now();
        const auto moved = pair::rebasedText (position, 1000);
        const auto e = std::chrono::steady_clock::now();
        REQUIRE (moved.has_value());
        positionBytes = position.size();

        summaryMade.push_back (std::chrono::duration<double, std::micro> (b - a).count());
        summaryCompared.push_back (std::chrono::duration<double, std::micro> (c - b).count());
        positionMade.push_back (std::chrono::duration<double, std::micro> (d - c).count());
        rebased.push_back (std::chrono::duration<double, std::micro> (e - d).count());
    }

    //  THE SEAT ITSELF: on a machine with nothing, every run built; then again, every run kept.
    for (int round = 0; round < 5; ++round)
    {
        Machine follower { text };
        auto at = leader.runner.positionNow (tick, 7);
        at.rebase (5000 - tick);
        const auto seatText = pair::toText (at);

        for (int again = 0; again < 2; ++again)
        {
            const auto local = 5000 + again;
            REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (seatText) }));
            follower.audio.samples = local * 960;
            const auto a = std::chrono::steady_clock::now();
            follower.engine.processTick (local);
            const auto b = std::chrono::steady_clock::now();
            follower.audio.completeArms (follower.engine);
            (again == 0 ? seatCold : seatWarm).push_back (std::chrono::duration<double, std::micro> (b - a).count());
        }

        CHECK (liveLines (follower.runs).size() == 50u);
    }

    for (auto* figures : { &summaryMade, &summaryCompared, &positionMade, &rebased, &seatCold, &seatWarm })
        std::sort (figures->begin(), figures->end());

    const auto median = [] (const std::vector<double>& figures) { return figures[figures.size() / 2]; };

    MESSAGE ("fifty sounds running: a summary made " << median (summaryMade) << " us (" << pair::summaryBytes (leader.runner.summaryNow (tick, 7)).size()
             << " bytes), read and compared " << median (summaryCompared) << " us; a position made "
             << median (positionMade) << " us (" << positionBytes << " bytes), read and moved onto another clock "
             << median (rebased) << " us; a seat applied, every run built " << median (seatCold)
             << " us, every run kept " << median (seatWarm) << " us");

    CHECK (median (summaryMade) < 5000.0);
    CHECK (median (summaryCompared) < 5000.0);
    CHECK (median (positionMade) < 50000.0);
    CHECK (median (seatCold) < 200000.0);
}

//==============================================================================
TEST_CASE ("seat: a follower seated while the leader's timecode runs starts its own code at the frame the leader's will be at, on the seat's launch sample")
{
    /*  THROUGH `seatPlan`, as a sound is seated (namespace draft §59, TC.7): the
        leader's run under the leader's name, its code started where the leader's
        will have got to when this one starts - a seat's lateness, the launch
        latency and a cold arm's lead on - and from then on the two read alike. */
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    doc::ShowDocument made;
    const auto list = made.createList ("Sound").id;
    REQUIRE (made.createBus ("direct", 2).ok);
    const auto line = made.createBus ("direct", 1);
    REQUIRE (line.ok);
    REQUIRE (made.setAttribute ("/godot/sync/ltcOut", line.id).ok);

    const auto code = made.createCue (list, 0, "timecode", {}, {},
                                      doc::ShowDocument::Attributes { { "format", "ltc" }, { "from", "01:00:00:00" } });
    REQUIRE (code.ok);
    const auto after = made.createCue (list, 1, "memo", "After").id;
    REQUIRE (! after.empty());
    REQUIRE (made.setAttribute ("/godot/list/goDebounce", "0").ok);

    const auto text = doc::CanonicalXml::write (made);
    Machine leader { text }, follower { text };

    follower.runIds.drawSeed();
    follower.runner.runNames().setSalt (follower.runner.runNames().salt() ^ 0x5a5a5a5au);

    std::int64_t tick = 50;
    leader.go (tick, list, code.id);
    tick += 2;

    for (; tick < 200; ++tick)
        leader.tickOnce (tick);

    REQUIRE (leader.audio.codes.size() == 1u);
    const auto theirs = leader.audio.codes.front();
    const auto runId = leader.runOf (code.id);
    REQUIRE (! runId.empty());

    const std::int64_t local = 3000;
    const auto leaderTick = tick - 1;
    auto at = leader.runner.positionNow (leaderTick, 7);
    at.rebase (local - leaderTick);

    REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (pair::toText (at)) }));
    follower.tickOnce (local, false);

    //  The leader's run, under its name, and nothing said: a code is the seat's to make.
    CHECK (follower.runner.seatNotes().empty());
    CHECK (follower.runOf (code.id) == runId);

    //  Placed by the next tick, on the sample a sound seated now would launch
    //  on - the leader's tick and the follower's a seat apart from here.
    leader.tickOnce (leaderTick + 1);
    follower.tickOnce (local + 1);
    REQUIRE (follower.audio.codes.size() == 1u);

    const auto ours = follower.audio.codes.front();
    const auto launchIn = static_cast<std::int64_t> (follower.runner.latencyTicks() + cue::Runner::seatLeadTicks);
    CHECK (ours.zero == (local + launchIn) * 960);

    //  Where the leader's code is at that moment - its tick `leaderTick + launchIn` - by hand.
    const auto leaderSince = (leaderTick + launchIn) * 960 - theirs.zero;
    CHECK (ours.frame == theirs.frame + leaderSince / 1920);

    //  And from then on, tick for tick, the same runs at the same playheads.
    std::string firstDifference;

    for (std::int64_t n = 2; n <= 80; ++n)
    {
        leader.tickOnce (leaderTick + n);
        follower.tickOnce (local + n);

        if (n <= follower.runner.seatSettleTicks() || ! firstDifference.empty())
            continue;

        if (const auto difference = differenceAt (leader, leaderTick + n, follower, local + n); ! difference.empty())
            firstDifference = "after " + std::to_string (n) + " ticks: " + difference;
    }

    CHECK (firstDifference == "");

    const auto* run = follower.runs.find (runId);
    REQUIRE (run != nullptr);
    CHECK (run->state == cue::runState::playing);
    CHECK (run->timecode == leader.runs.find (runId)->timecode);
}

//==============================================================================
/*  AN INCOMING CODE'S MAP IN THE POSITION (namespace draft §60, TG.5; AIP): a
    top-level line of its own, `code`, beside the trims - a new word, never an
    atom on an old line - read back exactly in any locale, its anchor's sample
    moved with the ticks by `rebase`, a position of the format before TG.5
    refused in a sentence; and a seat hands the leader's maps over in place of
    the follower's own. */
TEST_CASE ("seat: each incoming code's map rides the position as a code line - read back exactly, its sample moved with the ticks, another format refused in a sentence, and a seat hands it over")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const Show show;
    Machine leader { show.text }, follower { show.text };

    const auto heard = [] (const std::string& source, const char* what, const char* rate, std::int64_t frame,
                           std::int64_t sample, double speed)
    {
        return std::vector<osc::Value> { s (source), s (what), s (rate), osc::Value::int64 (frame),
                                         osc::Value::int64 (sample), osc::Value::float64 (speed) };
    };

    //  THE LEADER HEARD TWO CODES: LTC at 29.97 drop-frame a little fast, MTC standing.
    REQUIRE (leader.engine.submit ("ltc:IN000001", "code.heard",
                                   heard ("ltc:IN000001", "jumped", "29.97df", 107'892, 4'800'123, 1.00005)));
    REQUIRE (leader.engine.submit ("mtc:P0000001", "code.heard",
                                   heard ("mtc:P0000001", "stopped", "25", 90'000, 2'400'000, 0.0)));
    leader.tickOnce (100);
    REQUIRE (leader.runner.codeMaps().size() == 2u);

    const auto at = leader.runner.positionNow (100, 3);
    CHECK (at.codes == leader.runner.codeMaps());

    const auto text = pair::toText (at);
    INFO (text);

    //  One line a map, its unit in `code.heard`'s words, a point and never a comma.
    CHECK (text.find ("\ncode s:\"ltc:IN000001\" s:\"running\" s:\"29.97df\" h:107892 h:4800123 d:1.00005 i:48000\n")
             != std::string::npos);
    CHECK (text.find ("\ncode s:\"mtc:P0000001\" s:\"stopped\" s:\"25\" h:90000 h:2400000 d:") != std::string::npos);
    CHECK (text.find ("d:1,00005") == std::string::npos);

    //  READ BACK EXACTLY: the same maps, the speed to the bit.
    std::string why;
    const auto read = pair::readPosition (text, &why);
    INFO (why);
    REQUIRE (read.has_value());
    CHECK (read->codes == at.codes);
    CHECK (pair::toText (*read) == text);

    //  MOVED WITH THE TICKS: a thousand ticks are 960 000 samples at 48 kHz; nothing else of a map moves.
    {
        auto moved = *read;
        moved.rebase (1000);
        const auto& ltc = moved.codes.at ("ltc:IN000001");
        CHECK (ltc.sample == 4'800'123 + 960'000);
        CHECK (ltc.frame == 107'892);
        CHECK (std::bit_cast<std::uint64_t> (ltc.speed) == std::bit_cast<std::uint64_t> (1.00005));
        CHECK (moved.codes.at ("mtc:P0000001").sample == 2'400'000 + 960'000);
        CHECK (pair::ClockMap {}.sampleOnThisClock (2'400'000, -3, 44100) == 2'400'000 - 3 * 882);
    }

    /*  ANOTHER BUILD'S FORMAT, REFUSED WHOLE AND SAID: the format before TG.5
        read no `code` line, and this build reads no position without one's
        format. A word no position says, and a code line that does not read,
        refused too. */
    {
        auto older = text;
        const auto formatAtom = "position i:" + std::to_string (pair::Position::format) + " ";
        REQUIRE (older.rfind (formatAtom, 0) == 0);
        older.replace (0, formatAtom.size(), "position i:1 ");

        std::string refused;
        CHECK_FALSE (pair::readPosition (older, &refused).has_value());
        CHECK (refused == "a position of format 1; this build reads " + std::to_string (pair::Position::format));

        const auto head = "position i:" + std::to_string (pair::Position::format) + " h:1 h:1\n";
        CHECK_FALSE (pair::readPosition (head + "unheard s:\"A\"\nend\n", &refused).has_value());
        CHECK (refused.find ("which no position says") != std::string::npos);

        CHECK_FALSE (pair::readPosition (head + "code s:\"ltc:IN000001\" s:\"paused\" s:\"25\" h:1 h:1 d:1 i:48000\nend\n").has_value());
        CHECK_FALSE (pair::readPosition (head + "code s:\"ltc:IN000001\" s:\"running\" s:\"25\" h:2160000 h:1 d:1 i:48000\nend\n").has_value());
        CHECK (pair::readPosition (head + "code s:\"ltc:IN000001\" s:\"running\" s:\"25\" h:1 h:1 d:1 i:48000\nend\n").has_value());
    }

    /*  A SEAT HANDS THE MAPS OVER: the follower had a map of its own for the
        LTC - its own watcher's, before it followed - and is left with the
        leader's, on its own clock. */
    REQUIRE (follower.engine.submit ("ltc:IN000001", "code.heard",
                                     heard ("ltc:IN000001", "jumped", "25", 5, 77, 1.0)));
    follower.tickOnce (2999);
    REQUIRE (follower.runner.codeMapOf ("ltc:IN000001") != nullptr);

    const std::int64_t local = 3000;
    auto rebased = at;
    rebased.rebase (local - 100);

    REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (pair::toText (rebased)) }));
    follower.tickOnce (local, false);

    CHECK (follower.runner.codeMaps() == rebased.codes);
    REQUIRE (follower.runner.codeMapOf ("ltc:IN000001") != nullptr);
    CHECK (follower.runner.codeMapOf ("ltc:IN000001")->sample == 4'800'123 + (local - 100) * 960);
    CHECK (follower.runner.codeReadout ("mtc:P0000001", local * 960).state == "stopped");
}

//==============================================================================
/*  A TIME GROUP'S CODE IN A POSITION (namespace draft §60, TG.7): its job's
    `crossed` line - the frame the code has crossed and the members it has
    loaded for times still ahead; for a scene the code placed, the sample it
    starts on, that sample's rate and whether it has begun - read back as it
    was written, its sample moved with the ticks, refused when it does not
    read; and a follower seated while the leader's member is loaded and waiting
    fires that member once, on the sample the code gives it on the follower's
    clock, under the leader's name. */
TEST_CASE ("seat: a time group's crossed line rides the position - read back, its scene's sample moved with the ticks - and a follower seated while a member waits fires it once")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    //  BY HAND: a time group's job and a placed scene's, and a plain one with neither.
    {
        pair::Position at;
        at.tick = 40;

        cue::GroupJob time;
        time.run = "R8M2K4Q1";
        time.phase = cue::groupPhase::members;
        time.codeCrossed = 90'249;
        time.codeAhead = { "C0000001", "C0000002" };
        at.groups.push_back (time);

        cue::GroupJob scene;
        scene.run = "R8M2K4Q2";
        scene.phase = cue::groupPhase::members;
        scene.codeSample = 576'000;
        scene.codeRate = 48000;
        scene.codeCommitted = true;
        at.groups.push_back (scene);

        cue::GroupJob plain;
        plain.run = "R8M2K4Q3";
        at.groups.push_back (plain);

        const auto text = pair::toText (at);
        INFO (text);
        CHECK (text.find ("\ncrossed h:90249 h:0 i:0 ") != std::string::npos);
        CHECK (text.find (" s:\"C0000001\" s:\"C0000002\"\n") != std::string::npos);
        CHECK (text.find ("\ncrossed h:-1 h:576000 i:48000 ") != std::string::npos);

        //  One line for each job that has any of it, and none for the plain one.
        auto lines = 0;

        for (auto from = text.find ("\ncrossed "); from != std::string::npos; from = text.find ("\ncrossed ", from + 1))
            ++lines;

        CHECK (lines == 2);

        std::string why;
        const auto read = pair::readPosition (text, &why);
        INFO (why);
        REQUIRE (read.has_value());
        REQUIRE (read->groups.size() == 3u);
        CHECK (read->groups[0].codeCrossed == 90'249);
        CHECK (read->groups[0].codeAhead == time.codeAhead);
        CHECK (read->groups[0].codeSample == 0);
        CHECK (read->groups[1].codeCrossed == -1);
        CHECK (read->groups[1].codeSample == 576'000);
        CHECK (read->groups[1].codeRate == 48000);
        CHECK (read->groups[1].codeCommitted);
        CHECK (read->groups[2].codeCrossed == -1);
        CHECK (read->groups[2].codeSample == 0);
        CHECK (read->groups[2].codeAhead.empty());
        CHECK_FALSE (read->groups[2].codeCommitted);
        CHECK (pair::toText (*read) == text);

        //  MOVED WITH THE TICKS: the scene's sample, never the code's frame.
        auto moved = *read;
        moved.rebase (1000);
        CHECK (moved.groups[1].codeSample == 576'000 + 960'000);
        CHECK (moved.groups[0].codeCrossed == 90'249);
        CHECK (moved.groups[0].codeSample == 0);

        //  A crossed line with no job before it, or that does not read, refused.
        const auto head = "position i:" + std::to_string (pair::Position::format) + " h:1 h:1\n";
        CHECK_FALSE (pair::readPosition (head + "crossed h:1 h:0 i:0 F\nend\n").has_value());
        CHECK_FALSE (pair::readPosition (head + "job s:\"R1\" s:\"members\" h:0 h:0 s:\"\" s:\"\"\ncrossed h:1 h:0\nend\n")
                       .has_value());
        CHECK_FALSE (pair::readPosition (head + "job s:\"R1\" s:\"members\" h:0 h:0 s:\"\" s:\"\"\ncrossed h:1 h:-5 i:0 F\nend\n")
                       .has_value());
        CHECK (pair::readPosition (head + "job s:\"R1\" s:\"members\" h:0 h:0 s:\"\" s:\"\"\ncrossed h:1 h:0 i:0 F\nend\n")
                 .has_value());
    }

    //  END TO END: the leader's group armed, its member loaded for a time still ahead.
    std::string text, listId, group, rain, source;

    {
        doc::ShowDocument show;
        listId = show.createList ("Sound").id;
        const auto input = show.createInput (1);
        REQUIRE (input.ok);
        REQUIRE (show.setAttribute ("/godot/sync/ltcIn", input.id).ok);
        source = "ltc:" + input.id;
        REQUIRE (show.createCue (listId, 0, "memo", "Before").ok);
        group = show.createCue (listId, 1, "group", "On the code", {}, { { "mode", "time" } }).id;
        REQUIRE (! group.empty());
        rain = show.createCue (group, 0, "media", "Rain", {}, { { "at", "01:00:10:00" } }).id;
        REQUIRE (! rain.empty());
        REQUIRE (show.setAttribute ("/godot/cue/" + rain + "/file", "rain.wav").ok);
        REQUIRE (show.createCue (listId, 2, "memo", "After").ok);
        REQUIRE (show.setAttribute ("/godot/list/goDebounce", "0").ok);
        text = doc::CanonicalXml::write (show);
    }

    Machine leader { text }, follower { text };
    leader.go (10, listId, group);

    for (std::int64_t tick = 12; tick < 520; ++tick)
    {
        if (tick == 100)
            REQUIRE (leader.engine.submit (source, "code.heard",
                                           { s (source), s ("jumped"), s ("25"), osc::Value::int64 (90'000),
                                             osc::Value::int64 (100 * 960), osc::Value::float64 (1.0) }));

        leader.tickOnce (tick);
    }

    const auto loaded = leader.runOf (rain);
    REQUIRE (! loaded.empty());
    REQUIRE (leader.runs.find (loaded)->launchRequestedAtTick == 0);

    const auto at = leader.runner.positionNow (519, 7);
    const auto text519 = pair::toText (at);
    INFO (text519);
    CHECK (text519.find ("\ncrossed h:") != std::string::npos);
    CHECK (text519.find (" s:\"" + rain + "\"\n") != std::string::npos);

    //  SEATED 2481 ticks later on the follower's clock.
    const std::int64_t local = 3000;
    auto rebased = at;
    rebased.rebase (local - 519);
    REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (pair::toText (rebased)) }));
    follower.tickOnce (local, false);
    REQUIRE (follower.runOf (rain) == loaded);

    //  THE LEADER AND THE FOLLOWER EACH FIRE IT ONCE, the follower its offset later.
    const auto start = 100 * 960 + 250 * 1920;

    for (std::int64_t tick = 520; tick < 700; ++tick)
        leader.tickOnce (tick);

    for (std::int64_t tick = local + 1; tick < local + 200; ++tick)
        follower.tickOnce (tick);

    CHECK (leader.runs.find (loaded)->launchedAtSample == start);
    REQUIRE (follower.runs.find (loaded) != nullptr);
    CHECK (follower.runs.find (loaded)->launchedAtSample == start + (local - 519) * 960);

    std::size_t onTheFollower = 0;

    for (const auto& run : follower.runs.all())
        if (run.cue == rain)
            ++onTheFollower;

    CHECK (onTheFollower == 1u);
}

//==============================================================================
/*  A CHASER A LOCATE SEATED, CARRIED BY A SEAT (namespace draft §60, TG.8; RD.8):
    the leader's code locates into the middle of a member that chases, which is
    seated there, under the group; a follower seated from the leader's position
    a while later holds that run under the leader's name, under the leader's
    group, armed at the second the leader's will have reached when it starts -
    as any sound a seat carries. */
TEST_CASE ("seat: a chaser a locate seated is carried by pair.seat - under the leader's name, at the leader's playhead")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    std::string text, listId, group, wind, source;

    {
        doc::ShowDocument show;
        listId = show.createList ("Sound").id;
        const auto input = show.createInput (1);
        REQUIRE (input.ok);
        REQUIRE (show.setAttribute ("/godot/sync/ltcIn", input.id).ok);
        source = "ltc:" + input.id;
        REQUIRE (show.createCue (listId, 0, "memo", "Before").ok);
        group = show.createCue (listId, 1, "group", "On the code", {}, { { "mode", "time" } }).id;
        REQUIRE (! group.empty());
        wind = show.createCue (group, 0, "media", "Wind", {}, { { "at", "01:00:20:00" } }).id;
        REQUIRE (! wind.empty());
        REQUIRE (show.setAttribute ("/godot/cue/" + wind + "/file", "wind.wav").ok);
        REQUIRE (show.setAttribute ("/godot/cue/" + wind + "/chase", "jump").ok);
        REQUIRE (show.createCue (listId, 2, "memo", "After").ok);
        REQUIRE (show.setAttribute ("/godot/list/goDebounce", "0").ok);
        text = doc::CanonicalXml::write (show);
    }

    const std::map<std::string, double> lengths { { "wind.wav", 60.0 } };

    Machine leader { text }, follower { text };
    leader.runner.setMediaDurations (&lengths);
    follower.runner.setMediaDurations (&lengths);
    leader.go (10, listId, group);

    const auto heard = [&source] (std::int64_t frame, std::int64_t sample)
    {
        return std::vector<osc::Value> { s (source), s ("jumped"), s ("25"), osc::Value::int64 (frame),
                                         osc::Value::int64 (sample), osc::Value::float64 (1.0) };
    };

    //  The code from 01:00:00:00 at tick 100; at 300 it locates to 01:00:30:00, ten seconds into the wind.
    for (std::int64_t tick = 12; tick < 400; ++tick)
    {
        if (tick == 100)
            REQUIRE (leader.engine.submit (source, "code.heard", heard (90'000, 100 * 960)));

        if (tick == 300)
            REQUIRE (leader.engine.submit (source, "code.heard", heard (90'750, 300 * 960)));

        leader.tickOnce (tick);
    }

    const auto seated = leader.runOf (wind);
    REQUIRE (! seated.empty());
    const auto* leaderRun = leader.runs.find (seated);
    REQUIRE (leaderRun->launchedAtSample == 300 * 960 + 10 * 960);
    REQUIRE (leaderRun->startOffset == doctest::Approx (10.2));
    REQUIRE (leaderRun->position > 10.2);

    const auto at = leader.runner.positionNow (399, 9);
    const auto leaderPosition = leaderRun->position;
    const auto leaderParent = leaderRun->parent;

    //  SEATED 2601 ticks later on the follower's clock.
    const std::int64_t local = 3000;
    auto rebased = at;
    rebased.rebase (local - 399);
    REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (pair::toText (rebased)) }));
    follower.tickOnce (local, false);

    REQUIRE (follower.runOf (wind) == seated);
    const auto* carried = follower.runs.find (seated);
    REQUIRE (carried != nullptr);
    CHECK (carried->parent == leaderParent);

    //  At the leader's playhead, as far on as the leader's will be when this one starts: the launch latency and the seat's lead.
    const auto lead = static_cast<double> (follower.runner.latencyTicks() + cue::Runner::seatLeadTicks) / 50.0;
    CHECK (carried->startOffset == doctest::Approx (leaderPosition + lead).epsilon (1e-9));

    std::size_t onTheFollower = 0;

    for (const auto& run : follower.runs.all())
        if (run.cue == wind)
            ++onTheFollower;

    CHECK (onTheFollower == 1u);
}

TEST_CASE ("seat: a text cue's fields ride the position as field lines - read back exactly, a line break and all, untouched by a rebase, and a seat hands them over (§61, AKM)")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const Show show;
    Machine leader { show.text }, follower { show.text };

    cue::TextFields leaderFields, followerFields;
    leader.runner.setTextFields (&leaderFields);
    follower.runner.setTextFields (&followerFields);

    REQUIRE (leaderFields.set ("home", "3") == cue::TextFields::Set::written);
    REQUIRE (leaderFields.set ("line", "Act One\nThe forest, \"night\"") == cue::TextFields::Set::written);

    const auto at = leader.runner.positionNow (100, 3);
    CHECK (at.fields == leaderFields.all());

    const auto text = pair::toText (at);
    INFO (text);
    CHECK (text.find ("\nfield s:\"home\" s:\"3\"\n") != std::string::npos);

    //  READ BACK EXACTLY, the line break and the quotes in a value too.
    std::string why;
    const auto read = pair::readPosition (text, &why);
    INFO (why);
    REQUIRE (read.has_value());
    CHECK (read->fields == at.fields);
    CHECK (pair::toText (*read) == text);

    //  No tick in a field: a rebase leaves them.
    auto moved = *read;
    moved.rebase (1000);
    CHECK (moved.fields == at.fields);

    //  A field line whose name does not read is no position.
    const auto head = "position i:" + std::to_string (pair::Position::format) + " h:1 h:1\n";
    CHECK_FALSE (pair::readPosition (head + "field s:\"two words\" s:\"x\"\nend\n").has_value());
    CHECK (pair::readPosition (head + "field s:\"score\" s:\"x\"\nend\n").has_value());

    /*  A SEAT HANDS THEM OVER: the follower's own - set before it followed -
        give way to the leader's, a field the leader has not got forgotten. */
    REQUIRE (followerFields.set ("away", "9") == cue::TextFields::Set::written);
    follower.tickOnce (2999);

    auto rebased = at;
    rebased.rebase (3000 - 100);

    REQUIRE (follower.engine.submit (std::string (origin::engine), "pair.seat", { s (pair::toText (rebased)) }));
    follower.tickOnce (3000, false);

    CHECK (followerFields.all() == leaderFields.all());
    CHECK (followerFields.valueOf ("away").empty());
}
