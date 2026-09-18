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
    Bringing media into a show: the part of it that is not file handling.

    WHY THIS EXISTS AT ALL, and it is the reason this client is compiled rather
    than served. A browser is never told a dropped file's path - it is given
    the name and the bytes and nothing else, deliberately and by design - so a
    web client cannot express *use the file where it sits*. That is decision Y
    (§14.16), and it is the one capability the page cannot be given later: not
    a hole the engine must open for a second client, but something the FIRST
    client could never have.

    WHAT AN IMPORT IS, in three parts, and only the last of them is the show's:

      1. the bytes arrive in `<bundle>/media/` - a fact about a disk, like the
         timbre cache the analyser writes with no command and no record
      2. a cue is created, which IS a decision and goes through `cue.create`
      3. the cue names the file, which is the decision that matters (§4.10),
         and goes through `node.set` like any other value

    §14.16 argues 1 explicitly: copying media into the bundle is not a change
    to the show, so the client may do it itself and then send one ordinary
    command for the cue to name it. What it may never do is reach past the
    door for 2 and 3.

    AND THE CLIENT DOES NOT DRAW IDENTIFIERS. `cue.create` takes an optional
    id, and that argument is for REPLAY - the engine draws one, the log records
    the call with it, and a replay supplies it rather than drawing again. There
    is exactly one entropy consumer in this project and a window is not going
    to become the second, so a create is followed by finding what it made.

    std only, like the rest of model/: the file handling is the window's, and
    everything here can be asserted without one.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::client::model
{
    /*  What to call a cue made from a file. The extension goes and nothing
        else does: a name is the operator's, and a client that tidied
        underscores or stripped track numbers would be guessing at what
        somebody meant to call their own material. */
    std::string cueNameFor (const std::string& fileName);

    /** The name a cue's `file` attribute carries: relative to the bundle's `media/`. */
    std::string mediaNameFor (const std::string& fileName);

    /*  WHICH CUE A CREATE JUST MADE. `cue.create` answers on the tick thread
        and a client sees the result only in the next published tree, so an
        import that must then name a file has to find what it asked for. The
        index it gave is a MEMBER position and `order` lists members, so the
        cue at that position is the one - which is deterministic, and needs no
        diffing of before and after.

        Empty when the order cannot answer, which a caller must treat as "do
        not write anything": attaching a file to the wrong cue is worse than
        attaching it to none. */
    std::string createdAt (const std::string& orderText, int index);

    /*  A PENDING IMPORT: a file copied in, a cue asked for, and the naming
        still to do. Held by the window between one pass and the next. */
    struct Import
    {
        std::string parent;      ///< the list or group the cue was created in
        int index = 0;           ///< the member position it was asked for
        std::string cueName;     ///< what the create was told to call it
        std::string mediaName;   ///< what the cue's `file` should say
        std::uint64_t askedAt = 0;   ///< the revision when the create was sent
        int waited = 0;          ///< passes since, so a refusal is not waited on for ever
    };

    /*  WHETHER THE CUE STANDING AT THE ASKED-FOR POSITION IS THE ONE THE
        CREATE MADE, which has to be asked because the position alone does not
        answer it: the show has other clients, and somebody inserting a cue
        from the page in the same two hundred milliseconds would put a
        stranger exactly where this import is looking.

        Three things must agree - the name the create was given, a media cue,
        and no file yet - and a stranger passing all three is a media cue
        somebody called the same thing and left empty, where naming it is what
        was wanted anyway. Attaching a file to the wrong cue is worse than
        attaching it to none, so a caller writes nothing when this is false. */
    bool madeByImport (const Import& job, const std::string& kind,
                       const std::string& name, const std::string& file);

    /** How many passes an import waits for its cue before it is given up on. */
    inline constexpr int importPatience = 50;   // two seconds at 25 Hz
}
