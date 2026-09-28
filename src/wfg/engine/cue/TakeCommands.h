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
    A SAMPLING CHANNEL'S TAKE, AS COMMANDS (Phase 9c, stage 9c.3, namespace
    draft §19.6).

    `take.record|loop|overdub|undo|clear <channel s>` - the presses of §19.3's
    table, a hand's, a script's and a surface's alike; a transport cue says the
    first four through the Runner, which moves the same account the same way.
    Each moves `TakeTable` at once and asks for the press, which the Runner's
    hook places at `now + the launch latency` so it lands on a sample the log
    can name.

    `take.closed <channel s> <seconds d> <how s>` - the engine's: the length
    the audio thread closed a take at, and how - `pressed`, `full` when it
    filled its memory, `held` when its cue let go - so a replay knows it. In
    seconds, where the drawing said samples: the account needs no rate that
    way, and a replay has no audio side to ask one of.

    REFUSED FROM THE ACCOUNT, THE DOCUMENT AND THE RUN TABLE ALONE, never from
    the audio side (namespace draft §7): `unknown-id` for a channel the show
    does not declare; `bad-value` for one with no recorder; `not-running` for
    Rec, Loop or a layer with no sounding mic cue holding the channel - a cue
    armed ahead, or ringing out after a stop, is not sounding; `layers-full`
    for a layer with every one in use. Undo and Clear need no cue.

    KEEP (stage 9c.6, §19.8). `take.keep <channel s> [asCue T] [after s]` makes
    the closed take a file under the show's media/takes, and with `asCue` a
    media cue after the one given - or the one sounding on the channel - that
    loops it between the take's points. Refused `not-closed` for a take empty
    or still recording, `busy` while a Keep is still writing, and `locked`
    with `asCue` under the lock; Undo and Clear are refused `busy` too, until
    the file is whole. `take.kept <channel s> <file s> <error s> [cue s] [range
    s] [route s]` is the engine's: the name the writer found on the disk, or
    why it wrote nothing - and for `asCue` the cue, range and route it made,
    whose identifiers the record carries so a replay makes the same ones.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/TakeTable.h>
#include <wfg/engine/document/ShowDocument.h>

#include <string>

namespace wfg::cue
{
    /*  WHAT A CHANNEL SAYS ABOUT ITS RECORDER, through the schema's defaults:
        whether it is a sampling channel at all, its longest take and how many
        layers it keeps. Nothing for an identifier that is not a rack channel. */
    struct SamplingChannel
    {
        bool declared = false;          ///< a rack channel by that id
        std::string name;               ///< what the show calls it, which a kept take's file is called after
        double takeSeconds = 0.0;
        int layers = 4;

        bool samples() const noexcept   { return declared && takeSeconds > 0.0; }
    };

    SamplingChannel samplingChannelOf (const doc::ShowDocument&, const std::string& channelId);

    /*  Whether a sounding mic cue holds that channel - what Rec, Loop and a
        layer need, and what a transport cue's press is aimed through. */
    bool isSoundingOn (const RunTable&, const std::string& channelId);

    /** The verb's word, as the command and the transport cue say it. */
    bool takeVerbOf (const std::string& word, TakeVerb& verb);

    /*  Registers the five presses and `take.closed`. `takes`, `runs` and
        `document` may not outlive the registry. */
    void registerTakeCommands (CommandRegistry& registry, TakeTable& takes, RunTable& runs,
                               doc::ShowDocument& document);
}
