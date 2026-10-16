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

#include <wfg/client/model/KeyTriggers.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/cue/KeyChord.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

namespace wfg::client::model
{
    std::vector<KeyTrigger> readKeyTriggers (const tree::TreeSnapshot& snapshot)
    {
        std::vector<KeyTrigger> out;

        constexpr std::string_view prefix = "/godot/trigger/";
        constexpr std::string_view kindRow = "/kind";

        for (const auto* node : snapshot.all())
        {
            const std::string_view address = node->address;

            if (address.size() <= prefix.size() + kindRow.size()
                  || address.substr (0, prefix.size()) != prefix
                  || address.substr (address.size() - kindRow.size()) != kindRow
                  || text (node) != "key")
                continue;

            KeyTrigger trigger;
            trigger.id = std::string (address.substr (prefix.size(), address.size() - prefix.size() - kindRow.size()));

            const auto base = std::string (prefix) + trigger.id + "/";
            trigger.cue = text (snapshot, base + "cue");
            trigger.key = text (snapshot, base + "key");
            trigger.enabled = flag (snapshot, base + "enabled") != Flag::no;

            out.push_back (std::move (trigger));
        }

        return out;
    }

    std::vector<std::string> triggersForKey (const std::vector<KeyTrigger>& triggers, std::string_view key)
    {
        std::vector<std::string> out;

        if (key.empty() || cue::isReservedChord (key))
            return out;

        for (const auto& trigger : triggers)
            if (trigger.enabled && trigger.key == key)
                out.push_back (trigger.id);

        return out;
    }

    const KeyTrigger* keyTriggerOf (const std::vector<KeyTrigger>& triggers, const std::string& cueId)
    {
        for (const auto& trigger : triggers)
            if (trigger.cue == cueId)
                return &trigger;

        return nullptr;
    }

    void addKeyLine (Inspection& inspection, const std::vector<KeyTrigger>& triggers)
    {
        if (inspection.count != 1 || inspection.cueId.empty())
            return;

        Field field;
        field.name = "key";
        field.label = "Key";
        field.control = Control::keyCapture;
        field.writable = true;
        field.description = "The key that fires this cue, on this computer and on the page: press the"
                            " button, then the key. A hold member plays while the key is down.";

        if (const auto* trigger = keyTriggerOf (triggers, inspection.cueId))
        {
            field.address = "/godot/trigger/" + trigger->id + "/key";
            field.value = trigger->key;
        }

        inspection.blocks.push_back ({ "key", { std::move (field) } });
    }
}
