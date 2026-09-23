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
    WHICH PLUGINS THIS MACHINE HAS, and how it finds out (Phase 9a, §17.7).

    SCANNING IS OUT OF PROCESS, ALWAYS - PRD §3.18: that is where most plugin
    crashes happen, and a rescan taking the app down during a get-in is the
    classic failure. Tracktion's own machinery does it: its custom scanner
    launches this same binary as a child with a pipe, asks it file by file, and
    survives the child dying on one of them. The child is dispatched at the top
    of `runConsole` before any verb is read, which is what `runScanChildIfAsked`
    is for.

    MACHINE STATE, NEVER THE BUNDLE. The list is kept where Tracktion keeps its
    settings, under the engine's own folder in the user's application data -
    which plugins a machine has is a fact about the machine, as its MIDI device
    identifiers are. A show names what it needs by identifier; the list says
    whether tonight's machine has it.

    NAMES NO JUCE TYPE IN THIS HEADER. The .cpp stands a Tracktion engine up
    with no device, on the shared storage, to do the work.
*/

#include <string>
#include <vector>

namespace wfg::plugin
{
    struct KnownPlugin
    {
        std::string name;

        /** JUCE's identifier string: what a show's `plugin/identifier` holds
            and what a load resolves by. */
        std::string identifier;

        /** `VST3`, `AudioUnit`, … as the format names itself. */
        std::string format;
        std::string manufacturer;

        /** The file, on this machine. */
        std::string path;
    };

    /*  If this process was launched as Tracktion's scan child, runs it to
        completion and answers true - the caller then exits 0 without reading a
        verb. False for an ordinary run. */
    bool runScanChildIfAsked (int argc, char** argv);

    /*  Scans this machine, out of process, and persists what it found where
        `knownPlugins` reads it. `formatWord` is empty for every format, or
        `vst3` / `au`; `extraFolder` is searched beside the format's default
        places when given. Answers what is known afterwards; `problem` says
        why nothing could be done. Blocks for as long as the scan takes, on
        the calling thread, which runs a message loop meanwhile.

        A FILE THAT DOES NOT ANSWER IN `perFileSeconds` IS SKIPPED, named in
        `skipped`, and remembered: a plugin that puts a licence window up in
        the child, or waits on a server, would otherwise hold the scan for
        ever - Tracktion's coordinator waits without a deadline - and a get-in
        scan that never ends is the failure PRD §3.18 sends the scan out of
        process to avoid. The child is killed and relaunched for the next file.
        Skipped files stay skipped at the next scan until `retrySkipped` asks
        for them again. */
    constexpr int perFileSeconds = 30;

    std::vector<KnownPlugin> scanPlugins (const std::string& storageFolder,
                                          const std::string& formatWord,
                                          const std::string& extraFolder,
                                          bool retrySkipped,
                                          std::vector<std::string>& skipped,
                                          std::string& problem);

    /** What the last scan found, from storage; empty when nothing was scanned. */
    std::vector<KnownPlugin> knownPlugins (const std::string& storageFolder);

    /** The files earlier scans gave up on - hung past the deadline, or took
        the child down - which a scan skips until told to retry them. */
    std::vector<std::string> skippedPlugins (const std::string& storageFolder);

    /** The words `scanPlugins` takes for a format, in the order they are tried. */
    std::vector<std::string> formatWords();
}
