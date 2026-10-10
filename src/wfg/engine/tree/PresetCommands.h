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

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/PresetTable.h>

#include <juce_core/juce_core.h>

#include <string>

namespace wfg::tree
{
    /*  A DEVICE MADE FROM A PRESET (namespace draft §57, AFO).

        `mount.createFromPreset <slug> [id]` copies the installed preset into
        the bundle as `namespaces/<slug>-v<version>.json` - once: a second
        device from the same preset shares the file - makes the device with the
        roots the file has, its port and transport from the file, and
        `mount/preset` set to `<slug>@<version>`; then loads it, so its nodes
        are there on the tick it was made. Its record carries what it drew and
        decided - the identifier, the prefix row, the file, the port, the
        transport, the version - so a replay, which has no preset installed,
        rebuilds the device from the record and the file already in the bundle.

        `mount.refreshPreset <id>` writes a newer installed version beside the
        old file and points the device's rows at it, so an undo still names a
        file that exists. Refused when nothing newer is installed.

        `presets` may be null: a replay and a tree dump offer none, and apply a
        create from its own record alone. */
    void registerPresetCommands (CommandRegistry& registry, doc::ShowDocument& document, MountTable& mounts,
                                 const juce::File& bundleFolder, const PresetTable* presets);

    /** The bundle-relative file a preset is kept as: `namespaces/<slug>-v<version>.json`. */
    std::string presetFileFor (const std::string& slug, int version);
}
