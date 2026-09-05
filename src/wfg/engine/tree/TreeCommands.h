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
    The tree's own commands: taking a node and giving it back.

    `node.set` is not here. A value write goes to the document's single write
    path and is registered with the rest of the document's commands, which is
    where it belongs: the tree is a projection and owns no value.

    WHY TOUCHING IS A COMMAND like any other, rather than a flag on a
    connection. Because PRD §3.15 admits no second path: everything that
    changes engine state arrives as an event, is applied by the tick thread in
    arrival order, and is written to the log with the tick it landed on. A
    touch changes what the engine will send and to whom, so a replay that did
    not re-apply it would produce a different set of outbound messages from the
    session it claims to reproduce.

    THE ORIGIN COMES FROM THE EVENT, not from an argument. A client cannot
    touch a node on somebody else's behalf, because the only origin a handler
    can see is the one the transport stamped on the way in. That is not a
    permission check - Phase 1 has no permissions - it is that the question
    "who is holding this" has exactly one truthful answer and it is not the
    caller's to supply.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/Touches.h>

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

namespace wfg::tree
{
    /** Adds node.touch and node.release, both bound to `touches`. */
    void registerTreeCommands (CommandRegistry& registry, TouchTable& touches);

    //==============================================================================
    /*  Mounts, read out of the document and loaded off disk.

        The declaration lives in the show - `/godot/mount/<id>` says where a
        target is mounted and which file describes it - and the description
        lives in the bundle's `namespaces/`. These four put the two together;
        MountTable itself never touches a filesystem, which is what keeps every
        rule in Mount.h testable against a string literal.
    */

    /** Every mount the document declares, in document order. */
    std::vector<std::string> declaredMountIds (const doc::ShowDocument& document);

    /** One mount's declaration, or nullopt when the document has no such mount. */
    std::optional<MountDeclaration> mountDeclarationFor (const doc::ShowDocument& document,
                                                         const std::string& mountId);

    /** Reads that mount's namespace file out of the bundle and loads it. */
    /*  Every network cue that asks for something its target cannot give.

        QUESTION K, ANSWERED THE STRICT WAY (namespace draft §9). `transport`
        says how to send and nothing says whether a box can be ASKED, so a cue
        whose wait is `verified` aimed at a write-only device is a cue that can
        never succeed - and without this check nothing notices until somebody is
        standing in a theatre wondering why the list has stopped.

        It is a check on the DOCUMENT and needs no mount table, no socket and no
        device: the cue names an address, the address falls under a mount's
        prefix, and the mount says whether it can answer. So it runs wherever a
        show is read, including `wfg validate` on a laptop with nothing plugged
        in - which is the machine somebody is actually sitting at when they have
        time to fix it.

        One message per offending cue, empty when the show is sound. */
    std::vector<std::string> checkNetworkCues (const doc::ShowDocument&);

    MountResult loadMountFromBundle (const doc::ShowDocument& document, MountTable& mounts,
                                     const juce::File& bundleFolder, const std::string& mountId);

    /*  All of them, in document order. Returns every problem, each already
        saying which mount it came from. A mount that fails does not stop the
        others: one unreadable description should cost that one target, not the
        show. */
    std::vector<std::string> loadAllMountsFromBundle (const doc::ShowDocument& document,
                                                      MountTable& mounts,
                                                      const juce::File& bundleFolder);

    /*  Adds `mount.load`, which re-reads one mount's description.

        `bundleFolder` is held BY REFERENCE and read at call time, because which
        bundle is open changes while the engine runs and the command has to
        follow it. The caller owns that File and updates it on load. */
    void registerMountCommands (CommandRegistry& registry, const doc::ShowDocument& document,
                                MountTable& mounts, const juce::File& bundleFolder);
}
