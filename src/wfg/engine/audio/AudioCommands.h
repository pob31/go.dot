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

#pragma once

/*
    What the audio side reports about itself, as commands like any other.

    THE RULE THIS EXISTS TO KEEP. State transitions are events; continuous
    readouts are not. Everything the tick thread learns from the audio side that
    a decision depends on - the Edit was built, a run started, a device opened -
    enters the model as a LOGGED command applied on the tick it was observed.
    Position and level are readouts and are never model inputs.

    WHY A COMMAND AND NOT A METHOD CALL. PRD §3.15 admits no second path into
    engine state, and §4.11 says every gesture-reachable action exists as a
    named command - the machine's own reports are held to the same rule, because
    the alternative is a model whose contents depend on when a message thread
    happened to run. Going through the registry means the transition lands on a
    tick, in arrival order, with its arguments in the log; and replay re-injects
    it at the same tick with no audio present at all, which is why a log of a
    real session reproduces the same tree on a machine with no sound card.

    WHO MAY SEND IT. Anyone, and that is deliberate rather than an oversight.
    Phase 1 has no permissions, and a command that could only be sent from
    inside the process would be a command replay could not send - which would
    make the one guarantee this exists to provide impossible to test.

    IDEMPOTENT ON PURPOSE. The handler stores what it was told and nothing else.
    Applying it twice leaves the same state as applying it once, so a log
    replayed twice, or replayed after a session that already ran, converges.
*/

#include <wfg/engine/command/CommandRegistry.h>

#include <string>

namespace wfg::audio
{
    /*  What the audio side is actually doing, as against what the document
        decided (PRD §4.10). The tick thread owns this: the handlers below write
        it, and ParameterTree reads the nodes under `/godot/audio` from it on
        the same thread.

        It is not AudioHost. AudioHost is a Tracktion engine and a graph; this
        is four values a client can read, which a replay with no engine at all
        must be able to produce.
    */
    struct AudioState
    {
        /** The device in use, or empty when none is open. `hosted` has no card. */
        std::string device;

        /** The polyphony ceiling the Edit was actually built with. */
        int tracks = 0;

        /** How many hardware output channels the graph fills. */
        int outputs = 0;

        /** How many nodes the playback graph turned out to have. */
        int nodes = 0;

        /** `stopped` until something opens a device or a hosted driver. */
        std::string status = "stopped";
    };

    /*  Adds `audio.editBuilt`, bound to `state`.

        `audio.editBuilt <device:s> <tracks:i> <outputs:i> <nodes:i>` - the
        Edit exists and this is its shape. Submitted once, at show load, by
        whoever built it; PRD §3.25 fixes the graph there and nothing after
        changes it, so in a whole show this happens exactly once.

        The node count rides along because it is the one number that says the
        graph is the size it should be. A show that reported four tracks and
        nine nodes built four tracks that are not in the graph.
    */
    void registerAudioCommands (CommandRegistry& registry, AudioState& state);
}
