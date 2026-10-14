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

#include <wfg/engine/pair/Position.h>

#include <wfg/engine/video/Blanks.h>

#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <utility>

namespace wfg::pair
{
    namespace
    {
        /*  A NUMBER THE GRAMMAR CAN SAY: the log refuses a NaN or an infinity
            wherever one arrives, so a level that is somehow not a number goes
            as far below silence as matters, and never as a line that would not
            read back. */
        double sayable (double value) noexcept
        {
            if (std::isfinite (value))
                return value;

            return std::isnan (value) ? 0.0 : (value > 0.0 ? 1.0e9 : -1.0e9);
        }

        //======================================================================
        /*  ONE LINE BEING WRITTEN: its word, then each atom as the log spells it. */
        class Line
        {
        public:
            Line (std::string& outToFill, const char* word) : out (outToFill)
            {
                out += word;
            }

            ~Line() { out += '\n'; }

            Line (const Line&) = delete;
            Line& operator= (const Line&) = delete;

            Line& s (const std::string& text)   { return atom (osc::Value::string (text)); }
            Line& h (std::int64_t value)        { return atom (osc::Value::int64 (value)); }
            Line& u (std::uint64_t value)       { return atom (osc::Value::int64 (static_cast<std::int64_t> (value))); }
            Line& i (int value)                 { return atom (osc::Value::int32 (static_cast<std::int32_t> (value))); }
            Line& d (double value)              { return atom (osc::Value::float64 (sayable (value))); }
            Line& b (bool value)                { return atom (osc::Value::boolean (value)); }

        private:
            Line& atom (const osc::Value& value)
            {
                out += ' ';
                out += value.toAtom();
                return *this;
            }

            std::string& out;
        };

        //======================================================================
        /*  ONE LINE BEING READ: its atoms in order, each taken as the type the
            line's word says it is. A wrong type, or too few, marks it bad and
            reads as nought from then on, so a line is judged once at its end. */
        class Atoms
        {
        public:
            explicit Atoms (std::vector<osc::Value> valuesToRead) : values (std::move (valuesToRead)) {}

            std::string s()
            {
                const auto* value = next();
                return value != nullptr && value->isString() ? value->getString() : fail<std::string>();
            }

            std::int64_t h()
            {
                const auto* value = next();
                return value != nullptr && value->isInt64() ? value->getInt64() : fail<std::int64_t>();
            }

            /*  AN UNSIGNED 64-BIT NUMBER, carried as the grammar's int64 bit for
                bit: a salt uses all sixty-four. */
            std::uint64_t u()
            {
                return static_cast<std::uint64_t> (h());
            }

            int i()
            {
                const auto* value = next();
                return value != nullptr && value->isInt32() ? static_cast<int> (value->getInt32()) : fail<int>();
            }

            double d()
            {
                const auto* value = next();
                return value != nullptr && value->isFloat64() ? value->getFloat64() : fail<double>();
            }

            bool b()
            {
                const auto* value = next();
                return value != nullptr && value->isBool() ? value->getBool() : fail<bool>();
            }

            /** What is left, each a string: a list's members, a job's runs. */
            std::vector<std::string> rest()
            {
                std::vector<std::string> out;

                while (at < values.size())
                    out.push_back (s());

                return out;
            }

            /** Every atom taken, and every one the type it should have been. */
            bool ok() const noexcept { return good && at == values.size(); }

        private:
            const osc::Value* next()
            {
                return at < values.size() ? &values[at++] : nullptr;
            }

            template <typename T>
            T fail()
            {
                good = false;
                return T {};
            }

            std::vector<osc::Value> values;
            std::size_t at = 0;
            bool good = true;
        };

        std::string originWord (char origin)
        {
            return std::string (1, origin);
        }
    }

