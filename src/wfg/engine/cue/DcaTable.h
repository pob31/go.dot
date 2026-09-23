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
    WHAT EACH DCA IS TRIMMING BY TONIGHT, in dB (PRD §3.28).

    A DCA's name, its short name and the DCA it sits inside are decisions and
    live in the document. Its TRIM is not: it is where somebody's fader is, it
    changes fifty times a second while a hand rides it, and PRD §4.10 keeps
    that out of the show - so it lives here, beside the run table, and starts at
    nought every time a show opens. Nought is the resting value (§4.6): a DCA
    nobody has touched trims nothing.

    THE SUM IS THE RUNNER'S, NOT THIS TABLE'S. What a run plays at is its own
    level plus every trim above it - its groups' and its DCAs' - added in
    `Runner::applyLevels`; this only answers what one DCA is doing. Sums are
    order-independent, which is the property that matters: cues arrive in
    whatever order the operator pressed GO.

    THREADING: none of its own. Written on the tick thread - by `node.set`'s
    live door and by a fade aimed at a DCA - and read there by the Runner and
    by the parameter tree when it publishes. The model's thread, like the run
    table.
*/

#include <map>
#include <string>

namespace wfg::cue
{
    class DcaTable
    {
    public:
        /** Nought for a DCA nobody has touched, and for one that is not there. */
        double trimOf (const std::string& dcaId) const
        {
            const auto found = trims.find (dcaId);
            return found != trims.end() ? found->second : 0.0;
        }

        void set (const std::string& dcaId, double decibels) { trims[dcaId] = decibels; }

        /** Every DCA back to nought - a show closed, or another opened. */
        void clear() { trims.clear(); }

    private:
        std::map<std::string, double> trims;
    };
}
