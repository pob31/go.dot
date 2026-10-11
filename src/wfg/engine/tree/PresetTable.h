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

#include <wfg/engine/tree/Mount.h>

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

        bool usable() const noexcept { return problem.empty(); }

        /** The roots as a prefix row: space-separated, the way `mount/prefix` spells several. */
        std::string rootRow() const;
    };

    /*  Reads one preset from its text. Pure: no filesystem, no engine. The slug
        is the file's name, and a GODOT.PRESET that disagrees is a problem, so
        a renamed file says so rather than answering to two names. */
    Preset readPreset (const std::string& slug, std::string_view json);

    /*  Every preset in a folder, by slug. Empty when the folder is not there,
        which a replay and a tree dump treat as the ordinary case: they offer
        no menu and make no device. */
    class PresetTable
    {
    public:
        void scan (const std::string& folderPath);

        const std::vector<Preset>& all() const noexcept { return presets; }
        const Preset* find (const std::string& slug) const noexcept;
        const std::string& folder() const noexcept { return folderPath; }

    private:
        std::vector<Preset> presets;
        std::string folderPath;
    };
}
