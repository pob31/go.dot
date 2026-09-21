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

#include <wfg/engine/audio/AudioCommands.h>
#include <cmath>

namespace wfg::audio
{
    void stopOutputTest (AudioState& state)
    {
        state.test.type = 0; state.test.channel = -1; state.test.hold = false;
        if (state.sendTest) state.sendTest (state.test);
    }

    void registerAudioCommands (CommandRegistry& registry, AudioState& state)
    {
        registry.add ({ "audio.testSignal", "Test one hardware output without changing cue routing.",
            { { "type", 'i', false }, { "channel", 'i', false }, { "frequency", 'i', false },
              { "level", 'd', false }, { "hold", 'T', false } }, false,
            [&state] (CommandContext&, const std::vector<osc::Value>& args)
            {
                OutputTestSettings next { args[0].getInt32(), args[1].getInt32(), args[2].getInt32(),
                                          args[3].getFloat64(), args[4].getBool() };
                if (next.type < 0 || next.type > 4 || next.channel < -1 || next.channel >= maximumPatchChannels
                    || next.frequency < 20 || next.frequency > 20000 || ! std::isfinite (next.level)
                    || next.level < -92.0 || next.level > 0.0)
                    return Outcome::rejected (reason::typeMismatch);
                state.test = next;
                if (state.sendTest) state.sendTest (state.test);
                return Outcome::ok (args);
            } });
        registry.add ({ "audio.testStop", "Stop and clear the output test signal.", {}, false,
            [&state] (CommandContext&, const std::vector<osc::Value>& args)
            { stopOutputTest (state); return Outcome::ok (args); } });
        //----------------------------------------------------------------------
        registry.add ({ "audio.editBuilt",
                        "The playback graph exists, and this is its shape. Reported by the"
                        " audio side once, when the show loads.",
                        { { "device", 's', false },
                          { "tracks", 'i', false },
                          { "outputs", 'i', false },
                          { "nodes", 'i', false } },
                        false,
                        [&state] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto tracks = args[1].getInt32();
                            const auto outputs = args[2].getInt32();
                            const auto nodes = args[3].getInt32();

                            /*  Negative counts are refused rather than clamped.
                                Zero tracks is a real answer - a show with no
                                audio - but a graph with minus four nodes is a
                                message that got mangled somewhere, and quietly
                                storing 0 would put a number nobody measured in
                                front of every client reading the tree. */
                            if (tracks < 0 || outputs < 0 || nodes < 0)
                                return Outcome::rejected (reason::typeMismatch);

                            state.device = args[0].getString();
                            state.tracks = tracks;
                            state.outputs = outputs;
                            state.nodes = nodes;

                            /*  A graph exists, so the audio side is running.
                                There is no `audio.editBuilt` that leaves it
                                stopped: the transition IS the graph coming into
                                being, and PR 2.7's device open reports its own. */
                            state.status = "running";

                            return Outcome::ok (args);
                        } });
    }
}
