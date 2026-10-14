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

# WfgJucePatches.cmake — Go.dot's changes to JUCE, applied by the build to the
# submodule's working tree before a single JUCE source is compiled.
#
# WHY THERE ARE ANY. JUCE is pinned to the commit Tracktion Engine pins (README,
# "Dependency pins"), and stays there until Tracktion moves. When JUCE fixes a fault
# Go.dot cannot wait for, the fix is carried here, taken from JUCE's own history,
# instead of moving the pin ahead of Tracktion. The author's decision of 2026-10-14,
# for the first of them: a juce::String built from malformed UTF-8 could write past
# its allocation, and Go.dot builds one from every WebSocket message the OSCQuery
# server receives, from show files, from imported sets, and from text an OSC message
# puts on a text cue. JUCE fixed it in 8.0.16 and 9.0.4; Tracktion pins 8.0.13.
#
# A JUCE patch is a fix JUCE has already made, never a change of Go.dot's own, and
# each one goes when the pin moves to a JUCE that has it.
#
# How they are applied is WfgSubmodulePatches.cmake, shared with Tracktion's.
#
# Included from WfgThirdParty.cmake just before JUCE is added.

set(WFG_PATCHED "JUCE")
set(WFG_PATCHED_NAME "JUCE")
include(WfgSubmodulePatches)
