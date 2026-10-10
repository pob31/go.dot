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

#include <wfg/engine/cue/Preparedness.h>

namespace wfg::cue
{
    PrepareWord runWordOf (const Run& run)
    {
        if (run.state == runState::failed)
            return { preparedness::partial, run.error };

        /*  ARMED IS THE AUDIO SIDE'S TO SAY: until `audio.armed` the voice may
            not be real, and the row said `armed` the tick the arm was asked.
            A show with no player never confirms - its track stays unset - and
            reads as it always did. */
        if (run.kind == "media" && run.state == runState::armed && run.track >= 0 && ! run.armConfirmed
              && run.prepare == preparedness::armed)
            return { preparedness::preparing };

        return { run.prepare };
    }

    PrepareWord videoWordOf (bool missing, bool rendering, const std::string& renderer, bool heldCurrent,
                             const video::region::HeldReading* held)
    {
        //  Nothing to read until the render is there (§55.5).
        if (rendering)
            return { preparedness::partial, runError::rendering };

        if (missing)
            return { preparedness::partial, runError::mediaMissing };

        //  Nothing reads a picture while no renderer runs: nothing is got ready.
        if (renderer != "running")
            return { renderer == "starting" ? preparedness::preparing : "" };

        if (! heldCurrent)
            return { preparedness::preparing };

        //  Named past the region's room: not read ahead at all.
        if (held == nullptr)
            return { "" };

        switch (held->state)
        {
            case video::region::HeldState::ready:   return { preparedness::armed };
            case video::region::HeldState::failed:  return { preparedness::partial, runError::mediaMissing };
            case video::region::HeldState::none:
            case video::region::HeldState::reading: return { preparedness::preparing };
        }

        return { preparedness::preparing };
    }
}
