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

# WfgInstall.cmake — what `cmake --install` lays down, which is exactly what a
# test build carries: .github/workflows/release.yml installs into a staging
# folder and archives that folder whole, so this file IS the archive's contents.
#
# ONE FLAT FOLDER, NOT bin/ share/ AND lib/. The archive is unpacked somewhere by
# a tester and run from there, and the launchers next to the binary find
# everything relative to themselves. A prefix layout would be the right answer
# for a distribution package, which this is not yet.
#
# What it deliberately does NOT do: sign, notarize, bundle a macOS .app, or build
# an installer. Those are a release concern that needs identities only the author
# can hold; this is a test build, and packaging/README.txt tells the tester so.

# EVERY RULE HERE IS IN THE COMPONENT `wfg`, and the workflow installs that
# component alone (`cmake --install ... --component wfg`). A plain install also
# runs JUCE's own rules, which lay the whole of include/JUCE-x.y.z/modules/ into
# the folder - some thirty megabytes of headers no tester wants. Those rules are
# vendor code and stay as they are; naming ours is the cheaper fix, and it keeps
# working whatever a JUCE or TE bump adds.
set(_wfg_packaging "${PROJECT_SOURCE_DIR}/packaging")

install(TARGETS wfg RUNTIME DESTINATION . COMPONENT wfg)

# The web client, for `serve --ui=console` - the launchers pass it, so a tablet
# on the same network can reach http://<this machine>:<port>/ui beside the window.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/clients/console/" DESTINATION console COMPONENT wfg)

# An empty show to open, because `wfg serve` opens a bundle and has no "new".
# The window's Save As is how a tester makes their own.
install(DIRECTORY "${_wfg_packaging}/Untitled/" DESTINATION Untitled COMPONENT wfg)

install(FILES
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${_wfg_packaging}/README.txt"
    DESTINATION . COMPONENT wfg)

# One launcher per platform, and only that platform's: a .cmd in a Linux tarball
# is noise a tester has to think about. PROGRAMS, not FILES, so the two shell
# scripts arrive executable - the .command is what Finder runs on a double-click.
if(WIN32)
    install(PROGRAMS "${_wfg_packaging}/Go.dot.cmd" DESTINATION . COMPONENT wfg)
elseif(APPLE)
    install(PROGRAMS "${_wfg_packaging}/Go.dot.command" DESTINATION . COMPONENT wfg)
else()
    install(PROGRAMS "${_wfg_packaging}/go.dot.sh" DESTINATION . COMPONENT wfg)
endif()

# THE MSVC RUNTIME, next to wfg.exe. The CRT is /MD on purpose (the root
# CMakeLists says why: Go.dot hosts plugins), so wfg.exe needs vcruntime140.dll
# and friends, and a tester's machine without the VC++ redistributable would say
# "VCRUNTIME140_1.dll was not found" and nothing else. App-local copies are what
# Microsoft's redistribution terms allow for exactly this case.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT wfg)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
endif()

unset(_wfg_packaging)
