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
# ON macOS THE FOLDER HOLDS Go.dot.app instead of a loose binary and a launcher:
# wfg, a small launcher as the bundle's main executable (packaging/macos), and
# everything the launcher reads under Contents/Resources. A bundle is what can be
# signed, notarized and stapled as one thing, and what Gatekeeper lets a tester
# double-click - an unsigned .command script beside a signed binary would still
# be refused. The signing itself is scripts/package-macos.sh's, after install:
# CMake lays the bundle out and never signs it.
#
# What it deliberately does NOT do: build an installer, or sign for Windows.

# EVERY RULE HERE IS IN THE COMPONENT `wfg`, and the workflow installs that
# component alone (`cmake --install ... --component wfg`). A plain install also
# runs JUCE's own rules, which lay the whole of include/JUCE-x.y.z/modules/ into
# the folder - some thirty megabytes of headers no tester wants. Those rules are
# vendor code and stay as they are; naming ours is the cheaper fix, and it keeps
# working whatever a JUCE or TE bump adds.
set(_wfg_packaging "${PROJECT_SOURCE_DIR}/packaging")

if(APPLE)
    set(_wfg_contents "Go.dot.app/Contents")
    set(_wfg_bin "${_wfg_contents}/MacOS")
    set(_wfg_res "${_wfg_contents}/Resources")

    # The launcher, built here because it is packaging and nothing else links
    # it. C, universal like wfg because CMAKE_OSX_ARCHITECTURES reaches every
    # target. Its OUTPUT_NAME is Info.plist's CFBundleExecutable.
    add_executable(wfg_macos_launcher "${_wfg_packaging}/macos/launcher.c")
    set_target_properties(wfg_macos_launcher PROPERTIES OUTPUT_NAME "Go.dot")
    install(TARGETS wfg_macos_launcher RUNTIME DESTINATION "${_wfg_bin}" COMPONENT wfg)

    configure_file("${_wfg_packaging}/macos/Info.plist.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/Go.dot-Info.plist" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/Go.dot-Info.plist"
            DESTINATION "${_wfg_contents}" RENAME Info.plist COMPONENT wfg)
    install(FILES "${_wfg_packaging}/macos/launch.sh" DESTINATION "${_wfg_res}" COMPONENT wfg)
    # Info.plist's CFBundleIconFile, and the .wfg page its document type names.
    # Windows carries its icons inside wfg.exe and Go.dot.exe; Linux's wait for
    # a desktop entry to name them.
    install(FILES "${_wfg_packaging}/icons/Go.dot.icns"
                  "${_wfg_packaging}/icons/Go.dot-document.icns"
            DESTINATION "${_wfg_res}" COMPONENT wfg)
    set(_wfg_res_prefix "${_wfg_res}/")
else()
    # The resource prefix is EMPTY here, not "./": CMake 3.31 warns (CMP0177)
    # on a destination such as ./console that is not already normalized, and
    # the policy cannot be set NEW on the 3.22 floor.
    set(_wfg_bin .)
    set(_wfg_res_prefix "")
endif()

install(TARGETS wfg RUNTIME DESTINATION "${_wfg_bin}" COMPONENT wfg)

# The web client, for `serve --ui=console` - the launchers pass it, so a tablet
# on the same network can reach http://<this machine>:<port>/ui beside the window.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/clients/console/" DESTINATION "${_wfg_res_prefix}console" COMPONENT wfg)

# Go.dot's ready-made Pd patches - go.avg, go.scale and the rest, each with its
# help patch - where serve looks for them: beside the binary, or in the
# bundle's Resources on macOS (namespace draft §51, ACU).
install(DIRECTORY "${PROJECT_SOURCE_DIR}/pd/" DESTINATION "${_wfg_res_prefix}pd" COMPONENT wfg)
# The device presets (namespace draft §57, AFN), beside the binary as the patches are.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/presets/" DESTINATION "${_wfg_res_prefix}presets" COMPONENT wfg)

# An empty show to open, because a launcher opens a bundle rather than asking.
# The window's New show and Save as are how a tester makes their own.
install(DIRECTORY "${_wfg_packaging}/Untitled/" DESTINATION "${_wfg_res_prefix}Untitled" COMPONENT wfg)

# Shows to open and learn from: so far, two process cues playing Go.dot's
# ready-made Pd patches (namespace draft §51, PC.9).
install(DIRECTORY "${_wfg_packaging}/Examples/" DESTINATION "${_wfg_res_prefix}Examples" COMPONENT wfg)

# Beside the app on macOS (the DMG's window), beside the binary elsewhere. The
# two Try it out guides are the author's for testers, in English and in French;
# README.txt points to them.
install(FILES
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${_wfg_packaging}/README.txt"
    "${PROJECT_SOURCE_DIR}/docs/Go.dot_TryItOut_English.md"
    "${PROJECT_SOURCE_DIR}/docs/Go.dot_TryItOut_Français.md"
    DESTINATION . COMPONENT wfg)

# One launcher per platform, and only that platform's. macOS's launcher is the
# app itself, above.
#
# WINDOWS: Go.dot.exe (packaging/windows/launcher.c), a window program that
# starts wfg.exe with no console and its output in a log - wfg.exe is a console
# program, and started from Explorer it would bring a black window with it. It
# carries the app icon, the .wfg page icon the installer's file type points at,
# and the name "Open with" shows. C, like the Mac's launcher, and WIN32 so the
# linker makes it a window program.
#
# LINUX: the shell script, as PROGRAMS rather than FILES so it arrives
# executable.
if(WIN32)
    set(WFG_ICON_APP "${_wfg_packaging}/icons/Go.dot.ico")
    set(WFG_ICON_DOCUMENT "${_wfg_packaging}/icons/Go.dot-document.ico")
    configure_file("${_wfg_packaging}/windows/launcher.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/Go.dot-launcher.rc" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/Go.dot-launcher.rc"
                                PROPERTIES OBJECT_DEPENDS "${WFG_ICON_APP};${WFG_ICON_DOCUMENT}")
    unset(WFG_ICON_APP)
    unset(WFG_ICON_DOCUMENT)

    add_executable(wfg_windows_launcher WIN32
        "${_wfg_packaging}/windows/launcher.c"
        "${CMAKE_CURRENT_BINARY_DIR}/Go.dot-launcher.rc")
    set_target_properties(wfg_windows_launcher PROPERTIES OUTPUT_NAME "Go.dot")
    target_link_libraries(wfg_windows_launcher PRIVATE shell32 user32)
    install(TARGETS wfg_windows_launcher RUNTIME DESTINATION . COMPONENT wfg)
elseif(NOT APPLE)
    install(PROGRAMS "${_wfg_packaging}/go.dot.sh" DESTINATION . COMPONENT wfg)

    # The app's and the .wfg page's PNGs, which `wfg associate` copies into the
    # desktop's icon theme (src/wfg/engine/app/Associate.h). Not the folder's
    # or the menu bar's: Linux has a use for neither.
    install(DIRECTORY "${_wfg_packaging}/icons/png/" DESTINATION icons COMPONENT wfg
            FILES_MATCHING PATTERN "go.dot-[0-9]*.png" PATTERN "go.dot-document-*.png")
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
unset(_wfg_contents)
unset(_wfg_bin)
unset(_wfg_res)
unset(_wfg_res_prefix)
