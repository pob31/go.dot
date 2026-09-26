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

    MACHINE STATE, NEVER THE BUNDLE. The list is kept under the engine's own
    folder in the user's application data - which plugins a machine has is a
    fact about the machine, as its MIDI device identifiers are. A show names
    what it needs by identifier; the list says whether tonight's machine has it.

    IN A FILE OF GO.DOT'S OWN, `<engine>/plugins/known.xml` (2026-09-26), and
    no longer inside Tracktion's Settings.xml. That file is one Tracktion
    writes whole, two seconds after ANY of its keys changes - and a running
    serve changes several (the device setup, the wave devices) while holding
    the plugin list it read when it started. A scan run beside it would be
    written over by the next save of a key that has nothing to do with
    plugins. So the scan is the one writer of known.xml, by replacing the
    file whole, and everything else only reads it; a machine that scanned
    before the file existed has its list imported from Settings.xml once, on
    the first read.

    NAMES NO JUCE TYPE IN THIS HEADER. The scan stands a Tracktion engine up
    with no device to do the work; reading the list needs none.
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

        /** `VST3`, `AU` or `LV2`: the show's word for the format, which is
            JUCE's name for it except that JUCE calls an AU `AudioUnit`. */
        std::string format;
        std::string manufacturer;

        /** The file, on this machine. */
        std::string path;

        /*  The scan's whole description of it, as XML: what a child makes the
            plugin from. Empty when nobody asked the list for it. */
        std::string description;
    };

    /*  THE SHOW'S WORD FOR A FORMAT from JUCE's name for it: `AudioUnit`
        (and JUCE's `AudioUnit v3`-style variants) are `AU`; everything else
        is its own name. The schema allows `VST3 | AU | LV2`, so the word a
        known list publishes is the word `plugin.create` stores. */
    std::string formatWordOf (const std::string& juceFormatName);

    /** `<storageFolder>/plugins/known.xml`, the list's own file. */
    std::string knownListPath (const std::string& storageFolder);

    /*  If this process was launched as Tracktion's scan child, runs it to
        completion and answers true - the caller then exits 0 without reading a
        verb. False for an ordinary run. */
    bool runScanChildIfAsked (int argc, char** argv);

    /*  Scans this machine, out of process, and persists what it found where
        `knownPlugins` reads it. `formatWord` is empty for every format, or
        `vst3` / `au` / `lv2`; `extraFolder` is searched beside the format's default
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

    /*  WHERE A SCAN IS, for whoever launched it (2026-09-26): the app runs
        `wfg plugins --scan` as a child of its own and reads this file to say
        which plugin it is on. Written whole, by replacement, at the start,
        after every file and at the end - so a reader never meets half of one,
        and one that reads `scanning` after the child has gone knows the child
        died on `file`. */
    struct ScanProgress
    {
        /** `scanning`, `finished`, or `stopped` when the stop file was found. */
        std::string state;

        /** The show's word for the format of the file under scan: VST3, AU, LV2. */
        std::string format;

        /** The file under scan; empty once the scan is over. */
        std::string file;

        /** Files looked at, of `total`; what the machine knows so far. */
        int done = 0;
        int total = 0;
        int found = 0;

        /** Files this scan gave up on, as it gave up on them. */
        std::vector<std::string> skipped;

        std::string toJson() const;
        static bool fromJson (const std::string& text, ScanProgress& out);
    };

    /** The progress file read back; false when it is not there or not whole. */
    bool readScanProgress (const std::string& path, ScanProgress& out);

    /*  WHAT A SCAN LAUNCHED BY THE APP IS ASKED FOR, beyond the verb's own
        words: a file to report progress in, a file whose appearance stops
        the scan between two plugins, and one skipped file to try again - taken
        off the skip list and scanned alone. */
    struct ScanOptions
    {
        std::string formatWord;
        std::string extraFolder;
        bool retrySkipped = false;
        std::string retryFile;
        std::string progressFile;
        std::string stopFile;
    };

    std::vector<KnownPlugin> scanPlugins (const std::string& storageFolder, const ScanOptions& options,
                                          std::vector<std::string>& skipped, std::string& problem);

    /*  What the last scan found, from known.xml (imported from Settings.xml
        once when there is no file yet); empty when nothing was scanned. Each
        entry carries its description. Reads a file and nothing more - no
        engine, no format, no plugin loaded - so serve can call it at start
        and after every scan. */
    std::vector<KnownPlugin> knownPlugins (const std::string& storageFolder);

    /*  The scan's description of one identifier, as XML, for a child to
        make the plugin from; empty when the scan does not know it. */
    std::string describePlugin (const std::string& storageFolder, const std::string& identifier);

    /** The files earlier scans gave up on - hung past the deadline, or took
        the child down - which a scan skips until told to retry them. */
    std::vector<std::string> skippedPlugins (const std::string& storageFolder);

    /** The words `scanPlugins` takes for a format, in the order they are tried. */
    std::vector<std::string> formatWords();

    /*  LV2_PATH, SET ASIDE ON WINDOWS (found 2026-09-26). JUCE reads it with
        every ':' made a ';' - the Unix separator turned into Windows' - which
        on Windows cuts every absolute path at its drive letter; a path written
        the Windows way then reaches the LV2 library as an invalid URI and the
        process dies (an access violation, in the scan and in every engine
        start, since Tracktion makes the LV2 format when it starts). So this
        process forgets it before anything reads it, and so does every child it
        launches. Answers what it was, empty when it was not set or this is not
        Windows, so the caller can say it was set aside. An LV2 folder that is
        not a default one is scanned by name instead, and the scan remembers
        where it found each plugin. */
    std::string setAsideLv2PathOnWindows();
}