    //==========================================================================
    void Position::rebase (std::int64_t by, const ClockMap& map)
    {
        /*  A TICK THAT IS SET STAYS SET: moved onto a clock that started later,
            a moment before it started is below nought, and one that lands
            exactly on the value meaning "never" is put a tick before it. */
        const auto moved = [by] (std::int64_t& tickToMove, std::int64_t unset)
        {
            if (tickToMove == unset)
                return;

            tickToMove += by;

            if (tickToMove == unset)
                --tickToMove;
        };

        tick += by;

        for (auto& list : lists)
            for (auto& step : list.history)
                step.tick += by;

        moved (lastGoTick, -1);
        moved (lastDohTick, -1);
        moved (liftedAt, -1);

        for (auto& run : runs)
        {
            moved (run.launchTick, 0);
            moved (run.dueTick, 0);
            moved (run.postWaitBegan, -1);
            moved (run.startedTick, -1);
            moved (run.endedTick, -1);

            //  A round's clocks mean something only once a round has begun.
            if (run.iteration >= 1)
            {
                run.roundStartedTick += by;
                run.firstRoundTick += by;
            }
        }

        for (auto& fade : fades)
            moved (fade.stopsAtTick, 0);

        /*  A SCENE THE CODE PLACED STARTS ON A SAMPLE of the leader's clock
            (namespace draft §60, TG.7), moved with its ticks as a code map's
            anchor is. The frame a time group crossed is the code's, and stays. */
        for (auto& job : groups)
            if (job.codeSample > 0)
                job.codeSample = map.sampleOnThisClock (job.codeSample, by, job.codeRate);

        //  A CODE MAP'S ANCHOR IS A SAMPLE of the leader's clock, moved with its ticks (TG.5).
        for (auto& [source, codeMap] : codes)
            codeMap.sample = map.sampleOnThisClock (codeMap.sample, by, codeMap.sampleRate);
    }

    const SeatedRun* Position::runNamed (const std::string& id) const
    {
        for (const auto& run : runs)
            if (run.id == id)
                return &run;

        return nullptr;
    }

