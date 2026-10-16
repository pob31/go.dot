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
    A JUCE KEY PRESS IN THE SPELLING KEY TRIGGERS USE (namespace draft §74,
    AQW; engine/cue/KeyChord.h): the window's half of what the page does with
    a browser's key event, so a key chosen in one client is the key pressed in
    the other. `Mod` is the command modifier - Ctrl, or ⌘ on a Mac. A letter
    is the letter its key types, a top-row digit its place, and a key no
    spelling has - a comma, a media key - is no key a trigger can hold.
*/

#include <wfg/engine/cue/KeyChord.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace wfg::client::ui
{
    inline std::optional<std::string> chordOfKey (const juce::KeyPress& press)
    {
        const auto code = press.getKeyCode();
        std::string key;

        static const std::array<std::pair<int, const char*>, 15> named {
            { { juce::KeyPress::leftKey, "ArrowLeft" }, { juce::KeyPress::rightKey, "ArrowRight" },
              { juce::KeyPress::upKey, "ArrowUp" }, { juce::KeyPress::downKey, "ArrowDown" },
              { juce::KeyPress::returnKey, "Enter" }, { juce::KeyPress::tabKey, "Tab" },
              { juce::KeyPress::spaceKey, "Space" }, { juce::KeyPress::escapeKey, "Escape" },
              { juce::KeyPress::deleteKey, "Delete" }, { juce::KeyPress::backspaceKey, "Backspace" },
              { juce::KeyPress::homeKey, "Home" }, { juce::KeyPress::endKey, "End" },
              { juce::KeyPress::pageUpKey, "PageUp" }, { juce::KeyPress::pageDownKey, "PageDown" },
              { juce::KeyPress::insertKey, "Insert" } }
        };

        static const std::array<int, 24> functionKeys {
            juce::KeyPress::F1Key, juce::KeyPress::F2Key, juce::KeyPress::F3Key, juce::KeyPress::F4Key,
            juce::KeyPress::F5Key, juce::KeyPress::F6Key, juce::KeyPress::F7Key, juce::KeyPress::F8Key,
            juce::KeyPress::F9Key, juce::KeyPress::F10Key, juce::KeyPress::F11Key, juce::KeyPress::F12Key,
            juce::KeyPress::F13Key, juce::KeyPress::F14Key, juce::KeyPress::F15Key, juce::KeyPress::F16Key,
            juce::KeyPress::F17Key, juce::KeyPress::F18Key, juce::KeyPress::F19Key, juce::KeyPress::F20Key,
            juce::KeyPress::F21Key, juce::KeyPress::F22Key, juce::KeyPress::F23Key, juce::KeyPress::F24Key
        };

        static const std::array<int, 10> padDigits {
            juce::KeyPress::numberPad0, juce::KeyPress::numberPad1, juce::KeyPress::numberPad2,
            juce::KeyPress::numberPad3, juce::KeyPress::numberPad4, juce::KeyPress::numberPad5,
            juce::KeyPress::numberPad6, juce::KeyPress::numberPad7, juce::KeyPress::numberPad8,
            juce::KeyPress::numberPad9
        };

        for (const auto& [value, word] : named)
            if (code == value)
                key = word;

        for (std::size_t at = 0; key.empty() && at < functionKeys.size(); ++at)
            if (code == functionKeys[at])
                key = "F" + std::to_string (at + 1);

        for (std::size_t at = 0; key.empty() && at < padDigits.size(); ++at)
            if (code == padDigits[at])
                key = "Num" + std::to_string (at);

        if (key.empty() && ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9')))
            key = std::string (1, static_cast<char> (code));

        if (key.empty())
            return std::nullopt;

        const auto mods = press.getModifiers();
        return cue::chordOf (mods.isCommandDown(), mods.isAltDown(), mods.isShiftDown(), key);
    }
}
