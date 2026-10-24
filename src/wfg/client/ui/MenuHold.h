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
    A MENU A HAND IS IN IS LEFT ALONE BY THE POLL (the author, 2026-10-24:
    "sometimes selecting an item in a dropdown menu doesn't register and you
    have to click again").

    JUCE hands a ComboBox's pick over in two steps: the popup shuts and the
    item is selected, then `onChange` runs a message later and reads what is
    selected THEN. A pass that put the engine's value back in between - every
    panel here re-selects its menus from the tree at tick rate - turned the
    pick into a send of the old value, and nothing said so. A pass that came
    after the send but before the engine's answer showed the old item again.

    So a pass asks this before it touches a box's selection or its items: not
    while the popup is open, nor for `holdMs` after a pass last saw it open,
    which covers both steps and the engine's round trip. JUCE says nothing
    when a popup shuts, so it is SEEN open: a pass comes every few tens of
    milliseconds and a hand holds a menu open for more than that. A pick the
    engine refused is put right when the hold ends.
*/

#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::client::ui
{
    class MenuHold
    {
    public:
        static constexpr juce::uint32 holdMs = 750;

        /** Whether `box`'s selection is the hand's for now, and the pass must leave it be. */
        bool holds (const juce::ComboBox& box) noexcept
        {
            return holds (box.isPopupActive(), juce::Time::getMillisecondCounter());
        }

        /** The same, told whether the popup is open and the time, for a test. */
        bool holds (bool open, juce::uint32 now) noexcept
        {
            if (open)
            {
                seenAt = now;
                seen = true;
            }

            return seen && now - seenAt < holdMs;
        }

    private:
        juce::uint32 seenAt = 0;
        bool seen = false;
    };
}
