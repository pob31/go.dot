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
    What each gesture of the desktop client sends - the Event, whole.

    PRD §4.11: every gesture-reachable action exists as a named command. The
    page keeps that as gestures/commands.json, a table it reads at start; the
    desktop keeps it here for now, as functions that return the Event a gesture
    submits, so that a test can assert the bytes of a gesture with no engine
    and no window (§14.16's "client-side test with no engine"). M5 grows this
    into the one table both clients read; until then it is the seed, and the
    seed is one function.

    Every Event carries origin::window, which is what a log reader needs to
    tell a click from a datagram - and what makes rule 1 checkable: a change
    that reached the show any other way would write no record.
*/

#include <wfg/engine/command/Event.h>

namespace wfg::client::gesture
{
    /** GO: the button, and Space. */
    Event go();
}
