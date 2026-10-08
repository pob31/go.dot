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

#include <wfg/engine/cue/CurveCommands.h>

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CurveTable.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <string>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        /*  WHETHER A CURVE BELONGS TO A CUE: directly under it, or under one of
            its messages. */
        bool curveOfCue (const doc::ShowDocument& document, const std::string& curveId, const std::string& cueId)
        {
            const auto curve = document.findById (curveId);

            if (! curve.isValid() || ! curve.hasType ("Curve"))
                return false;

            auto parent = curve.getParent();

            if (parent.hasType ("Message"))
                parent = parent.getParent();

            return parent.isValid() && parent.getProperty (juce::Identifier ("id")).toString().toStdString() == cueId;
        }
    }

    void registerCurveCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                                doc::ShowDocument& document, CurveTable& curves)
    {
        registry.add ({ "curve.arm",
                        "Arms an OSC cue for recording its curves: its curves can then be armed and a pass"
                        " recorded. Empty frees it.",
                        { { "cue", 's', false } },
                        true,
                        [&document, &curves] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            //  A pass is a decision half made; the cue stays until it ends.
                            if (curves.recording)
                                return Outcome::rejected (reason::busy);

                            if (id.empty())
                            {
                                curves.free();
                                return Outcome::ok (args);
                            }

                            const auto cue = document.findById (id);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            if (! cue.hasType ("Osc"))
                                return Outcome::rejected (reason::badValue);

                            /*  A CURVE IS THE SHOW'S, and the lock keeps the show:
                                a pass would end in a write the lock refuses. */
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            curves.arm (id);
                            return Outcome::ok (args);
                        } });

        registry.add ({ "curve.free",
                        "Frees the OSC cue armed for recording, and every curve armed on it.",
                        {},
                        true,
                        [&curves] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (curves.recording)
                                return Outcome::rejected (reason::busy);

                            curves.free();
                            return Outcome::ok (args);
                        } });

        registry.add ({ "curve.rec",
                        "Arms one curve of the armed OSC cue, or disarms it - before a pass, or during one.",
                        { { "curve", 's', false }, { "on", 'T', false } },
                        true,
                        [&document, &curves] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            if (! curves.armed())
                                return Outcome::rejected (reason::notArmed);

                            const auto id = args[0].getString();

                            if (! curveOfCue (document, id, curves.cue()))
                                return Outcome::rejected (reason::unknownId);

                            curves.setArmed (id, args[1].getBool());
                            return Outcome::ok (args);
                        } });

        registry.add ({ "curve.record",
                        "Starts a pass on the OSC cue armed for recording: it plays from the second given,"
                        " or from its start, and every armed curve is written from the first value heard or"
                        " ridden for it until the pass stops.",
                        { { "from", 'd', true }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, &curves] (CommandContext& context,
                                                                const std::vector<osc::Value>& args)
                        {
                            if (! curves.armed())
                                return Outcome::rejected (reason::notArmed);

                            if (curves.recording)
                                return Outcome::rejected (reason::busy);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            if (! document.findById (curves.cue()).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto from = ! args.empty() ? args[0].getFloat64() : 0.0;
                            const auto supplied = args.size() > 1 ? args[1].getString() : std::string {};

                            /*  ARMED BEFORE THE FIRE, so the cue it fires plays
                                its clock open-ended for the pass even with no
                                curve drawn yet. */
                            curves.startPass ({});
                            curves.startTick = context.tick;
                            const auto runId = runner.startCurvePass (engine, context.tick, curves.cue(), from, supplied);

                            if (runId.empty())
                            {
                                curves.endPass ({});
                                return Outcome::rejected (reason::badValue);
                            }

                            curves.run = runId;
                            return Outcome::ok ({ osc::Value::float64 (from), osc::Value::string (runId) });
                        } });

        registry.add ({ "curve.stop",
                        "Ends the pass: with no argument, asks it to end; with kept, untouched, locked or"
                        " dropped, says the curves have been written, that nothing moved an armed one, that"
                        " the lock kept them, or that the pass was dropped.",
                        { { "how", 's', true }, { "points", 'i', true }, { "curves", 's', true } },
                        true,
                        [&curves] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            if (! curves.recording)
                                return Outcome::rejected (reason::notRunning);

                            if (args.empty() || args[0].getString().empty())
                            {
                                curves.stopping = true;
                                return Outcome::ok (args);
                            }

                            const auto how = args[0].getString();

                            if (how != "kept" && how != "untouched" && how != "locked" && how != "dropped")
                                return Outcome::rejected (reason::badValue);

                            auto said = std::to_string (context.tick) + " " + curves.cue() + " " + how;

                            if (how == "kept" && args.size() >= 2)
                                said += " " + std::to_string (args[1].getInt32());

                            if (how == "kept" && args.size() >= 3)
                                said += " " + args[2].getString();

                            curves.endPass (said);
                            return Outcome::ok (args);
                        } });

        /*  A HAND'S VALUE FOR CURVES OF THE ARMED CUE, pairs of curve and value:
            the SpaceMouse's (O.11), one record a tick. Latched like a report,
            and SENT - the device follows the hand (ZJ). */
        registry.add ({ "curve.ride",
                        "A hand's value for armed curves of the cue armed for recording, as curve-value pairs.",
                        { { "curve", 's', false }, { "value", 'd', false }, { "more", '*', true, true } },
                        true,
                        [&curves] (CommandContext& context, const std::vector<osc::Value>& args)
                        {
                            if (! curves.armed())
                                return Outcome::rejected (reason::notArmed);

                            if (args.size() % 2 != 0)
                                return Outcome::rejected (reason::arity);

                            for (std::size_t at = 0; at + 1 < args.size(); at += 2)
                            {
                                if (! args[at].isString() || ! args[at + 1].isNumber())
                                    return Outcome::rejected (reason::typeMismatch);

                                auto& ride = curves.rideOf (args[at].getString());
                                ride.value = args[at + 1].asDouble();
                                ride.source = "hand";
                                ride.lastTick = context.tick;
                            }

                            return Outcome::ok (args);
                        } });
    }
}
