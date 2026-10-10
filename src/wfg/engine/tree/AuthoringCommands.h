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
    AUTHORING FROM A PROCESSOR (PRD §3.26, namespace draft §56): the verbs by
    which WFS-DIY and S21_HiJack write cues into the show, in Go.dot's own
    protocol rather than QLab's. The contract both ends implement is
    docs/godot-authoring-protocol-0.1.md.

      mount.declare  prefix port queryPort name [id] [host]
          The processor becomes a device - made from the datagram's address the
          first time, its host and ports moved after (AEJ). Answers
          /godot/declared <id> <outcome>.

      cue.capture  where target id name number notes messageIds [address value]...
          One OSC cue, made after the standby or at the end of a list, or the
          same cue updated in place when its identifier already names one (AEK).
          Answers /godot/captured <id> <outcome> <number> <name>.

    ONE COMMAND IS ONE UNDO STEP, which is the engine's rule already
    (`beginTransaction` per applied command), and ONE ANSWER, queued on the
    RawSender and sent at the end of the tick (AEI).

    REPLAY NEEDS NO SOCKET AND DRAWS NOTHING. The host a declare was answered
    at, the identifiers a capture drew - the cue's and every further
    message's - ride on the applied record, so `wfg replay` makes the same
    document from the log alone.

    THE DEVICE IS FOUND IN THE DOCUMENT, not in the MountTable: the table knows
    only what serve's after-tick refresh has loaded, so a capture in the tick
    after a declare, a rig and a replay would all disagree with it.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/RawSender.h>

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <string>

namespace wfg::tree
{
    /*  What a declare asks for when it carries a query port: the description
        fetched (serve's MountFetcher). Empty everywhere else - a replay reads
        the fetch's own record, `mount.described`, from the log. */
    using DescribeRequest = std::function<void (const std::string& mountId, const std::string& host,
                                                int queryPort, const std::string& prefix)>;

    /** Adds mount.declare, cue.capture, cue.fireNumber and standby.setNumber, answering through `answers`. */
    void registerAuthoringCommands (CommandRegistry& registry, doc::ShowDocument& document, RawSender& answers,
                                    DescribeRequest describe = {});

    /*  Adds mount.described: a fetched description adopted - the device's
        `namespace` row pointed at the file, the file loaded into `mounts` from
        the bundle `bundleFolder` names (read at call time, as `mount.load`
        reads it) - or the fetch's problem shown on the device. Answers
        /godot/described <id> <nodeCount> <problem>. */
    void registerDescriptionCommands (CommandRegistry& registry, doc::ShowDocument& document, MountTable& mounts,
                                      const juce::File& bundleFolder, RawSender& answers);

    /*  The address in an origin string, or empty: `udp:<ip>:<port>` and
        `ws:<ip>:<port>` give `<ip>` (the text between the first and the last
        colon, so an IPv6 address survives); any other origin gives nothing. */
    std::string originHost (const std::string& origin);

    /*  Where a declared device is answered: its host and port, or nullopt for a
        device on a serial port or one with no port. */
    struct AnswerAddress
    {
        std::string host;
        int port = 0;
    };

    std::optional<AnswerAddress> answerAddressOf (const doc::ShowDocument& document, const std::string& mountId);
}
