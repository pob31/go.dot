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
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <string>
#include <vector>

namespace wfg::cue
{
    void registerLaneCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                               doc::ShowDocument& document, LaneTable& lanes)
    {
        registry.add ({ "lane.arm",
                        "Arms a media cue's level lane to be recorded from a fader: the next"
                        " fader touched on any surface is taken for it. Empty frees the fader.",
                        { { "cue", 's', false } },
                        true,
                        [&document, &lanes] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            /*  A PASS IS A DECISION HALF MADE: the lane it ends in
                                is written when it stops, and moving the fader to
                                another cue under it would leave that half nowhere. */
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

                            lanes.arm (id);
                            return Outcome::ok (args);
                        } });

        registry.add ({ "lane.take",
                        "Takes a strip's fader for the lane that waits for one - what a surface"
                        " sends for the first fader touched while a lane is armed.",
                        { { "strip", 's', false } },
                        true,
                        [&document, &lanes] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (! lanes.waiting())
                                return Outcome::rejected (reason::notWaiting);

                            const auto id = args[0].getString();
                            const auto strip = document.findById (id);

                            if (! strip.isValid() || strip.getType().toString() != "Strip")
                                return Outcome::rejected (reason::unknownId);

                            /*  A PAD IS A GATE, NOT A FADER: it says pressed and
                                let go, and a lane is a level that moves. */
                            const auto surface = strip.getParent()[idProperty].toString().toStdString();

                            if (document.getAttribute ("/godot/surface/" + surface + "/profile").value_or ("")
                                  == "midiPads")
                                return Outcome::rejected (reason::badValue);

                            lanes.take (id);
                            return Outcome::ok (args);
                        } });

        registry.add ({ "lane.free",
                        "Lets the lane's fader go back to what it rode, and forgets the lane.",
                        {},
                        true,
                        [&lanes] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (lanes.recording)
                                return Outcome::rejected (reason::busy);

                            lanes.free();
                            return Outcome::ok (args);
                        } });

        /*  THE PASS STARTS BY FIRING THE CUE the way `cue.fire` does - not a GO,
            so the GO window (§21.2) never holds it back and the standby does not
            move - and AT THE SECOND ASKED, nought being the cue's own start
            (2026-10-05, namespace draft §30.4): a cue already sounding is
            moved there rather than recorded wherever it had got to. The
            Runner's `startLanePass` says how. The run's identifier is drawn
            here and written into the record, so a replay re-supplies it, as
            `cue.fire` does. */
        registry.add ({ "lane.record",
                        "Starts a pass on the lane that has a fader: its cue plays from the"
                        " second given, or from its own start with none, and the fader's level"
                        " is written from its first touch until the pass stops.",
                        { { "from", 'd', true }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, &lanes]
                        (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            if (! lanes.taken())
                                return Outcome::rejected (reason::noFader);

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

        /*  TWO VOICES, ONE VERB. With no argument it is a hand asking the pass
            to end - the window's stop, the D700's Rec - and only marks it: the
            Runner holds the samples and writes the lane on its next tick. With
            a word it is the Runner saying it has, which ends the pass; logged
            after the lane's own `node.set`, so a replay reads the ask, the lane
            and the end in the order they happened.

            AND THE END GIVES THE FADER BACK, however the pass ended (2026-10-05,
            QX, the author's decision) - so a pass never leaves a fader riding a
            lane nobody is recording. What it ended in is kept for the window to
            say (namespace draft §30.4): `kept` with the points the pass wrote
            and the seconds they span; `untouched` when nobody rode the fader
            while the cue sounded, and there is nothing to write; `locked` when
            the show was locked under the pass, which keeps the lane as it was;
            `dropped` when a kill took the pass. A `kept` with no numbers is a
            record from before they were carried, and is still a pass kept. */
        registry.add ({ "lane.stop",
                        "Ends the pass: with no argument, asks it to end; with kept, untouched,"
                        " locked or dropped, says the lane has been written (how many points, over"
                        " which seconds), that nobody rode the fader, that the lock kept the lane,"
                        " or that the pass was dropped - and gives the fader back.",
                        { { "how", 's', true }, { "points", 'i', true },
                          { "from", 'd', true }, { "to", 'd', true } },
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

                            lanes.endPass (said);
                            return Outcome::ok (args);
                        } });
    }
}
