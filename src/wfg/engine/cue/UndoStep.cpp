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

#include <wfg/engine/cue/UndoStep.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/cue/LiveRows.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/pair/MachineLocal.h>

namespace wfg::cue
{
    void beginUndoStep (doc::ShowDocument& document, const LiveEdits* live, const Command& command,
                        const Event& submitted, const std::vector<osc::Value>& args, std::int64_t tick)
    {
        /*  A RIDE IS NOT AN EDIT: a fader on a live row writes fifty times a
            second and none of it is a decision about the show, so it opens no
            transaction and Undo never moves a fader (§14.9's reserved domain). */
        if (isLiveWrite (command.name, args)
              || (live != nullptr && isLiveEdit (command.name, args, document, *live)))
            return;

        /*  NOR IS WHAT IS THIS MACHINE'S (namespace draft §58, RD.3, AGC): a
            command that acts on this machine alone, or a value set on its rows
            - the interface, a port's device, a serial path, a display. A backup
            is never sent those, so here they open no step, end no drag, and
            land on no step left open; the show file still keeps the rows that
            are in it. */
        if (pair::isMachineLocal (command, args))
        {
            document.beginMachineEdit();
            return;
        }

        /*  NOR, UNTIL IT WRITES THE SHOW, IS WHAT THE ENGINE SAYS BY ITSELF
            (RD.6): its moment is this machine's - when its media was ready,
            when its interface answered - and a step opened for a record that
            changes no row of the show ended an operator's drag on one machine
            of a pair and not the other. See `ShowDocument::beginEngineEdit`. */
        if (submitted.origin == origin::engine)
        {
            document.beginEngineEdit (command.name, tick, args);
            return;
        }

        document.beginTransaction (command.name, tick, submitted.origin, args);
    }
}
