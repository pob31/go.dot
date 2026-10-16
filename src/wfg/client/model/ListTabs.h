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
    THE SHOW'S LISTS, AS TABS (namespace draft §74, AQM, AQS). A show list is
    walked by GO; a cart is a list GO never meets, its cues fired out of order.
    The window shows one list at a time, and which one is the window's own
    business: picking a show list's tab moves GO's focus there as the page's
    tab always has, and picking a cart's tab moves nothing - the transport
    goes on saying which list GO acts on.

    std only, like the rest of model/.
*/

#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct ListTab
    {
        std::string id;
        std::string name;
        bool cart = false;

        bool operator== (const ListTab&) const = default;
    };

    /** The show's lists in their order, each with whether it is a cart. */
    std::vector<ListTab> readListTabs (const tree::TreeSnapshot& snapshot);

    /*  THE LIST GO ACTS ON: the engine's focus when it names a show list,
        else the first show list, else nothing - the engine's own fallback
        (CueList.h, `Focus::list`), so the window never names a cart for GO. */
    std::string goListOf (const tree::TreeSnapshot& snapshot);

    /*  WHAT A TAB SAYS: its name, a mark before the list GO acts on, and the
        word "cart" after a cart's - words, so colour carries nothing alone
        (PRD §4.8). The window's words, the author's to change. */
    std::string tabWords (const ListTab& tab, bool goActsOnIt);

    /*  A NAME FOR A NEW LIST nobody has named yet: "Cart", then "Cart 2"; a
        show list "List 2" and on. Renamed by a double click on its tab. */
    std::string newListName (const std::vector<ListTab>& tabs, bool cart);
}