    //==========================================================================
    std::string toText (const Position& at)
    {
        std::string out;
        out.reserve (512 + 400 * at.runs.size());

        Line (out, "position").i (Position::format).u (at.counter).h (at.tick);

        for (const auto& row : at.rows)
            Line (out, "row").s (row.address).s (row.value);

        for (const auto& list : at.lists)
        {
            Line (out, "list").s (list.id).s (list.aim.cue).d (list.aim.offset).s (list.landed.cue).d (list.landed.offset);

            for (const auto& step : list.history)
                Line (out, "step").h (step.tick).s (step.cue).s (originWord (step.origin)).u (step.serial);
        }

        Line (out, "go").h (at.lastGoTick).h (at.lastDohTick).u (at.goCount);

        for (const auto& run : at.runs)
        {
            Line (out, "run")
                .s (run.id).s (run.cue).s (run.kind).s (run.parent).s (run.state)
                .h (run.launchTick).h (run.dueTick).h (run.postWaitBegan).h (run.startedTick).h (run.endedTick)
                .b (run.launchRequested).b (run.launched)
                .d (run.position).d (run.slicePlayed).i (run.range).i (run.pass).d (run.rate).d (run.ownRate)
                .i (run.iteration).i (run.iterations).i (run.seed).h (run.roundStartedTick).h (run.firstRoundTick)
                .s (run.stopAfter).s (run.enterAt)
                .d (run.ownLevel).d (run.trim).s (run.strip).b (run.sampler).b (run.held).s (run.heldBy).b (run.solo)
                .b (run.closing)
                .b (run.stopAsked).b (run.stopEndsWait).b (run.skipFooter).b (run.killed).b (run.asserted)
                .b (run.advanceRequested).i (run.advanceTo).u (run.goSerial).b (run.curving);

            //  The lists a run carries, each on a line of its own after it.
            if (! run.round.empty())
            {
                Line line (out, "round");

                for (const auto& member : run.round)
                    line.s (member);
            }

            if (! run.pruned.empty())
            {
                Line line (out, "pruned");

                for (const auto& member : run.pruned)
                    line.s (member);
            }

            if (! run.lostStrips.empty())
            {
                Line line (out, "lost");

                for (const auto& strip : run.lostStrips)
                    line.s (strip);
            }

            for (const auto& [entry, value] : run.moved)
                Line (out, "moved").s (entry).d (value);
        }

        for (const auto& job : at.groups)
        {
            Line (out, "job").s (job.run).s (job.phase).u (job.nextMember).u (job.launched).s (job.awaiting)
                             .s (job.enterAt);

            const auto list = [&out] (const char* word, const std::vector<std::string>& items)
            {
                Line line (out, word);

                for (const auto& item : items)
                    line.s (item);
            };

            list ("jobruns", job.phaseRuns);
            list ("jobtaken", job.taken);
            list ("jobcues", job.phaseCues);
            list ("jobprepared", job.prepared);

            /*  WHERE A TIME GROUP'S CODE HAS GOT TO (namespace draft §60, TG.7):
                the frame it has crossed and the members it has loaded for times
                still ahead - so a follower seated while a code runs starts what
                the leader had still to start, and none of it twice - and, for a
                scene the code placed, the sample it starts on, the rate that
                sample counts at (which `rebase` moves it by) and whether it has
                begun. Only for a job that has any of it: every other scene's
                position reads as it did. */
            if (job.codeCrossed >= 0 || job.codeSample > 0 || ! job.codeAhead.empty())
            {
                Line line (out, "crossed");
                line.h (job.codeCrossed).h (job.codeSample).i (job.codeRate).b (job.codeCommitted);

                for (const auto& cue : job.codeAhead)
                    line.s (cue);
            }
        }

        for (const auto& fade : at.fades)
        {
            Line (out, "fade")
                .s (fade.target).s (fade.dca).b (fade.movesRate).d (fade.fromRate).d (fade.toRate)
                .s (fade.move).s (fade.moveRun).i (static_cast<int> (fade.moveDomain)).d (fade.fromValue).d (fade.toValue)
                .s (fade.self).b (fade.reportsSelf).d (fade.fromDb).d (fade.toDb).i (fade.ticksTotal).i (fade.ticksDone)
                .i (static_cast<int> (fade.curve)).b (fade.stopWhenDone).h (fade.stopsAtTick).s (fade.failure)
                .b (fade.waitsForLaunch);

            for (const auto& point : fade.points)
                Line (out, "fadepoint").d (point.t).d (point.levelDb);

            if (fade.then.has_value())
            {
                const auto& then = *fade.then;
                Line (out, "fadethen").d (then.toDb).i (then.ticks).i (static_cast<int> (then.curve)).b (then.stopWhenDone);

                for (const auto& point : then.points)
                    Line (out, "fadethenpoint").d (point.t).d (point.levelDb);
            }
        }

        if (! at.finishing.empty())
        {
            Line line (out, "finishing");

            for (const auto& id : at.finishing)
                line.s (id);
        }

        for (const auto& [dca, trim] : at.dcas)
            Line (out, "dca").s (dca).d (trim);

        /*  EACH INCOMING CODE'S MAP (TG.5), a top-level line of its own beside
            the trims - tonight's numbers, which no show holds - its unit in
            `code.heard`'s words and its clock's rate last, which is what
            `rebase` moves its sample by. */
        for (const auto& [source, map] : at.codes)
            Line (out, "code")
                .s (source).s (std::string (timecode::wordOf (map.state))).s (timecode::unitWordOf (map.unit))
                .h (map.frame).h (map.sample).d (map.speed).i (map.sampleRate);

        //  EACH TEXT CUE'S FIELD (§61, AKM): its name and its value tonight.
        for (const auto& [name, value] : at.fields)
            Line (out, "field").s (name).s (value);

        if (! at.laneCue.empty())
        {
            Line line (out, "lane");
            line.s (at.laneCue);

            for (const auto& key : at.laneArmed)
                line.s (key);
        }

        if (! at.suspended.empty())
        {
            Line line (out, "suspended");

            for (const auto& cue : at.suspended)
                line.s (cue);
        }

        Line (out, "lifted").h (at.liftedAt);

        for (const auto& bed : at.paused)
            Line (out, "paused").s (bed.cue).s (bed.run).b (bed.resumes).d (bed.from).i (bed.range).s (bed.media);

        Line (out, "salt").u (at.names.salt);

        for (const auto& [key, count] : at.names.counts)
            Line (out, "count").s (key.first).s (key.second).u (count);

        Line (out, "end");
        return out;
    }

