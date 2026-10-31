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

#include <wfg/client/model/Cart.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace wfg::client::model
{
    namespace
    {
        //  The cues a run of is going now (the tablet's `goingCues`).
        std::set<std::string> goingCues (const tree::TreeSnapshot& snapshot)
        {
            std::set<std::string> going;

            for (const auto& run : words (text (snapshot, "/godot/run/order")))
            {
                const auto base = "/godot/run/" + run + "/";
                const auto state = text (snapshot, base + "state");

                if (state == "playing" || state == "waiting" || state == "stopping" || state == "postWait")
                    going.insert (text (snapshot, base + "cue"));
            }

            return going;
        }
    }

    std::vector<CartPad> readCartPads (const tree::TreeSnapshot& snapshot, const std::string& cartId)
    {
        std::vector<CartPad> pads;

        if (cartId.empty() || text (snapshot, "/godot/list/" + cartId + "/kind") != "cart")
            return pads;

        const auto going = goingCues (snapshot);

        const auto pad = [&snapshot, &going, &pads] (const std::string& cue, const std::string& bank)
        {
            const auto base = "/godot/cue/" + cue + "/";

            CartPad made;
            made.cue = cue;
            made.number = text (snapshot, base + "number");
            made.name = shownCueName (snapshot, cue);
            made.kind = text (snapshot, base + "kind");
            made.mode = text (snapshot, base + "mode");
            made.colour = text (snapshot, base + "colour");
            made.going = going.count (cue) != 0;
            made.bank = bank;
            pads.push_back (std::move (made));
        };

        for (const auto& cue : words (text (snapshot, "/godot/list/" + cartId + "/order")))
        {
            const auto base = "/godot/cue/" + cue + "/";

            if (text (snapshot, base + "mode") == "sampler")
            {
                const auto bank = shownCueName (snapshot, cue);

                for (const auto& member : words (text (snapshot, base + "order")))
                    pad (member, bank);
            }
            else
            {
                pad (cue, {});
            }
        }

        return pads;
    }

    int padVelocity (double height) noexcept
    {
        //  The desk's pads' rule: the bottom a soft 1, the top a hard 127.
        const auto held = std::clamp (height, 0.0, 1.0);
        return std::clamp (1 + static_cast<int> (std::lround (held * 126.0)), 1, 127);
    }

    std::string cartViewWords (bool showingPads)
    {
        return showingPads ? "Show as a list" : "Show as pads";
    }
}
