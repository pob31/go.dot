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

# WfgTracktionPatches.cmake — Go.dot's changes to Tracktion Engine, applied by the
# build to the submodule's working tree before a single Tracktion source is compiled.
#
# WHY THERE ARE ANY. A media cue's speed (namespace draft §22) needs two things
# Tracktion cannot do at its pin: resample an auto-tempo clip while a time-stretcher
# is compiled in, and change the speed of a clip that is playing in a launcher slot.
# Both are small changes inside Tracktion. The author's decision DQ (2026-09-28) is
# that they live here, in Go.dot's own repository beside the code that needs them,
# and that the build applies them - rather than in a fork of Tracktion. The price,
# accepted with the decision: `git status` reports ThirdParty/tracktion_engine as
# modified, and moving the pin takes the patches off first (README, "Bumping a pin").
#
# How they are applied is WfgSubmodulePatches.cmake, shared with JUCE's.
#
# Included from WfgThirdParty.cmake just before Tracktion's modules are added, so
# what CMake reads from them is the patched tree.

set(WFG_PATCHED "tracktion_engine")
set(WFG_PATCHED_NAME "Tracktion Engine")
include(WfgSubmodulePatches)
