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
    SPIKE 04 — GRAPH STABILITY.  THROWAWAY CODE (devplan:19).

    PRD §6.1 item 4, verbatim, and this is the pass criterion:

        4. **Graph stability under sustained launching with a fixed track set**
           — no rebuild, no crossfade tax on already-playing material.

    The devplan makes this the FIRST spike to run, because it is the one that
    validates the polyphony model of PRD §3.25. If the Tracktion graph rebuilds
    on every launch, or if already-playing material pays a crossfade whenever a
    new clip starts, the "Go.dot owns time, TE is the player" inversion needs
    amending in the PRD before Phase 2 is designed around it.

    NOT IMPLEMENTED. This file is the Phase 0 stub: it exists so the target is
    wired, the Tracktion link is proved, and the argument surface is fixed
    before anyone writes the measurement. The verdict, when there is one, is a
    paragraph a human writes in docs/spikes/spike04-graph-stability.md after
    watching this run — not an exit code, which is why nothing in the build
    system ever calls add_test() on a spike.

    WHY THE ARGUMENTS ARE MANDATORY AND HAVE NO DEFAULTS
    ---------------------------------------------------
    --tracks, --sample-rate and --buffer are REQUIRED, and there is no fallback
    value for any of them anywhere in this file. The default fixed track count
    and the target sample rates / buffer sizes are two of the author's open
    Phase 0 decisions (devplan:49-50). A "sensible default" written here would
    be an ANSWER to a question he has not answered, and it would then get read
    back out of this file as though it were one.

    argv is the only place a number like this is allowed to live in Go.dot right
    now, and it is only allowed here because this program is throwaway and never
    migrates into src/. Do not helpfully add defaults.

    When this spike IS written, it should also settle PRD §6.1's second
    unnumbered "Also verify" item if it fits naturally — multiple active Edits
    summed by the DeviceManager — or say in the report why it does not. That is
    an open question, not a decision to take here.
*/

#include <tracktion_engine/tracktion_engine.h>

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>

namespace
{
    /*  Deliberately hand-rolled and about fifteen lines: a spike must not grow
        an argument-parsing dependency, and juce::ArgumentList lives on the far
        side of a boundary the spikes are not allowed to cross.
    */
    std::optional<long> valueFor (int argc, char** argv, std::string_view flag)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg { argv[i] };

            if (arg.size() > flag.size() && arg.compare (0, flag.size(), flag) == 0)
            {
                const auto* first = arg.data() + flag.size();
                const auto* last  = arg.data() + arg.size();

                long parsed = 0;

                if (std::from_chars (first, last, parsed).ec == std::errc{} && parsed > 0)
                    return parsed;

                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    int usage()
    {
        std::cerr <<
            "spike04_graph_stability - PRD 6.1 #4, graph stability under sustained launching\n"
            "\n"
            "usage: spike04_graph_stability --tracks=N --sample-rate=N --buffer=N\n"
            "\n"
            "All three are REQUIRED and have no defaults. The fixed track count and the\n"
            "target sample rates / buffer sizes are open author decisions (devplan:49-50);\n"
            "this spike takes them on the command line so that no value for either is\n"
            "recorded anywhere in the source tree.\n"
            "\n"
            "Pass criterion (PRD 6.1 #4): sustained launching against a FIXED track set\n"
            "causes no graph rebuild and imposes no crossfade tax on already-playing\n"
            "material. The verdict is written by a human into\n"
            "docs/spikes/spike04-graph-stability.md.\n";

        return 2;
    }
}

//==============================================================================
int main (int argc, char** argv)
{
    const auto tracks     = valueFor (argc, argv, "--tracks=");
    const auto sampleRate = valueFor (argc, argv, "--sample-rate=");
    const auto bufferSize = valueFor (argc, argv, "--buffer=");

    if (! tracks || ! sampleRate || ! bufferSize)
        return usage();

    /*  The Tracktion link proof, and the only vendor call this stub makes.
        Engine::getVersion() is an out-of-line static (tracktion_Engine.cpp:120),
        so it forces the linker to pull TE's compiled code in and the target
        stops being a stub that merely names a header.

        No Engine is CONSTRUCTED here. Constructing one is exactly what this
        spike will do when it is implemented - it builds fifteen subsystems and
        may open audio devices, which is fine for a program a human runs by hand
        and wrong for one CI builds unattended.
    */
    std::cout << "spike04_graph_stability\n"
              << "tracktion: " << tracktion::engine::Engine::getVersion().toStdString() << "\n"
              << "tracks: "      << *tracks     << "\n"
              << "sample rate: " << *sampleRate << " Hz\n"
              << "buffer: "      << *bufferSize << " frames\n"
              << "SPIKE NOT RUN - implementation pending; verdict goes in docs/spikes/spike04-graph-stability.md"
              << std::endl;

    // Non-zero on purpose. This program has not answered its question, and a
    // spike that exits 0 without measuring anything is worse than one that
    // fails: someone would eventually read the 0 as a pass.
    return 1;
}
