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
    The boxes this show talks to, as a list somebody can read and edit.

    One row per declared device, which in the document is a Mount. The word in
    the window is DEVICE because that is what it is to an operator - a lighting
    desk at an address, a processor on the show LAN - and `mount` is what the
    engine calls the thing that holds its address space. They are one object;
    the plainer word won.

    A DEVICE EITHER DESCRIBES ITSELF OR IT DOES NOT, and that is the only kind
    there is. A described one shipped with an OSCQuery namespace file: its nodes
    are known, so a cue aimed at one is checked before the show and the value is
    coerced to the declared type. An opaque one is a desk somebody typed an
    address into: nothing is published under its prefix and the value goes out
    exactly as it was written. The second is the ordinary case and is what the
    window makes (PRD §3.22).

    WHAT A CUE CARRIES IS THE WHOLE ADDRESS, prefix included, and that is why
    `retarget` exists. Aiming a cue at another device is not a field somebody
    sets: it is a rewrite of the address the cue already holds, swapping the
    leading segment for the new device's. One truth in the file - the address -
    rather than an address and a pointer that can disagree with it.

    A DEVICE MAY ANSWER AT SEVERAL ROOTS. A DiGiCo S21 reached directly speaks
    `/channel/…`, `/console/…` and `/digico/…`, with nothing above them, so its
    `prefix` row holds all three separated by spaces. Which one an address
    matched is `wfg::tree::prefixMatchLength`'s answer - THE ENGINE'S OWN
    FUNCTION, called here rather than restated, because a menu that worked out
    the device differently from the engine that sends the message would name
    one box and write to another. It was restated once and they disagreed.

    std only, and a pure reading: it takes the snapshot the window already holds
    and answers with plain strings, so every rule here is tested with no window,
    no engine and no network.
*/

#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct DeviceRow
    {
        std::string id;

        /** What the show calls it. Empty is ordinary; `label()` covers it. */
        std::string name;

        /*  The addresses this device answers at, as the row spells them:
            "/desk", or "/channel /console /digico" for a desk with several
            roots. `prefixes()` splits it. */
        std::string prefix;

        /** The roots, split. Empty for a device nobody has finished writing. */
        std::vector<std::string> prefixes() const;

        /** The first root, which is what a rewrite puts in front. */
        std::string firstPrefix() const;

        std::string host;

        /** Zero means the document declares none, which is a device that cannot work. */
        int port = 0;

        /** Whether what it sends is accepted, and whether it is sent to. */
        bool rx = false;
        bool tx = true;

        /** Empty for an opaque device, which is most of them. */
        std::string namespaceFile;

        /** How many messages have left for it since the show opened. */
        int sent = 0;

        /*  Why the engine cannot use it as declared, in the engine's own
            sentence, and empty when it can. Never rewritten here: a client that
            invented its own words for a refusal would be a second place for
            them to be wrong. */
        std::string problem;

        /** A device with no description file. See the header. */
        bool opaque() const { return namespaceFile.empty(); }

        /*  What to put in a menu or a list: the name, or the prefix when there
            is none. A device is never a blank row - somebody has to be able to
            point at it before they have finished naming it. */
        std::string label() const;
    };

    /** Every declared device, in identifier order. */
    std::vector<DeviceRow> readDevices (const tree::TreeSnapshot&);

    /*  Which device an address is aimed at, by identifier, or empty for an
        address under none of them.

        THE LONGEST PREFIX WINS, across every root every device declares, and
        the match ends on a separator: "/desktop" is not under "/desk". It is
        the engine's own `prefixMatchLength` doing the work rather than a copy
        of it - see the header above. */
    std::string deviceOf (const std::string& address, const std::vector<DeviceRow>&);

    /*  The address this cue would have if it were aimed at another device.

        Swaps the matched root for the new device's FIRST one. An address under
        no device gets that root put in front of it, which is what makes a cue
        somebody typed by hand aimable without retyping. An empty `deviceId` is
        the inverse: it strips the root and leaves what is under it - honest
        rather than helpful, because a cue aimed at nothing is a thing the
        operator has to be able to say, and `wfg validate` names it.

        THE FIRST ROOT IS A STARTING POINT AND NOT A TRANSLATION. Moving a cue
        between two devices that share a vocabulary - two identical desks - is
        exact. Moving one between devices that do not is a guess at best: a
        fader on a DiGiCo and a fader on a WFS are not the same address with a
        different beginning, and nothing here pretends otherwise. What the menu
        is really for is saying which device a cue is aimed at, and moving it
        between boxes that speak the same language.

        Returns the address unchanged when the identifier names no device. */
    std::string retarget (const std::string& address, const std::vector<DeviceRow>&,
                          const std::string& deviceId);

    /*  The menu, as the inspector wants it: the whole address each choice would
        produce, paired with what to show for it.

        THE KEY IS AN ADDRESS AND NOT AN IDENTIFIER, which is what lets the
        inspector commit a choice with the `node.set` it already has: picking a
        device writes the rewritten address to the cue's `address` row, and no
        new command, attribute or refusal is needed anywhere. "(none)" comes
        first, as it does for an output, because empty is a choice and not an
        absence. */
    std::vector<std::pair<std::string, std::string>>
        targetChoices (const std::string& address, const std::vector<DeviceRow>&);
}
