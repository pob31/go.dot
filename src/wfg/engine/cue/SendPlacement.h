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

#pragma once

#include <string_view>
#include <vector>

/*  WHERE A SEND SITS IN ITS MIX (namespace draft §113, the author 2026-10-25:
    "Can the sends also have panoramic/balance controls?").

    A send was one number, a level, spread by a fixed rule - a mono cue onto
    every channel of its mix at full, two sides onto two channels side to side,
    two sides onto one channel at half each (§42). This is the second number
    and a half: a pan where one channel meets two, a width, a balance and a
    swap where two meet two, a balance where two meet one. Which of them is
    heard is the SHAPE of the send, derived from the cue's channel count and
    the bus's width and never stored, so the rows that do not apply tonight
    stay in the document for the day the file or the bus changes.

    THE LAW IS "FULL AT CENTRE, LOWER THE FAR SIDE" (decision BCC). At the
    defaults every shape is exactly the spread it always was, so a show saved
    before this existed sounds the same; a move away from centre only turns the
    other side down, so placing a cue never costs level; and any pair of gains
    a matrix could say - QLab's crosspoints, a desk's - is one level and one
    pan or balance, with nothing left over. Live's constant-power pan is 3 dB
    lower at centre, which the import notes as an approximation.

    Pure arithmetic on a gains list, shared by the Runner (which turns it into
    coefficients), the tree (which publishes the shape's word) and the
    importers (which fit a matrix back into these numbers). No JUCE here. */
namespace wfg::cue
{
    struct SendPlace
    {
        double pan = 0.0;       // -1 left .. 1 right; a mono cue into a stereo bus
        double width = 1.0;     // 0 mono .. 1 as the file is; two channels into two
        double balance = 0.0;   // -1 left .. 1 right; two channels into two or into one
        bool swap = false;      // the two sides exchanged first; two channels into two
    };

    /** The shape's word, as `send/placement` publishes it: none, pan, stereo or fold. */
    std::string_view placementWordOf (int channels, int width) noexcept;

    /*  Apply the placement to a gains list laid out as the Runner's `spreadOf`
        builds it: `channels` rows of `width` columns, row-major, every cell the
        spread's own value times `gain`. Shapes the law does not cover are
        left exactly as they were. */
    void applyPlacement (std::vector<double>& gains, int channels, int width, double gain,
                         const SendPlace& place) noexcept;

    /*  THE OTHER WAY ROUND, for an importer holding a matrix: the level gain and
        the placement that reproduce `gains` (rows of `width` columns) exactly
        under the law, when one exists. False when no placement says it - more
        than two rows, cross terms that are not symmetric - and the caller keeps
        its matrix as a route. `levelGain` is a linear gain, nought for silence. */
    bool fitPlacement (const std::vector<double>& gains, int channels, int width,
                       double& levelGain, SendPlace& place) noexcept;
}
