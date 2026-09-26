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
    A media cue's inserts, as the window reads them (Phase 9a, PR 9a.9), and
    the show's plugin set with the machine's known list for the Plugins tab.

    A STRIP PER ENTRY OF THE SET AND NOT PER FX, which is Sends.h's shape and
    for its reason: every voice carries the whole set (decision AE), so a cue
    that switches one entry in has one Fx child and says nothing about the
    rest - but a panel that drew one row would be a panel you cannot switch a
    second entry in on. The strips come from the set, in `plugins/order`, which
    is the chain's order on every voice; the document says which of them this
    cue switches in, and switching one in that has no Fx is what creates it.

    EVERYTHING A ROW DRAWS IS READ, NEVER WORKED OUT: the entry's name and its
    state word are the engine's; a parameter's name, unit, steps and bipolar
    flag are the catalogue's, published under the entry; its value and the
    plugin's own text for it are the cue's `p<n>` and `t<n>` nodes - and where
    the cue has no Fx yet, the value shown is the catalogue's default and the
    text is empty, because nothing has been said.

    std only, one snapshot door as every model file.
*/

#include <wfg/engine/tree/TreeSnapshot.h>

#include <map>
#include <string>
#include <vector>

namespace wfg::client::model
{
    struct FxParameter
    {
        int index = 0;
        std::string name;
        std::string shortName;
        std::string unit;

        /** Normalised 0..1, as the plugin takes it. */
        double value = 0.0;
        double defaultValue = 0.0;

        /** The plugin's own text for the value; empty where the cue has no Fx. */
        std::string text;

        bool discrete = false;
        std::vector<std::string> steps;
        bool bipolar = false;
    };

    struct FxStrip
    {
        /** The set entry: its id, name, place in the chain, and what became of it tonight. */
        std::string pluginId;
        std::string name;
        int index = 0;
        std::string state;
        std::string problem;

        /*  How late a cue is while this entry is in its signal, in samples:
            the plugin's own latency, uncompensated (PRD §3.25). Nought for
            one that adds none, and for one that has not loaded. */
        int latencySamples = 0;

        /** The cue's Fx for it, when there is one; empty until the first switch-in. */
        std::string fxId;
        bool enabled = false;

        /*  Why it plays THIS cue dry, switched in (2026-09-26): the cue wider
            than it takes, or it would give the cue back narrower. The Fx's own
            `problem` row; empty when it takes the cue. */
        std::string dryWhy;

        /** What it takes, in words - "mono in, stereo out" (2026-09-26). */
        std::string layout;

        std::vector<FxParameter> params;

        bool present() const noexcept { return ! fxId.empty(); }
    };

    struct FxReading
    {
        bool present = false;
        std::vector<FxStrip> strips;
        std::string notice;

        /*  THE CUE THROUGH ITS INSERTS (2026-09-26): its file's width, how
            wide it comes out, and how many samples late - read off the cue's
            `channels`, `chainChannels` and `insertLatency` rows. */
        int fileChannels = 0;
        int chainChannels = 0;
        int insertLatency = 0;
        int sampleRate = 0;
    };

    /*  "Plays as stereo through its inserts, 21 ms late." - or nothing, for a
        cue its inserts neither widen nor delay. */
    std::string chainWords (const FxReading&);

    /*  The cue's strips against the set. `present` is false, with a sentence,
        for a cue that is not media; strips are empty, with a sentence, for a
        show that declares no set. */
    FxReading readFx (const tree::TreeSnapshot&, const std::string& cueId);

    /*  WHAT BECAME OF AN ENTRY TONIGHT, in words, for its box in the chain:
        the engine's state word, its sentence, and - only when this cue has
        the entry in - what that means for the sound. A plugin that is not
        there is not a problem for a cue that does not use it. */
    std::string stateSentence (const FxStrip&);

    /** "64 samples late while it is in", or nothing for an entry that adds none. */
    std::string latencyWords (const FxStrip&);

    /** One insert the cue holds: its Fx, and whether it is in the signal. */
    struct HeldInsert
    {
        std::string fxId;
        bool enabled = true;
    };

    /** The cue's inserts, by the set entry each is for. */
    std::map<std::string, HeldInsert> insertsOf (const tree::TreeSnapshot&, const std::string& cueId);

    /** `/godot/fx/<id>/<leaf>`. */
    std::string fxAddress (const std::string& fxId, const std::string& leaf);

    /** `/godot/fx/<id>/p<n>`. */
    std::string fxParameterAddress (const std::string& fxId, int index);

    //==============================================================================
    /** One entry of the show's set, for the Plugins tab. */
    struct PluginRow
    {
        std::string id;
        std::string name;
        std::string identifier;
        std::string format;
        std::string path;
        std::string preset;
        std::string state;
        std::string problem;
        int latencySamples = 0;
        int paramCount = 0;

        /** What it takes, in words - "stereo in, stereo out" (2026-09-26). */
        std::string layout;
    };

    /** The set, in chain order. */
    std::vector<PluginRow> readPluginSet (const tree::TreeSnapshot&);

    /** One plugin this machine's scan found. */
    struct KnownPluginRow
    {
        std::string name;
        std::string identifier;
        std::string format;
        std::string manufacturer;
        std::string path;
    };

    /** What the last scan found, as the tree lists it; empty when none was run. */
    std::vector<KnownPluginRow> readKnownPlugins (const tree::TreeSnapshot&);

    /*  WHERE THE APP'S PLUGIN SCAN IS (2026-09-26, the author's decision: a
        Scan button in Show settings, Plugins), read off /godot/plugin/scan. */
    struct ScanRow
    {
        /** idle, scanning, finished, failed. */
        std::string state = "idle";
        std::string format;
        std::string file;
        int done = 0;
        int total = 0;
        int found = 0;
        int skipped = 0;
        std::string problem;
    };

    ScanRow readScan (const tree::TreeSnapshot&);

    /** The files a scan gave up on - the ones Retry names. */
    std::vector<std::string> readSkippedPlugins (const tree::TreeSnapshot&);

    /** Whether the show's set differs from the audio graph: what Load now is for. */
    bool readSetChanged (const tree::TreeSnapshot&);

    /*  What the Plugins tab says under the machine's list, in words: the
        scan's progress while it runs, why it failed when it did, how many
        plugins the machine knows otherwise - and what to press when it knows
        none. */
    std::string scanWords (const ScanRow& scan, std::size_t knownCount);
}
