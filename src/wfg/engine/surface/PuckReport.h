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
    WHAT A SPACEMOUSE SAYS, READ (namespace draft 45, O.11).

    3Dconnexion's pucks speak HID: report 1 carries the three pushes as little-
    endian 16-bit numbers - and on the newer devices the three twists after
    them, thirteen bytes in all - report 2 the three twists on the older ones,
    and report 3 the buttons, a bit each. A push runs to about 350 either way,
    read here as -1..1. The layout and the 350 are spatcore's
    (controllers/spacemouse/SpaceMouseDevice.h, `parseReport`), transcribed
    rather than included: spatcore's driver hands its axes to the message
    thread, and only when they change (ZE).

    std only and pure, so a report is checked against bytes.
*/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wfg::surface
{
    /*  3Dconnexion's vendor identifier: every SpaceMouse since the company
        left Logitech's, the Compact, the Wireless, the Pro and the Enterprise. */
    inline constexpr unsigned short puckVendor = 0x256f;

    inline constexpr double puckRawMax = 350.0;

    /*  The puck at a moment: its six axes - tx, ty, tz, rx, ry, rz - from -1 to
        1, and its two buttons. */
    struct PuckState
    {
        std::array<double, 6> axes {};
        std::array<bool, 2> buttons {};
    };

    namespace detail
    {
        inline void readPuckAxes (const std::uint8_t* data, std::size_t first, PuckState& state) noexcept
        {
            for (std::size_t at = 0; at < 3; ++at)
            {
                const auto raw = static_cast<std::int16_t> (static_cast<std::uint16_t> (data[at * 2])
                                                            | static_cast<std::uint16_t> (data[at * 2 + 1] << 8));
                state.axes[first + at] = std::clamp (static_cast<double> (raw) / puckRawMax, -1.0, 1.0);
            }
        }
    }

    /*  One report read into the state: the axes or the buttons it carries,
        the rest left as they were. A report too short for what it claims, or
        one of another kind - the LED's, the battery's - changes nothing. */
    inline void readPuckReport (const std::uint8_t* data, std::size_t length, PuckState& state) noexcept
    {
        if (data == nullptr || length < 1)
            return;

        switch (data[0])
        {
            case 1:
                if (length >= 13)
                {
                    detail::readPuckAxes (data + 1, 0, state);
                    detail::readPuckAxes (data + 7, 3, state);
                }
                else if (length >= 7)
                {
                    detail::readPuckAxes (data + 1, 0, state);
                }

                break;

            case 2:
                if (length >= 7)
                    detail::readPuckAxes (data + 1, 3, state);

                break;

            case 3:
                if (length >= 2)
                {
                    state.buttons[0] = (data[1] & 0x01) != 0;
                    state.buttons[1] = (data[1] & 0x02) != 0;
                }

                break;

            default:
                break;
        }
    }
}
