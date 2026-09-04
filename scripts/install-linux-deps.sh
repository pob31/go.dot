#!/usr/bin/env bash
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
#
# ---------------------------------------------------------------------------
# Produces: nothing. Installs the Debian/Ubuntu packages a Go.dot build needs.
# Usage:    bash scripts/install-linux-deps.sh
# Build requirements: a Debian-family distro with apt-get, and sudo (or root).
#
# This file IS the apt list. CI runs this exact script rather than an inline
# `apt-get install` in ci.yml, which is the only arrangement in which the two
# cannot drift: the moment someone installs a package by hand to unblock a local
# build and does not add it here, CI reddens.
#
# The X11 / freetype / fontconfig packages are needed even though Phase 0 ships
# no UI at all: juce_audio_processors -> juce_gui_extra -> juce_gui_basics ->
# juce_graphics is an unbreakable dependency chain in JUCE 8, and
# tracktion_engine requires juce_audio_processors. A "headless" Tracktion binary
# is still GUI-linked. Nothing here opens a display at runtime.
#
# DELIBERATELY ABSENT, with the flag that keeps each one off (all set once, in
# cmake/WfgThirdParty.cmake, on the wfg_deps target):
#   libcurl4-openssl-dev   JUCE_USE_CURL=0
#   libwebkit2gtk-4.1-dev  JUCE_WEB_BROWSER=0   (revisit at Phase 11, PRD 3.23)
#   libjack-jackd2-dev     JUCE_JACK=0          (a Phase-2 Linux audio-backend decision)
#   ladspa-sdk             JUCE_PLUGINHOST_LADSPA=0
#   libglu1-mesa-dev /     juce_opengl, which we do not link
#     mesa-common-dev
#   xvfb                   ScopedJuceInitialiser_GUI only calls
#                          MessageManager::getInstance() and opens no X display
#                          (verified against juce_MessageManager.cpp). If a
#                          display error ever appears in ctest, add `xvfb-run -a`
#                          in front of the ctest step in ci.yml and note the date
#                          on this line.
#
# THE TRAP, because "configure succeeded" does not mean these are installed:
# JUCE's pkg_check_modules() calls are NOT marked REQUIRED
# (JUCEModuleSupport.cmake:392). A missing libfreetype-dev therefore gives a
# perfectly SUCCESSFUL configure, and then
# "ft2build.h: No such file or directory" halfway through compiling
# juce_graphics. If you get a missing-header error from a JUCE module, come back
# here before you suspect the build system.
# ---------------------------------------------------------------------------

set -euo pipefail

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "error: not root and sudo is not installed" >&2
        exit 1
    fi
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "error: this script is Debian/Ubuntu only (no apt-get found)." >&2
    echo "       On Fedora/Arch/openSUSE, install the equivalents of the list" >&2
    echo "       inside this file: ALSA, freetype, fontconfig and the X11" >&2
    echo "       development headers, plus cmake, ninja, ccache and pkg-config." >&2
    exit 1
fi

echo "==> apt-get update"
$SUDO apt-get update

echo "==> Installing Go.dot build dependencies"
$SUDO apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    ninja-build \
    ccache \
    locales \
    libasound2-dev \
    libfreetype-dev \
    libfontconfig1-dev \
    libx11-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxext-dev \
    libxinerama-dev \
    libxrandr-dev \
    libxrender-dev

# Per-package reasons, kept next to the list rather than inline so the list
# stays copy-pasteable into a terminal:
#
#   build-essential      the compiler. GCC 11 is the floor (Tracktion Engine's
#                        C++20 requirement); cmake/WfgGuards.cmake enforces it.
#   pkg-config           JUCEModuleSupport.cmake:391 does
#                        find_package(PkgConfig REQUIRED) — configure fails
#                        outright without it.
#   ninja-build          the generator the `dev` / `ci-linux` presets use.
#   ccache               the ci-linux preset sets it as the compiler launcher.
#   locales              provides locale-gen, needed for `locale-gen fr_FR.UTF-8`.
#                        Every serialisation test runs under fr_FR as well as C
#                        (devplan, cross-cutting), and tests/TestMain.cpp exits 2
#                        rather than reporting green if the locale is absent.
#   libasound2-dev       juce_audio_devices (ALSA)
#   libfreetype-dev      juce_graphics
#   libfontconfig1-dev   juce_graphics
#   libx11-dev           juce_gui_basics
#   libxcomposite-dev    juce_gui_basics
#   libxcursor-dev       juce_gui_basics
#   libxext-dev          juce_gui_basics
#   libxinerama-dev      juce_gui_basics
#   libxrandr-dev        juce_gui_basics
#   libxrender-dev       juce_gui_basics
#
# cmake itself is NOT in the list: the runner images and most desktops ship one,
# and the version we need (3.22+) is older than every supported distro's. If
# yours is older, install it from Kitware's apt repo rather than adding it here
# — the distro cmake is often too old and apt will not tell you so.

echo
echo "Dependencies installed."
echo "Next:  cmake --preset dev  &&  cmake --build --preset dev-debug"
