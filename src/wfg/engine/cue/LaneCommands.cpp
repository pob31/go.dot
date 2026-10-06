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

#include <wfg/engine/cue/LaneCommands.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/cue/ShowWalk.h>
#include <wfg/engine/document/LevelLane.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace wfg::cue
{
    std::vector<std::string> flippedLanes (const doc::ShowDocument& document)
    {
        std::vector<std::pair<int, std::string>> mixes;

        for (const auto& bus : document.root().getChildWithName ("Audio"))
        {
            if (! bus.hasType ("Bus") || ! bus.hasProperty (idProperty))
                continue;

            //  An absent kind is the row's default, a direct out.
            if (bus.getProperty ("kind").toString() != "mix")
                continue;

            mixes.emplace_back (static_cast<int> (bus.getProperty ("firstChannel", 0)),
                                bus[idProperty].toString().toStdString());
        }

        std::sort (mixes.begin(), mixes.end());

        std::vector<std::string> out { levelLaneKey };

        for (auto& [first, id] : mixes)
            out.push_back (std::move (id));

        return out;
    }

    namespace
    {
        bool isLaneOf (const doc::ShowDocument& document, const std::string& key)
        {
            const auto keys = flippedLanes (document);
            return std::find (keys.begin(), keys.end(), key) != keys.end();
        }

        /** The cue's send into a mix, or an invalid tree when it sends nowhere there. */
        juce::ValueTree sendInto (const juce::ValueTree& cue, const std::string& busId)
        {
            for (const auto& child : cue)
                if (child.hasType ("Send") && child[juce::Identifier ("bus")].toString().toStdString() == busId)
                    return child;

            return {};
        }
    }

    void registerLaneCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                               doc::ShowDocument& document, LaneTable& lanes)
    {
        /*  THE FLIP (namespace draft §34, UI and UJ): every fader surface shows
            this cue's level and its sends, a fader to a lane, until the faders
            are flipped back. The window's, and the window's alone (UJ). */
        registry.add ({ "lane.arm",
                        "Flips every fader surface to a media cue's level and sends, a fader to each"
                        " of its lanes, to be armed by their REC and recorded. Empty flips them back.",
                        { { "cue", 's', false } },
                        true,
                        [&document, &lanes] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            /*  A PASS IS A DECISION HALF MADE: the lanes it ends
                                in are written when it stops, and moving the
                                faders to another cue under it would leave that
                                half nowhere. */
                            if (lanes.recording)
                                return Outcome::rejected (reason::busy);

                            if (id.empty())
                            {
                                lanes.free();
                                return Outcome::ok (args);
                            }

                            const auto cue = document.findById (id);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            if (! cue.hasType ("Media"))
                                return Outcome::rejected (reason::badValue);

                            /*  A LANE IS THE SHOW'S, and the lock keeps the show:
                                a pass would end in a write the lock refuses, so it
                                is refused here, before a hand has ridden anything. */
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            lanes.flip (id);
                            return Outcome::ok (args);
                        } });

        registry.add ({ "lane.free",
                        "Flips the faders back to what they rode, and forgets the cue and its REC choices.",
                        {},
                        true,
                        [&lanes] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (lanes.recording)
                                return Outcome::rejected (reason::busy);

                            lanes.free();
                            return Outcome::ok (args);
                        } });

        /*  A STRIP'S REC (UI): one lane armed or not, by its key - `level`, or
            a mix's identifier. During a pass too (UL): armed, it is written
            from its next touch; disarmed, what it rode is kept and the lane
            plays as written from then on (UP). An explicit switch rather than
            a toggle, so a record says what the light showed. */
        registry.add ({ "lane.rec",
                        "Arms or disarms one lane of the cue the faders are flipped to - level, or a"
                        " mix's identifier for the cue's send into it - before a pass or during one.",
                        { { "lane", 's', false }, { "on", 'T', false } },
                        true,
                        [&document, &lanes] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (! lanes.flipped())
                                return Outcome::rejected (reason::notFlipped);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto key = args[0].getString();

                            if (! isLaneOf (document, key))
                                return Outcome::rejected (reason::badValue);

                            lanes.setArmed (key, args[1].getBool());
                            return Outcome::ok (args);
                        } });

        /*  THE PASS STARTS BY FIRING THE CUE the way `cue.fire` does - not a GO,
            so the GO window (§21.2) never holds it back and the standby does not
            move - and AT THE SECOND ASKED, nought being the cue's own start
            (2026-10-05, namespace draft §30.4): a cue already sounding is
            moved there rather than recorded wherever it had got to. The
            Runner's `startLanePass` says how. The run's identifier is drawn
            here and written into the record, so a replay re-supplies it, as
            `cue.fire` does. A pass with nothing armed is a pass: a REC can join
            it (UL). */
        registry.add ({ "lane.record",
                        "Starts a pass on the cue the faders are flipped to: it plays from the second"
                        " given, or from its own start with none, and every armed lane is written"
                        " from its fader's first touch until the pass stops.",
                        { { "from", 'd', true }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, &lanes]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            if (! lanes.flipped())
                                return Outcome::rejected (reason::notFlipped);

                            if (lanes.recording)
                                return Outcome::rejected (reason::busy);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            if (! document.findById (lanes.cue()).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto from = ! args.empty() ? args[0].getFloat64() : 0.0;
                            const auto supplied = args.size() > 1 ? args[1].getString() : std::string {};
                            const auto runId = runner.startLanePass (engine, context.tick, lanes.cue(),
                                                                     from, supplied);

                            if (runId.empty())
                                return Outcome::rejected (reason::badValue);

                            lanes.startPass (runId);

                            return Outcome::ok ({ osc::Value::float64 (from), osc::Value::string (runId) });
                        } });

        /*  WHAT A PASS ENDS IN, ONE STEP OF UNDO (namespace draft §34, UQ): the
            Runner's, once a pass that rode something is over. For each lane, its
            key, the lane's whole text, and the send it is written on - the
            level's has none, nor has a mix the cue did not send to, which is
            given a send here, at nought, holding the lane (§29's convention for
            a moving send). The identifier drawn for it goes into the record, so
            a replay makes the same send. Judged whole before anything is
            written: every lane or none. */
        registry.add ({ "lane.write",
                        "Writes the lanes a pass rode on a media cue, in one step of undo: for each,"
                        " its key - level, or a mix's identifier - its text, and the send it is on,"
                        " a mix the cue does not send to being given a send at nought.",
                        { { "cue", 's', false }, { "lanes", 's', false, true } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            if (! cue.hasType ("Media"))
                                return Outcome::rejected (reason::badValue);

                            if ((args.size() - 1) % 3 != 0)
                                return Outcome::rejected (reason::arity);

                            std::vector<std::string> seen;

                            for (std::size_t at = 1; at < args.size(); at += 3)
                            {
                                const auto key = args[at].getString();

                                if (! isLaneOf (document, key)
                                      || std::find (seen.begin(), seen.end(), key) != seen.end()
                                      || ! doc::readLevelLane (args[at + 1].getString()).problem.empty())
                                    return Outcome::rejected (reason::badValue);

                                seen.push_back (key);
                            }

                            auto said = args;

                            for (std::size_t at = 1; at < args.size(); at += 3)
                            {
                                const auto key = args[at].getString();
                                const auto text = args[at + 1].getString();
                                std::string address;

                                if (key == levelLaneKey)
                                {
                                    address = "/godot/cue/" + cueId + "/levelLane";
                                }
                                else
                                {
                                    auto sendId = sendInto (cue, key)[idProperty].toString().toStdString();

                                    if (sendId.empty())
                                    {
                                        const auto made = document.createSend (cueId, key, args[at + 2].getString(), "0");

                                        if (! made.ok)
                                            return Outcome::rejected (made.reason);

                                        sendId = made.id;
                                    }

                                    said[at + 2] = osc::Value::string (sendId);
                                    address = "/godot/send/" + sendId + "/levelLane";
                                }

                                if (const auto written = document.setAttribute (address, text); ! written.ok)
                                    return Outcome::rejected (written.reason);
                            }

                            return Outcome::ok (std::move (said));
                        } });

        /*  TWO VOICES, ONE VERB. With no argument it is a hand asking the pass
            to end - the window's stop, the D700's Rec - and only marks it: the
            Runner holds the samples and writes the lanes on its next tick. With
            a word it is the Runner saying it has, which ends the pass; logged
            after the lanes' own `lane.write`, so a replay reads the ask, the
            lanes and the end in the order they happened.

            THE FADERS STAY FLIPPED (2026-10-06, UM, the author's decision), with
            their REC choices: the next pass rides the next lane. What it ended
            in is kept for the window to say (namespace draft §30.4): `kept`
            with the points the pass wrote, the seconds they span and the lanes
            it wrote; `untouched` when nobody rode an armed fader while the cue
            sounded, and there is nothing to write; `locked` when the show was
            locked under the pass, which keeps the lanes as they were; `dropped`
            when a kill took the pass. */
        registry.add ({ "lane.stop",
                        "Ends the pass: with no argument, asks it to end; with kept, untouched,"
                        " locked or dropped, says the lanes have been written (how many points, over"
                        " which seconds, which lanes), that nobody rode an armed fader, that the lock"
                        " kept the lanes, or that the pass was dropped.",
                        { { "how", 's', true }, { "points", 'i', true },
                          { "from", 'd', true }, { "to", 'd', true }, { "lanes", 's', true } },
                        true,
                        [&lanes] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            if (! lanes.recording)
                                return Outcome::rejected (reason::notRunning);

                            if (args.empty() || args[0].getString().empty())
                            {
                                lanes.stopping = true;
                                return Outcome::ok (args);
                            }

                            const auto how = args[0].getString();

                            if (how != "kept" && how != "untouched" && how != "locked" && how != "dropped")
                                return Outcome::rejected (reason::badValue);

                            auto said = std::to_string (context.tick) + " " + lanes.cue() + " " + how;

                            if (how == "kept" && args.size() >= 4)
                                said += " " + std::to_string (args[1].getInt32())
                                          + " " + osc::formatDouble (args[2].getFloat64())
                                          + " " + osc::formatDouble (args[3].getFloat64());

                            if (how == "kept" && args.size() >= 5)
                                said += " " + args[4].getString();

                            lanes.endPass (said);
                            return Outcome::ok (args);
                        } });
    }
}
