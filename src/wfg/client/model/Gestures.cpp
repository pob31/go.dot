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

#include <wfg/client/model/Gestures.h>

#include <string>

namespace wfg::client::gesture
{
    namespace
    {
        Event plain (const char* command)
        {
            return { origin::window, command, {} };
        }
    }

    Event go()   { return plain ("go"); }

    Event standbyNext()     { return plain ("standby.next"); }
    Event standbyPrevious() { return plain ("standby.previous"); }

    Event park (const std::string& cueId)
    {
        return { origin::window, "standby.set", { osc::Value::string (cueId) } };
    }

    /*  NO ARGUMENTS, though both take an optional domain: one domain exists
        (`document`), and naming it would be this client deciding what the
        default is rather than the engine. When Phase 6 adds the parameter
        domain, the window will have somewhere to say which - and until it
        does, saying nothing is the honest way to mean "the one there is". */
    Event undo() { return plain ("undo"); }
    Event redo() { return plain ("redo"); }

    Event save()            { return plain ("document.save"); }
    Event revert()          { return plain ("document.revert"); }
    Event recover()         { return plain ("document.recover"); }
    Event discardRecovery() { return plain ("document.discardRecovery"); }

    Event kill (const std::string& runId)
    {
        return { origin::window, "run.kill", { osc::Value::string (runId) } };
    }

    Event setLocked (bool locked)
    {
        /*  A BOOLEAN, not the word "true": `node.set` takes its value as a
            wildcard and the row is a `T`, so the value carries its own type
            and nothing has to parse a string back into one. The page sends
            the same two OSC tags. */
        return { origin::window, "node.set",
                 { osc::Value::string ("/godot/document/locked"), osc::Value::boolean (locked) } };
    }
}
