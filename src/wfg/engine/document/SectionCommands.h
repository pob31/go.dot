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

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>

#include <functional>
#include <string>

/*  THE COMMANDS OF A SOUND'S EDIT, AND A MOVIE'S (namespace draft §55.3, 55.5): the sections cut,
    joined, trimmed, moved, removed and cleared, and the edit frozen and
    unfrozen as document edits. Each is a thin name over a `ShowDocument`
    method, which is where the refusals and the carry live, so `node.set`,
    `object.move`, `object.delete` and these say the same thing.

    Every one is a document edit, logged and replayed - unlike `media.freeze`,
    the renderer's verb that asks for the bounce and submits `media.frozen`
    once the file is there.
*/
namespace wfg::doc
{
    /*  WHAT THE SESSION KNOWS OF A FILE, by the name the show gives it: its
        length in seconds (what the first split of a cue with no sections
        makes its whole-file section from), and for a movie its frame rate
        (what a cut snaps to, 55.5 ADT) and its codec (Hap1, Hap5 or HapY, or
        the verbs refuse it, ADX). Nought and empty when not known. A session
        hands lambdas over its media table; a replay hands nothing and reads
        the length and the rate off the record. */
    struct MediaFacts
    {
        std::function<double (const std::string& file)> lengthOf;
        std::function<double (const std::string& file)> frameRateOf;
        std::function<std::string (const std::string& file)> codecOf;
    };

    void registerSectionCommands (CommandRegistry& registry, ShowDocument& document,
                                  MediaFacts facts = {});
}