    //==========================================================================
    std::optional<Position> readPosition (std::string_view text, std::string* why)
    {
        const auto refuse = [why] (std::string reason) -> std::optional<Position>
        {
            if (why != nullptr)
                *why = std::move (reason);

            return std::nullopt;
        };

        Position at;
        auto begun = false;
        auto ended = false;
        auto lineNumber = 0;

        while (! text.empty())
        {
            const auto newline = text.find ('\n');
            const auto line = text.substr (0, newline);
            text = newline == std::string_view::npos ? std::string_view {} : text.substr (newline + 1);
            ++lineNumber;

            if (line.empty())
                continue;

            if (ended)
                return refuse ("line " + std::to_string (lineNumber) + " follows the end");

            const auto space = line.find (' ');
            const auto word = line.substr (0, space);
            std::vector<osc::Value> values;

            if (space != std::string_view::npos)
            {
                const auto split = osc::splitAtoms (line.substr (space + 1));

                if (! split.has_value())
                    return refuse ("line " + std::to_string (lineNumber) + " has a quote left open");

                for (const auto atom : *split)
                {
                    auto value = osc::Value::fromAtom (atom);

                    if (! value.has_value())
                        return refuse ("line " + std::to_string (lineNumber) + " has an atom that does not read: "
                                       + std::string (atom));

                    values.push_back (std::move (*value));
                }
            }

            Atoms a { std::move (values) };
            const auto bad = [&refuse, &word, lineNumber]
            {
                return refuse ("line " + std::to_string (lineNumber) + " (" + std::string (word) + ") is not one");
            };

            if (! begun)
            {
                if (word != "position")
                    return refuse ("a position begins with `position`, not `" + std::string (word) + "`");

                const auto format = a.i();
                at.counter = a.u();
                at.tick = a.h();

                if (! a.ok())
                    return bad();

                if (format != Position::format)
                    return refuse ("a position of format " + std::to_string (format) + "; this build reads "
                                   + std::to_string (Position::format));

                begun = true;
                continue;
            }

            /*  WHAT A LINE AFTER ANOTHER BELONGS TO: a step its list, a round its
                run, a point its fade - the last of each read. */
            const auto lastRun = [&at]() -> SeatedRun* { return at.runs.empty() ? nullptr : &at.runs.back(); };
            const auto lastJob = [&at]() -> cue::GroupJob* { return at.groups.empty() ? nullptr : &at.groups.back(); };
            const auto lastFade = [&at]() -> cue::FadeJob* { return at.fades.empty() ? nullptr : &at.fades.back(); };

            if (word == "row")
            {
                Position::Row row;
                row.address = a.s();
                row.value = a.s();

                if (! a.ok())
                    return bad();

                at.rows.push_back (std::move (row));
            }
            else if (word == "list")
            {
                Position::List list;
                list.id = a.s();
                list.aim.cue = a.s();
                list.aim.offset = a.d();
                list.landed.cue = a.s();
                list.landed.offset = a.d();

                if (! a.ok())
                    return bad();

                at.lists.push_back (std::move (list));
            }
            else if (word == "step")
            {
                cue::Step step;
                step.tick = a.h();
                step.cue = a.s();
                const auto origin = a.s();
                step.serial = a.u();

                if (! a.ok() || origin.size() != 1 || at.lists.empty())
                    return bad();

                step.origin = origin[0];
                at.lists.back().history.push_back (std::move (step));
            }
            else if (word == "go")
            {
                at.lastGoTick = a.h();
                at.lastDohTick = a.h();
                at.goCount = a.u();

                if (! a.ok())
                    return bad();
            }
            else if (word == "run")
            {
                SeatedRun run;
                run.id = a.s();
                run.cue = a.s();
                run.kind = a.s();
                run.parent = a.s();
                run.state = a.s();
                run.launchTick = a.h();
                run.dueTick = a.h();
                run.postWaitBegan = a.h();
                run.startedTick = a.h();
                run.endedTick = a.h();
                run.launchRequested = a.b();
                run.launched = a.b();
                run.position = a.d();
                run.slicePlayed = a.d();
                run.range = a.i();
                run.pass = a.i();
                run.rate = a.d();
                run.ownRate = a.d();
                run.iteration = a.i();
                run.iterations = a.i();
                run.seed = static_cast<std::int32_t> (a.i());
                run.roundStartedTick = a.h();
                run.firstRoundTick = a.h();
                run.stopAfter = a.s();
                run.enterAt = a.s();
                run.ownLevel = a.d();
                run.trim = a.d();
                run.strip = a.s();
                run.sampler = a.b();
                run.held = a.b();
                run.heldBy = a.s();
                run.solo = a.b();
                run.closing = a.b();
                run.stopAsked = a.b();
                run.stopEndsWait = a.b();
                run.skipFooter = a.b();
                run.killed = a.b();
                run.asserted = a.b();
                run.advanceRequested = a.b();
                run.advanceTo = a.i();
                run.goSerial = a.u();
                run.curving = a.b();

                if (! a.ok() || run.id.empty() || run.cue.empty())
                    return bad();

                at.runs.push_back (std::move (run));
            }
            else if (word == "round" || word == "pruned" || word == "lost")
            {
                auto* run = lastRun();
                auto items = a.rest();

                if (run == nullptr || ! a.ok())
                    return bad();

                (word == "round" ? run->round : word == "pruned" ? run->pruned : run->lostStrips) = std::move (items);
            }
            else if (word == "moved")
            {
                auto* run = lastRun();
                const auto entry = a.s();
                const auto value = a.d();

                if (run == nullptr || ! a.ok())
                    return bad();

                run->moved[entry] = value;
            }
            else if (word == "job")
            {
                cue::GroupJob job;
                job.run = a.s();
                job.phase = a.s();
                job.nextMember = static_cast<std::size_t> (a.u());
                job.launched = static_cast<std::size_t> (a.u());
                job.awaiting = a.s();
                job.enterAt = a.s();

                if (! a.ok() || job.run.empty())
                    return bad();

                at.groups.push_back (std::move (job));
            }
            else if (word == "jobruns" || word == "jobtaken" || word == "jobcues" || word == "jobprepared")
            {
                auto* job = lastJob();
                auto items = a.rest();

                if (job == nullptr || ! a.ok())
                    return bad();

                (word == "jobruns" ? job->phaseRuns
                   : word == "jobtaken" ? job->taken
                   : word == "jobcues" ? job->phaseCues : job->prepared) = std::move (items);
            }
            else if (word == "crossed")
            {
                auto* job = lastJob();

                if (job == nullptr)
                    return bad();

                job->codeCrossed = a.h();
                job->codeSample = a.h();
                job->codeRate = a.i();
                job->codeCommitted = a.b();
                job->codeAhead = a.rest();

                if (! a.ok() || job->codeCrossed < -1 || job->codeSample < 0 || job->codeRate < 0)
                    return bad();
            }
            else if (word == "fade")
            {
                cue::FadeJob fade;
                fade.target = a.s();
                fade.dca = a.s();
                fade.movesRate = a.b();
                fade.fromRate = a.d();
                fade.toRate = a.d();
                fade.move = a.s();
                fade.moveRun = a.s();
                const auto domain = a.i();
                fade.fromValue = a.d();
                fade.toValue = a.d();
                fade.self = a.s();
                fade.reportsSelf = a.b();
                fade.fromDb = a.d();
                fade.toDb = a.d();
                fade.ticksTotal = a.i();
                fade.ticksDone = a.i();
                const auto curve = a.i();
                fade.stopWhenDone = a.b();
                fade.stopsAtTick = a.h();
                fade.failure = a.s();
                fade.waitsForLaunch = a.b();

                if (! a.ok() || domain < 0 || domain > 2 || curve < 0 || curve > 1)
                    return bad();

                fade.moveDomain = static_cast<cue::MoveDomain> (domain);
                fade.curve = static_cast<cue::FadeCurve> (curve);
                at.fades.push_back (std::move (fade));
            }
            else if (word == "fadepoint" || word == "fadethenpoint")
            {
                auto* fade = lastFade();
                doc::FadePoint point;
                point.t = a.d();
                point.levelDb = a.d();

                if (fade == nullptr || ! a.ok() || (word == "fadethenpoint" && ! fade->then.has_value()))
                    return bad();

                (word == "fadepoint" ? fade->points : fade->then->points).push_back (point);
            }
            else if (word == "fadethen")
            {
                auto* fade = lastFade();
                cue::FadeSegment then;
                then.toDb = a.d();
                then.ticks = a.i();
                const auto curve = a.i();
                then.stopWhenDone = a.b();

                if (fade == nullptr || ! a.ok() || curve < 0 || curve > 1)
                    return bad();

                then.curve = static_cast<cue::FadeCurve> (curve);
                fade->then = std::move (then);
            }
            else if (word == "dca")
            {
                const auto dca = a.s();
                const auto trim = a.d();

                if (! a.ok())
                    return bad();

                at.dcas[dca] = trim;
            }
            else if (word == "field")
            {
                const auto name = a.s();
                const auto value = a.s();

                if (! a.ok() || ! video::isFieldName (name))
                    return bad();

                at.fields[name] = value;
            }
            else if (word == "code")
            {
                const auto source = a.s();
                const auto state = timecode::codeStateFromWord (a.s());
                const auto unit = timecode::unitFromWord (a.s());

                timecode::CodeMap map;
                map.frame = a.h();
                map.sample = a.h();
                map.speed = a.d();
                map.sampleRate = a.i();

                if (! a.ok() || source.empty() || ! state.has_value() || ! unit.has_value()
                      || map.frame < 0 || map.frame >= unit->perDay)
                    return bad();

                map.state = *state;
                map.unit = *unit;
                at.codes[source] = map;
            }
            else if (word == "lane")
            {
                auto items = a.rest();

                if (! a.ok() || items.empty())
                    return bad();

                at.laneCue = items.front();
                at.laneArmed.assign (items.begin() + 1, items.end());
            }
            else if (word == "finishing")
            {
                at.finishing = a.rest();

                if (! a.ok())
                    return bad();
            }
            else if (word == "suspended")
            {
                at.suspended = a.rest();

                if (! a.ok())
                    return bad();
            }
            else if (word == "lifted")
            {
                at.liftedAt = a.h();

                if (! a.ok())
                    return bad();
            }
            else if (word == "paused")
            {
                Position::Paused bed;
                bed.cue = a.s();
                bed.run = a.s();
                bed.resumes = a.b();
                bed.from = a.d();
                bed.range = a.i();
                bed.media = a.s();

                if (! a.ok())
                    return bad();

                at.paused.push_back (std::move (bed));
            }
            else if (word == "salt")
            {
                at.names.salt = a.u();

                if (! a.ok())
                    return bad();
            }
            else if (word == "count")
            {
                const auto cause = a.s();
                const auto cue = a.s();
                const auto count = a.u();

                if (! a.ok())
                    return bad();

                at.names.counts[{ cause, cue }] = count;
            }
            else if (word == "end")
            {
                if (! a.ok())
                    return bad();

                ended = true;
            }
            else
            {
                return refuse ("line " + std::to_string (lineNumber) + " says `" + std::string (word)
                               + "`, which no position says");
            }
        }

        if (! begun)
            return refuse ("an empty position");

        if (! ended)
            return refuse ("a position cut short: no `end`");

        return at;
    }

    std::optional<std::string> rebasedText (std::string_view text, std::int64_t by, const ClockMap& map)
    {
        auto at = readPosition (text);

        if (! at.has_value())
            return std::nullopt;

        at->rebase (by, map);
        return toText (*at);
    }
}
