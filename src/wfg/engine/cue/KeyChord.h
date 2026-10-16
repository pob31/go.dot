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
    A KEY, SPELLED ONCE (namespace draft §74, AQK, AQW).

    A key trigger fires its cue from a key of the computer a client runs on -
    the window or the page - so the key is matched where it arrives, and both
    clients and the document's door must spell it alike. The spelling is the
    page's key table's (`clients/console/gestures/commands.json`): the
    modifiers in a fixed order, `Mod` (Ctrl, or ⌘ on a Mac), `Alt`, `Shift`,
    then the key - `F6`, `Mod+K`, `Shift+1`, `ArrowLeft`.

    THE KEYS: a letter by the letter it types (so the key engraved A on an
    AZERTY keyboard is A), a digit of the top row by its place (`1`), a digit
    of the number pad (`Num1`), F1 to F24, the arrows, and Enter, Tab, Space,
    Escape, Delete, Backspace, Home, End, PageUp, PageDown, Insert. Nothing
    else: a key that types a different character on every layout is no key
    two clients could agree on.

    THE KEYS THE SHOW KEEPS can never be a trigger's (AQK): GO's Space, Esc
    and Esc Esc, Doh!'s F9 - with any modifier, since a hand reaching for any
    of them in a hurry must never fire a cue instead - and every shortcut
    either client already answers to. std only, so both clients and the
    engine read the one list.
*/

#include <optional>
#include <string>
#include <string_view>

namespace wfg::cue
{
    /*  THE CANONICAL SPELLING of `text`: the modifiers in their order, then
        the key, whatever case or order they came in (`shift+mod+k` is
        `Mod+Shift+K`), `Ctrl` and `Cmd` read as `Mod` and `Option` as `Alt`.
        Empty when `text` is no key this spelling has. */
    std::optional<std::string> canonicalChord (std::string_view text);

    /*  A KEY THE SHOW KEEPS (AQK, AQW): spelled canonically, as the door
        stores it. */
    bool isReservedChord (std::string_view canonical);

    /*  A chord from its parts, as a client reads them off a key press:
        `key` is the key's own name in this spelling (`K`, `F6`, `Num1`). Empty
        when the key is not one this spelling has. */
    std::optional<std::string> chordOf (bool mod, bool alt, bool shift, std::string_view key);
}
