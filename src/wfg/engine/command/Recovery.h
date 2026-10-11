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
    THE STOPS AN OPERATOR MAY ALWAYS MAKE (PRD §6.2, §4.4): "Esc is accepted at
    any moment, and takes effect when the device returns. An operator may
    always abort, outage or not; the abort is never the thing that is
    refused."

    Esc (`run.stopAll`), double Esc (`run.killAll`), and the same two for one
    run (`run.stop`, `run.kill`) - the four commands that end what is playing
    and start nothing. ONE LIST, FOR TWO PLACES THAT MUST AGREE:

      - the audio's admission gate (audio/AudioSettings.cpp), which lets them
        through while the interface is reconnecting and refuses a GO;
      - a backup that takes over from a main it lost (namespace draft §58,
        RD.7, decision AGJ), which applies the stops it had passed on and the
        main never confirmed, and drops everything else - a GO, Doh!, a fader,
        a standby move - since a command that was never seen to land cannot
        be known to have landed, and of all of them only a stop is safe to
        make twice.

    Doh! is NOT here, though the audio's gate admits it too (PRD §3.32, its own
    entry there): it puts back what a GO did, which is recovery in an outage
    and a guess after a takeover - the main may have taken the GO back, or not,
    and the backup cannot tell which (AGJ drops it).
*/

#include <string_view>

namespace wfg::recovery
{
    /** Whether a command is one of the operator's stops: Esc, double Esc, or either for one run. */
    inline bool isStop (std::string_view command) noexcept
    {
        return command == "run.stopAll" || command == "run.killAll" || command == "run.stop" || command == "run.kill";
    }
}
