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
    DEVICES FOUND ON THE NETWORK (namespace draft §76, ARW, ARX).

    `network.browse` asks the network for OSCQuery servers now - the Network
    tab's FOUND as it opens. What answers is not a decision and is no command:
    it is the runtime row `network/found`, which serve's discovery thread
    fills (oscquery/Discovery.h).

    `mount.describeAt <host> <queryPort> [name] [id]` is the decision: a device
    made from the description at that address, found or typed. It draws the
    device's identifier on the tick and hands the fetch to the thread that
    fetches descriptions; what that thread learned comes back as
    `mount.discovered`, the engine's own record, which makes the device - or
    moves the one already holding every root of it - and loads it. Both are on
    the log, so a replay rebuilds the device with no network in the room.

    Where no hook is given - a replay, a tree dump - `network.browse` and
    `mount.describeAt` are taken and ask nobody.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>

#include <juce_core/juce_core.h>

#include <functional>
#include <string>

namespace wfg::tree
{
    struct DiscoveryHooks
    {
        std::function<void()> browse {};

        /*  The fetch of a description at an address: the device's identifier
            drawn, the host, the query port, the name to give it. */
        std::function<void (const std::string& id, const std::string& host, int queryPort,
                            const std::string& name)> describe {};

        /*  What became of a pick: empty when the device was made or moved,
            else the sentence - for `network/discoveryProblem`. */
        std::function<void (const std::string& problem)> told {};
    };

    /** A literal IPv4 address, four numbers of 0 to 255: `mount/host` takes no name. */
    bool isIpv4Address (const std::string& text);

    void registerDiscoveryCommands (CommandRegistry& registry, doc::ShowDocument& document, MountTable& mounts,
                                    const juce::File& bundleFolder, DiscoveryHooks hooks = {});
}
