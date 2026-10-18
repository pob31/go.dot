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

#include <wfg/client/model/ListTabs.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>

namespace wfg::client::model
{
    std::vector<ListTab> readListTabs (const tree::TreeSnapshot& snapshot)
    {
        std::vector<ListTab> out;

        for (const auto& id : words (text (snapshot, "/godot/list/order")))
        {
            ListTab tab;
            tab.id = id;
            tab.name = text (snapshot, "/godot/list/" + id + "/name");
            tab.cart = text (snapshot, "/godot/list/" + id + "/kind") == "cart";
            out.push_back (std::move (tab));
        }

        return out;
    }

    std::string goListOf (const tree::TreeSnapshot& snapshot)
    {
        const auto tabs = readListTabs (snapshot);
        const auto focus = text (snapshot, "/godot/list/focus");

        for (const auto& tab : tabs)
            if (tab.id == focus && ! tab.cart)
                return focus;

        for (const auto& tab : tabs)
            if (! tab.cart)
                return tab.id;

        return {};
    }

    std::string tabWords (const ListTab& tab, bool goActsOnIt)
    {
        const auto name = tab.name.empty() ? tab.id : tab.name;

        return std::string (goActsOnIt ? "\xe2\x96\xb8 " : "") + name + (tab.cart ? " \xc2\xb7 cart" : "");
    }

    bool mayRemoveList (const std::vector<ListTab>& tabs, const std::string& listId)
    {
        const auto found = std::find_if (tabs.begin(), tabs.end(), [&listId] (const ListTab& tab) { return tab.id == listId; });

        if (found == tabs.end())
            return false;

        if (found->cart)
            return true;

        return std::count_if (tabs.begin(), tabs.end(), [] (const ListTab& tab) { return ! tab.cart; }) > 1;
    }

    std::string newListName (const std::vector<ListTab>& tabs, bool cart)
    {
        const std::string stem = cart ? "Cart" : "List";

        const auto taken = [&tabs] (const std::string& name)
        {
            return std::any_of (tabs.begin(), tabs.end(), [&name] (const ListTab& tab) { return tab.name == name; });
        };

        if (cart && ! taken (stem))
            return stem;

        for (int n = 2;; ++n)
            if (const auto name = stem + " " + std::to_string (n); ! taken (name))
                return name;
    }
}
