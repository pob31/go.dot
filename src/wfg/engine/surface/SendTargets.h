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
    WHAT A SURFACE'S SEND PAGES WALK (namespace draft §145; the author,
    2026-11-23: "Send on the MIDI controller, here a D700, should also show
    the auxes"): one rotary, or one dial of a Stream Deck+, a channel - the
    show's mix channels in output order (`/godot/audio/mixes`), then every
    aux in the rack's order, as the desktop's Sends mixer lays its strips
    (client/model/Sends.cpp). The Pan and Width pages walk the same list.

    A send names either by identifier in its `bus` row, so a turn on an aux's
    rotary makes and moves a send exactly as it does on a mix's. The name is
    read where each lives - a mix's under /godot/bus/, an aux's under
    /godot/slot/ - and an aux nobody named is "Aux n", n its place among the
    rack's channels, the desktop's own word for it (client/model/Rack.cpp).

    std only: the reader is the bridge's.
*/

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::surface
{
    struct SendTarget
    {
        std::string id;
        std::string nameAt;         ///< where its name is published
        std::string unnamed;        ///< what is shown when that name is empty
    };

    namespace sendTargetsDetail
    {
        inline std::vector<std::string> words (std::string_view text)
        {
            std::vector<std::string> out;
            std::size_t at = 0;

            while (at < text.size())
            {
                const auto start = text.find_first_not_of (' ', at);

                if (start == std::string_view::npos)
                    break;

                const auto end = text.find (' ', start);
                out.emplace_back (text.substr (start, end == std::string_view::npos ? std::string_view::npos
                                                                                     : end - start));
                at = end == std::string_view::npos ? text.size() : end;
            }

            return out;
        }
    }

    /** `textAt (address)` is the bridge's reading of one row as text. */
    template <typename TextAt>
    std::vector<SendTarget> sendTargetsOf (TextAt&& textAt)
    {
        std::vector<SendTarget> out;

        for (auto& id : sendTargetsDetail::words (textAt (std::string ("/godot/audio/mixes"))))
        {
            auto nameAt = "/godot/bus/" + id + "/name";
            auto unnamed = id;
            out.push_back ({ std::move (id), std::move (nameAt), std::move (unnamed) });
        }

        int rackChannel = 0;

        for (auto& id : sendTargetsDetail::words (textAt (std::string ("/godot/slot/order"))))
        {
            const auto base = "/godot/slot/" + id + "/";

            if (textAt (base + "kind") != "rackChannel")
                continue;

            ++rackChannel;

            if (textAt (base + "access") != "shared")
                continue;

            out.push_back ({ std::move (id), base + "name", "Aux " + std::to_string (rackChannel) });
        }

        return out;
    }

    /** What a page was made from: the channels in order, so a new one, or one gone, makes it again. */
    inline std::string sendTargetsKey (const std::vector<SendTarget>& targets)
    {
        std::string key;

        for (const auto& target : targets)
            key += target.id + ' ';

        return key;
    }
}
