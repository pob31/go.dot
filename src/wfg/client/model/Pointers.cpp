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

#include <wfg/client/model/Pointers.h>

#include <algorithm>

namespace wfg::client::model
{
    namespace
    {
        constexpr int perKind = 1000;
    }

    int pointerKey (PointerKind kind, int index) noexcept
    {
        const auto number = std::clamp (index, 0, perKind - 1);

        switch (kind)
        {
            case PointerKind::mouse: return number;
            case PointerKind::touch: return perKind + number;
            case PointerKind::pen:   return 2 * perKind + number;
        }

        return number;
    }

    PointerKind kindOfPointer (int key) noexcept
    {
        if (key >= 2 * perKind)
            return PointerKind::pen;

        if (key >= perKind)
            return PointerKind::touch;

        return PointerKind::mouse;
    }

    bool isFinger (int key) noexcept
    {
        return kindOfPointer (key) == PointerKind::touch;
    }

    bool PointerTurns::down (int key)
    {
        if (taken.count (key) != 0)
            return true;

        if (refused.count (key) != 0)
            return false;

        /*  THE OTHER KIND IS ON IT: a finger while the mouse is down, the
            mouse while a finger is. Refused for the whole press. */
        const auto acting = isFinger (key) ? mouseDown() : fingerDown();

        if (acting)
        {
            refused.insert (key);
            return false;
        }

        taken.insert (key);
        return true;
    }

    bool PointerTurns::owns (int key) const
    {
        return taken.count (key) != 0;
    }

    bool PointerTurns::up (int key)
    {
        refused.erase (key);
        return taken.erase (key) != 0;
    }

    std::vector<int> PointerTurns::sweep (int dragging)
    {
        if (dragging > 1)
            return {};

        return clear();
    }

    std::vector<int> PointerTurns::held() const
    {
        return { taken.begin(), taken.end() };
    }

    bool PointerTurns::fingerDown() const
    {
        return std::any_of (taken.begin(), taken.end(), [] (int key) { return isFinger (key); });
    }

    bool PointerTurns::mouseDown() const
    {
        return std::any_of (taken.begin(), taken.end(), [] (int key) { return ! isFinger (key); });
    }

    std::vector<int> PointerTurns::clear()
    {
        auto stale = held();
        taken.clear();
        refused.clear();
        return stale;
    }
}
