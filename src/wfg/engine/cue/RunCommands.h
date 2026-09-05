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
    A run's whole life, as commands.

    THE RULE THESE EXIST TO KEEP, and it is the one that makes a log worth
    keeping: state transitions are events, continuous readouts are not.
    Everything the tick thread learns that a decision depends on - a track was
    reserved, the sound started, it ended, it never started and here is why -
    arrives as a LOGGED command applied on the tick it was observed. Position
    and level are readouts; they are published for a client to watch and never
    written to the log.

    WHY THE MACHINE'S OWN REPORTS GO THROUGH THE REGISTRY. PRD §3.15 admits no
    second path into engine state, and §4.11 says every gesture-reachable action
    exists as a named command. Holding the machine to the same rule is what
    makes a session reproducible: the audio side runs on the message thread and
    the model belongs to the tick thread, so a report that reached in directly
    would make the model depend on WHEN the message thread happened to run.
    Going through the registry means it lands on a tick, in arrival order, with
    its arguments in the log - and `wfg replay` re-injects it at the same tick
    on a machine with no sound card and gets the same tree.

    ANYONE MAY SEND THEM, deliberately. A command only the inside of the process
    could send would be a command a replay could not send, which would make the
    one guarantee they exist to provide impossible to test.

    IDEMPOTENT WHERE IT COSTS NOTHING. `run.started` on a run already playing is
    applied and changes nothing; so is `run.ended` on a run already done. A log
    replayed twice converges, and a report that arrived late does not undo one
    that arrived on time.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>

namespace wfg::cue
{
    /*  Registers the run lifecycle against `runs`.

        `document` is read to find the cue an arm names and to learn its kind;
        `ids` draws a run's identifier, and the drawn value is written back into
        the log as the argument the caller left out, so a replay re-supplies it
        rather than drawing again.

        None of the three references may outlive the registry.
    */
    void registerRunCommands (CommandRegistry& registry,
                              const doc::ShowDocument& document,
                              RunTable& runs,
                              doc::IdRegistry& ids);
}
