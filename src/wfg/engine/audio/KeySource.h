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
    WHAT AN EFFECT LISTENS TO (namespace draft §141): its KEY - the signal a
    ducker or a sidechain compressor follows in place of the one it changes.

    THE SHOW NAMES IT; THIS IS WHERE THE GRAPH FINDS IT. The document holds a
    word on the insert (`fx/key`): `self`, an aux's id, an input's id, or
    nothing (BHW). The Runner turns that word into one of these on every pass
    that reads the inserts - an input's id into the logical inputs it spans,
    an aux's into its row of the key bus - so a renumbered patch or a reordered
    rack is followed without the audio side knowing a name.

    ONE WORD ON THE AUDIO SIDE. Packed into thirty-two bits, so the tick
    thread hands it to a plugin place with one relaxed store and the audio
    thread reads it once a block with one load: no lock, no allocation, no
    tearing (PRD §4.2).

    NAMES NO JUCE TYPE: the cue layer carries it in FxSetting, and a test
    builds one with no engine.
*/

#include <algorithm>
#include <cstdint>

namespace wfg::audio
{
    struct KeySource
    {
        enum class Kind : std::uint8_t
        {
            none,       ///< no key: the effect listens to what it changes, a plugin's sidechain is silence
            input,      ///< a named input: `first` and `width` are its logical inputs
            aux,        ///< an aux: `first` is its row of the key bus
            self        ///< this cue, after its level
        };

        Kind kind = Kind::none;
        int first = 0;
        int width = 0;

        /** The widest input a key names: an input is one to eight channels wide. */
        static constexpr int maxWidth = 63;

        bool isNone() const noexcept { return kind == Kind::none; }

        /*  KIND in the top two bits, WIDTH in the next six, FIRST in the
            twenty-four below - more inputs and auxes than any show has. A
            first past that, or a negative one, is no key. */
        std::uint32_t packed() const noexcept
        {
            if (kind == Kind::none || first < 0 || first >= (1 << 24))
                return 0;

            return (static_cast<std::uint32_t> (kind) << 30)
                 | (static_cast<std::uint32_t> (std::clamp (width, 0, maxWidth)) << 24)
                 | static_cast<std::uint32_t> (first);
        }

        static KeySource unpacked (std::uint32_t word) noexcept
        {
            KeySource out;
            out.kind = static_cast<Kind> ((word >> 30) & 0x3u);
            out.width = static_cast<int> ((word >> 24) & 0x3fu);
            out.first = static_cast<int> (word & 0xffffffu);

            if (out.kind == Kind::none)
                return {};

            return out;
        }

        friend bool operator== (const KeySource& a, const KeySource& b) noexcept
        {
            return a.packed() == b.packed();
        }

        friend bool operator!= (const KeySource& a, const KeySource& b) noexcept
        {
            return ! (a == b);
        }
    };
}
