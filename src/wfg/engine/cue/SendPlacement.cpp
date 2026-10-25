/*
    Go.dot - show control for sound, light and video
    Copyright (C) 2026 Pierre-Olivier Boulant

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <wfg/engine/cue/SendPlacement.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace wfg::cue
{
    namespace
    {
        constexpr double sameAs = 1.0e-6;   // two gains the fit may call one

        bool close (double a, double b) noexcept
        {
            return std::abs (a - b) <= sameAs * std::max (1.0, std::max (std::abs (a), std::abs (b)));
        }

        /*  The far side's share for a pan or a balance: full on the side the
            value leans to, 1 - |value| on the other. */
        double leftShare (double value) noexcept  { return std::min (1.0, 1.0 - value); }
        double rightShare (double value) noexcept { return std::min (1.0, 1.0 + value); }

        /*  A pan or a balance from two sides' gains, the louder one full: the
            inverse of the two shares above, exact for any pair. Nought when
            both are nought, so silence has no lean. */
        double leanOf (double left, double right) noexcept
        {
            const auto loud = std::max (left, right);

            if (loud <= 0.0)
                return 0.0;

            return left >= right ? -(1.0 - right / loud) : (1.0 - left / loud);
        }

        std::size_t cell (int input, int width, int channel) noexcept
        {
            return static_cast<std::size_t> (input * width + channel);
        }
    }

    std::string_view placementWordOf (int channels, int width) noexcept
    {
        if (channels == 1 && width == 2) return "pan";
        if (channels == 2 && width == 2) return "stereo";
        if (channels == 2 && width == 1) return "fold";
        return "none";
    }

    void applyPlacement (std::vector<double>& gains, int channels, int width, double gain,
                         const SendPlace& place) noexcept
    {
        if (gains.size() != static_cast<std::size_t> (channels) * static_cast<std::size_t> (width))
            return;

        const auto pan = std::clamp (place.pan, -1.0, 1.0);
        const auto widthOf = std::clamp (place.width, 0.0, 1.0);
        const auto balance = std::clamp (place.balance, -1.0, 1.0);

        if (channels == 1 && width == 2)
        {
            gains[0] = gain * leftShare (pan);
            gains[1] = gain * rightShare (pan);
            return;
        }

        if (channels == 2 && width == 2)
        {
            /*  Swap first, then width, then balance by the OUTPUT side: the
                sides are exchanged, each output hears `a` of its own side and
                `c` of the other, and the balance lowers one output - so a
                swapped cue leaned left is quieter on the LEFT speaker, which
                is what a hand on a balance knob means. */
            const auto left = place.swap ? 1 : 0;
            const auto right = place.swap ? 0 : 1;
            const auto a = (1.0 + widthOf) * 0.5;
            const auto c = (1.0 - widthOf) * 0.5;
            const auto onLeft = leftShare (balance);
            const auto onRight = rightShare (balance);

            gains[cell (left, 2, 0)] = gain * a * onLeft;
            gains[cell (right, 2, 0)] = gain * c * onLeft;
            gains[cell (left, 2, 1)] = gain * c * onRight;
            gains[cell (right, 2, 1)] = gain * a * onRight;
            return;
        }

        if (channels == 2 && width == 1)
        {
            /*  The fold's two halves, leaned: nought is §42's half each, -1
                the left alone, 1 the right alone. The shares sum to one, so
                a coherent signal keeps its level however it is leaned. */
            gains[0] = gain * (1.0 - balance) * 0.5;
            gains[1] = gain * (1.0 + balance) * 0.5;
        }
    }

    bool fitPlacement (const std::vector<double>& gains, int channels, int width,
                       double& levelGain, SendPlace& place) noexcept
    {
        place = {};
        levelGain = 0.0;

        if (gains.size() != static_cast<std::size_t> (channels) * static_cast<std::size_t> (width))
            return false;

        for (const auto g : gains)
            if (g < 0.0)
                return false;

        //  Mono into stereo: the louder side is the level, the ratio the pan.
        if (channels == 1 && width == 2)
        {
            levelGain = std::max (gains[0], gains[1]);
            place.pan = leanOf (gains[0], gains[1]);
            return true;
        }

        //  Two sides into one: the sum is the level, the share the balance.
        if (channels == 2 && width == 1)
        {
            levelGain = gains[0] + gains[1];

            if (levelGain > 0.0)
                place.balance = (gains[1] - gains[0]) / levelGain;

            return true;
        }

        if (channels == 2 && width == 2)
        {
            auto ll = gains[cell (0, 2, 0)], lr = gains[cell (0, 2, 1)];
            auto rl = gains[cell (1, 2, 0)], rr = gains[cell (1, 2, 1)];

            /*  Each output column is (a, c) times its balance share, in some
                order: the own side gets `a` and the other `c`, with a >= c.
                A column whose larger term comes from the OTHER input is the
                swapped reading; both columns must agree. */
            const auto leftOwn = ll >= rl;
            const auto rightOwn = rr >= lr;

            if (leftOwn != rightOwn)
            {
                //  One column leans its own way and the other does not: not one width.
                if (! (close (ll, rl) || close (rr, lr)))
                    return false;
            }

            if (! leftOwn && ! rightOwn)
            {
                place.swap = true;
                std::swap (ll, rl);
                std::swap (lr, rr);
            }

            const auto leftColumn = ll + rl;     // a*bl + c*bl = bl (a + c = 1)
            const auto rightColumn = lr + rr;    // br

            if (leftColumn <= 0.0 && rightColumn <= 0.0)
                return true;    // silence: a send at nothing, every control at rest

            /*  The width is the same on both columns, read off whichever is
                heard: a = own / column, and width = a - c = 2a - 1. */
            const auto widthFrom = [] (double own, double column) { return 2.0 * (own / column) - 1.0; };
            const auto wl = leftColumn > 0.0 ? widthFrom (ll, leftColumn) : -2.0;
            const auto wr = rightColumn > 0.0 ? widthFrom (rr, rightColumn) : -2.0;

            if (wl > -2.0 && wr > -2.0 && ! close (wl, wr))
                return false;

            place.width = std::clamp (wl > -2.0 ? wl : wr, 0.0, 1.0);
            levelGain = std::max (leftColumn, rightColumn);
            place.balance = leanOf (leftColumn, rightColumn);
            return true;
        }

        return false;
    }
}
