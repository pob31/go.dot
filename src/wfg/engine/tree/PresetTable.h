// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/tree/Mount.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree
{
    /*  A DEVICE PRESET (namespace draft §57, AFG, AFN): a namespace file
        shipped with Go.dot - the same OSCQuery JSON a show keeps as
        `namespaces/<id>.json` - describing one device family in one of its
        protocols, so that a desk, a lighting console or a spatial processor
        is made in the Network tab from a menu rather than typed in. The root's
        `GODOT` key names it; the rest is the description `readNamespace` reads
        for any device.

        It is read ONCE, at start, from the folder beside the binary, and never
        stored: a device made from it copies the file into the show
        (`mount.createFromPreset`), so a show opens on a machine whose Go.dot
        has never seen the preset. What a preset cannot be used for is said in
        `problem`, in a sentence, rather than by leaving it out of the list. */
    struct Preset
    {
        std::string slug;                 ///< the file's name without `.json`, equal to GODOT.PRESET
        std::string vendor;               ///< GODOT.VENDOR: Yamaha, DiGiCo, d&b audiotechnik
        std::string model;                ///< GODOT.MODEL: the family and its protocol, "DS100, DS100M (OSC)"
        int version = 0;                  ///< GODOT.VERSION, a whole number from 1
        std::string transport = "udp";    ///< GODOT.TRANSPORT: udp, tcp or midi
        std::string wire = "osc";         ///< GODOT.WIRE: osc, rcp, line or midi
        std::string framing = "length";   ///< GODOT.FRAMING, OSC over TCP only: length or slip
        std::string readback;             ///< GODOT.READBACK (DP.10): how the device is heard back, empty for none
        int port = 0;                     ///< GODOT.PORT, the device's usual one; nought when it has none
        std::vector<std::string> roots;   ///< what a device made from it answers at (`rootsOfNamespace`)
        std::vector<std::string> sources; ///< GODOT.SOURCES: the documents it was written from, one line each
        std::string generated;            ///< GODOT.GENERATED: the script that writes it, or empty
        std::string verified;             ///< GODOT.VERIFIED: when and on what it was seen to work, or empty
        int nodeCount = 0;                ///< how many nodes the description has
        std::string problem;              ///< why it cannot be used, or empty
        std::string text;                 ///< the file's bytes, what a device made from it keeps

        /*  WHOSE IT IS (namespace draft §76, ART): `shipped` beside the binary,
            or `yours` from the user's own folder - which is used in place of a
            shipped preset of the same name, `shadows` saying the version it
            hides. */
        std::string origin = "shipped";
        std::string path;                 ///< the file it was read from, absolute
        int shadows = 0;                  ///< the shipped VERSION this one hides; nought for none

        bool usable() const noexcept { return problem.empty(); }

        /** The roots as a prefix row: space-separated, the way `mount/prefix` spells several. */
        std::string rootRow() const;
    };

    /*  Reads one preset from its text. Pure: no filesystem, no engine. The slug
        is the file's name, and a GODOT.PRESET that disagrees is a problem, so
        a renamed file says so rather than answering to two names. */
    Preset readPreset (const std::string& slug, std::string_view json);

    /*  THE SAME, WITHOUT READING THE DESCRIPTION WHOLE (DP.11): the root's
        GODOT key, its FULL_PATH and the names under CONTENTS are found by
        walking the text once, string-aware, and the nodes are counted by
        their FULL_PATH keys. Twenty megabytes of preset parsed into nodes
        cost the Debug binary fifteen seconds at every start of serve, for
        rows that say what the file's head says; the whole read stays the
        gate's (`wfg presets --check`) and the tests'. What the quick read
        cannot see - a node the description would refuse - the device's own
        load says when one is made. */
    Preset readPresetQuickly (const std::string& slug, std::string_view json);

    /*  WHERE A DESCRIPTION'S HEADER IS (namespace draft §76, ARY): the root
        object's `GODOT` value, from its `{` to just past its `}`, found by the
        quick read's walk - or nothing, when the root has no GODOT object. */
    struct TextSpan
    {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    std::optional<TextSpan> rootGodotSpan (std::string_view json);

    /*  THE DESCRIPTION WITH `header` AS ITS GODOT KEY: written compact in place
        of the old one, or as the root's first member when it had none; every
        other byte of the file as it was. Empty when the text is not a JSON
        object. What `preset.save` writes (ARY). */
    std::string withGodotHeader (std::string_view json, const json::Value& header);

    /*  Every preset in a folder, by slug, each marked with `origin`: what a
        table reads, and what the thread that reads your folder again hands it
        (§76, ARZ). Empty when the folder is not there. */
    std::vector<Preset> readPresetFolder (const std::string& folderPath, bool quickly, const std::string& origin);

    /*  Every preset in a folder, by slug. Empty when the folder is not there,
        which a replay and a tree dump treat as the ordinary case: they offer
        no menu and make no device.

        TWO FOLDERS (namespace draft §76, ART): the shipped one beside the
        binary, read once - it changes only with the program - and the user's
        own, read at the start and again whenever `adoptYours` is handed a
        fresh reading. A preset of yours with a shipped preset's name is used
        in its place; the shipped one is kept aside, not dropped, so that
        taking your file away brings it back. Swapping your part moves
        presets, never copies them: the shipped text is twenty megabytes. */
    class PresetTable
    {
    public:
        void scan (const std::string& folderPath);

        /** The same folder through `readPresetQuickly`: serve's way in. */
        void scanQuickly (const std::string& folderPath);

        /** The shipped folder and yours; `yoursPath` may be empty for none. */
        void scan (const std::string& shippedPath, const std::string& yoursPath);
        void scanQuickly (const std::string& shippedPath, const std::string& yoursPath);

        /*  YOUR PRESETS, READ AGAIN: what was yours goes, what it hid comes
            back, and these take their places. Tick thread, as every reader
            of the table is; moves the revision. */
        void adoptYours (std::vector<Preset> yours, const std::string& yoursPath);

        const std::vector<Preset>& all() const noexcept { return presets; }
        const Preset* find (const std::string& slug) const noexcept;
        const std::string& folder() const noexcept { return folderPath; }

        /** Your folder, as last read; empty when none was given. */
        const std::string& yoursFolder() const noexcept { return yoursPath; }

        /*  Moved by every scan, so a reader that caches what it made of the
            table - the tree's rows (DP.11) - knows when to make them again.
            A table never scanned reads nought. */
        std::uint64_t revision() const noexcept { return stamp; }

    private:
        std::vector<Preset> presets;
        std::vector<Preset> hidden;       ///< shipped presets yours stand in for
        std::string folderPath;
        std::string yoursPath;
        std::uint64_t stamp = 0;
    };
}
