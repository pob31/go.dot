# This file is part of Go.dot — https://github.com/pob31/go.dot
#
# Copyright (C) 2026 Pierre-Olivier Boulant
#
# Go.dot is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version. Go.dot is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
# (LICENSE, at the repository root) for more details.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# =============================================================================
# spikes/ — the seven Tracktion Engine validation programs of PRD §6.1.
#
# These are THROWAWAY (devplan:19). They exist to answer a question about
# Tracktion Engine, each one ends in a written human pass/fail against its §6.1
# criterion, and none of them ever migrates into src/. Two rules below make
# that structural rather than aspirational — see the target_link_libraries()
# comment and the absence of add_test().
#
# The whole directory is behind WFG_BUILD_SPIKES, default OFF, and the root
# CMakeLists.txt adds it LAST. Only the `spikes` preset and the isolated
# `spikes` CI job turn it on, so a rotted throwaway reddens its own clearly
# labelled job and can never break the build of the product.
# =============================================================================

# -----------------------------------------------------------------------------
# NAMED BY PRD §6.1's NUMBER, not by the devplan's priority order.
#
# The devplan works these in the order #4, #2, then #1, #3, #5, #6, #7 — and
# that order lives in THIS COMMENT and nowhere else. Naming the targets in
# priority order would desynchronise them from §6.1 and from the
# docs/spikes/*.md filenames the moment the author reprioritises, and the
# spike reports are the deliverable.
#
# Each criterion below is PRD §6.1's own wording, so the pass/fail question a
# spike has to answer is legible from the build file.
# -----------------------------------------------------------------------------
set(WFG_SPIKES
    # 1. Launcher clip -> arbitrary multichannel bus routing at target channel counts.
    spike01_bus_routing

    # 2. Launcher start at an arbitrary in-file offset - load-to-time depends on
    #    it; if absent, this is the one genuine gap.  (devplan priority: 2nd)
    spike02_launch_offset

    # 3. Follow-action join quality: sample-accurate? crossfade at the boundary
    #    achievable without a custom clip?
    spike03_join_quality

    # 3b. Phase 3's sequel to #3, and not a criterion of the author's: of the
    #     clip's own loop wrap, a setLooping re-trigger and a placed cross-slot
    #     boundary, which join is cleanest. M12 of the Phase 3 plan, whose
    #     verdict fixes how a looping range is carried from pass to pass.
    spike03b_loop_joins

    # 4. Graph stability under sustained launching with a fixed track set - no
    #    rebuild, no crossfade tax on already-playing material.  (1st priority)
    spike04_graph_stability

    # 5. External parameter control at 50 Hz without disturbing the message thread.
    spike05_param_50hz

    # 6. Live-input latency through a Rack, with TE's PDC behaviour on live tracks
    #    understood (it may insert alignment delay exactly where none is wanted).
    spike06_rack_latency_pdc

    # 7. The proxy-plugin sandbox as a custom TE plugin type wrapping the IPC.
    spike07_proxy_plugin)

# PRD §6.1 also carries two UNNUMBERED "Also verify" items - TE transport
# chasing MTC, and multiple active Edits summed by the DeviceManager. They get
# no spike08/spike09 here. Inventing numbers for them would silently promote
# two loose ends into the author's numbered inventory and into the report
# filenames; whether they belong inside #4/#5 or deserve targets of their own
# is his call, and it is flagged in the README as an open question.

foreach(spike IN LISTS WFG_SPIKES)
    # Guarded by EXISTS because six of the seven are unwritten. A hard
    # add_executable on a missing source is a configure-time error, which would
    # make the `spikes` preset unusable until all seven exist - exactly backwards
    # for throwaway code that is meant to be written one at a time.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${spike}/main.cpp")
        add_executable(${spike} "${spike}/main.cpp")

        # wfg::thirdparty and wfg::warnings ONLY. NEVER wfg::engine.
        #
        # devplan:19 - "spikes never migrate into src/". A spike that CANNOT be
        # written against our engine API has nothing to migrate, so the rule is
        # enforced by the link line instead of by everyone remembering it. The
        # spikes talk to Tracktion Engine directly, which is the point: they are
        # asking questions about TE, not about Go.dot.
        target_link_libraries(${spike} PRIVATE wfg::thirdparty wfg::warnings)

        # NO add_test(), for any spike, ever. A spike's verdict is a paragraph a
        # human writes in docs/spikes/ after watching it run - "no rebuild, no
        # crossfade tax" is not an exit code. CI BUILDS them, which is what stops
        # them rotting silently against a TE bump, and does not RUN them, which
        # is what stops throwaway code from reddening a build.
    else()
        message(STATUS "wfg: spike ${spike} not written yet - skipping")
    endif()
endforeach()
