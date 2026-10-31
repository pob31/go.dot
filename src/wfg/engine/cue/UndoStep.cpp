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
#include <wfg/engine/tree/Touches.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        bool isHold (const std::string& name)
        {
            return name == "node.touch" || name == "node.release" || name == "node.releaseAll";
        }

        /*  WHETHER A WRITE IS HELD: its origin holds every address it writes
            - one for `node.set`, each address of a `node.setMany` (its
            arguments address, value, address, value...). A write with no
            address is never held. */
        bool isHeldWrite (const tree::TouchTable& touches, const std::string& holder,
                          const std::string& name, const std::vector<osc::Value>& args)
        {
            if (name == "node.set")
                return ! args.empty() && args[0].isString() && touches.isHeld (holder, args[0].getString());

            if (name != "node.setMany" || args.empty())
                return false;

            for (std::size_t at = 0; at < args.size(); at += 2)
                if (! args[at].isString() || ! touches.isHeld (holder, args[at].getString()))
                    return false;

            return true;
        }

        /*  WHETHER A HOLD IS THE ORIGIN'S LAST LET-GO: a release of the one
            address it still holds (or of nothing), or everything let go. */
        bool isLastLetGo (const tree::TouchTable& touches, const std::string& holder,
                          const std::string& name, const std::vector<osc::Value>& args)
        {
            if (name == "node.releaseAll")
                return true;

            if (name != "node.release")
                return false;

            const auto held = touches.heldBy (holder);

            if (held.empty())
                return true;

            return held.size() == 1 && ! args.empty() && args[0].isString() && held.front() == args[0].getString();
        }
    }

    void beginUndoStep (doc::ShowDocument& document, const LiveEdits* live, const Command& command,
                        const Event& submitted, const std::vector<osc::Value>& args, std::int64_t tick,
                        const tree::TouchTable* touches)
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

        /*  WHO HOLDS IS THE CLIENT, as the touch table keys it: a record a
            backup applies for its main's client is held by that client. */
        const auto holder = std::string (origin::localClientOf (submitted.origin));

        /*  A HOLD IS NOT AN EDIT (namespace draft §125, BFC): it changes who
            holds a node, never a row of the show, and inside a run of held
            writes it must not cut the run - a second hand landing, or one of
            two lifting, is the middle of one gesture. */
        if (touches != nullptr && isHold (command.name))
        {
            document.noteHold (submitted.origin, isLastLetGo (*touches, holder, command.name, args));
            return;
        }

        const auto held = touches != nullptr && isHeldWrite (*touches, holder, command.name, args);
        document.beginTransaction (command.name, tick, submitted.origin, args, held);
    }
}
