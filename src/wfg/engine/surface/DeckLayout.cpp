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

#include <wfg/engine/surface/DeckLayout.h>

#include <algorithm>

namespace wfg::surface
{
    bool hasDials (const DeckSpec& spec) noexcept
    {
        return spec.dials > 0;
    }

    std::vector<DeckRole> deckShowKeys (const DeckSpec& spec)
    {
        if (hasDials (spec))
            return {};

        if (spec.model == DeckModel::pedal)
            return { DeckRole::esc, DeckRole::go, DeckRole::doh };      // GO under the middle foot

        if (spec.keys >= 15)
            return { DeckRole::standbyUp, DeckRole::standbyDown, DeckRole::doh, DeckRole::esc, DeckRole::go };

        if (spec.keys >= 8)
            return { DeckRole::doh, DeckRole::esc, DeckRole::go };

        return { DeckRole::esc, DeckRole::go };
    }

    DeckLayout deckLayoutFor (const DeckSpec& spec, bool showKeys, int pads, int page)
    {
        DeckLayout out;
        out.keys.assign (static_cast<std::size_t> (std::max (0, spec.keys)), DeckKey {});

        const auto shown = showKeys ? deckShowKeys (spec) : std::vector<DeckRole> {};
        const auto showCount = static_cast<int> (shown.size());
        const auto freeKeys = std::max (0, spec.keys - showCount);

        //  The Pedal's keys are the show's alone when they are on.
        for (int at = 0; at < showCount; ++at)
            out.keys[static_cast<std::size_t> (freeKeys + at)].role = shown[static_cast<std::size_t> (at)];

        pads = std::max (0, pads);

        /*  ONE PAGE when the cart fits; otherwise every page gives its last
            free key to turning it - but the Neo, whose touch keys turn it. */
        const auto turnsByTouch = spec.touchKeys >= 2;
        const auto fits = pads <= freeKeys;
        const auto perPage = freeKeys == 0               ? 0
                           : fits || turnsByTouch        ? freeKeys
                                                         : std::max (1, freeKeys - 1);

        out.pages = perPage > 0 ? std::max (1, (pads + perPage - 1) / perPage) : 1;
        out.page = ((page % out.pages) + out.pages) % out.pages;

        const auto first = out.page * perPage;

        for (int at = 0; at < perPage && first + at < pads; ++at)
        {
            auto& key = out.keys[static_cast<std::size_t> (at)];
            key.role = DeckRole::pad;
            key.pad = first + at;
        }

        if (! fits && ! turnsByTouch && freeKeys > 0)
            out.keys[static_cast<std::size_t> (freeKeys - 1)].role = DeckRole::cartPage;

        return out;
    }

    DeckLayout plusLayout (bool showKeys)
    {
        DeckLayout out;
        out.keys = { { DeckRole::pageSend }, { DeckRole::pageEq }, { DeckRole::pageFx }, { DeckRole::pageLoop },
                     { DeckRole::aimPrevious }, { DeckRole::aimNext },
                     { showKeys ? DeckRole::esc : DeckRole::controlsPrevious },
                     { showKeys ? DeckRole::go : DeckRole::controlsNext } };
        return out;
    }
}
