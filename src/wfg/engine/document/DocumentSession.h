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
    What one session knows about the show's relationship with its disk: which
    folder it came from, and which revision of it that folder holds.

    NOT PART OF THE DOCUMENT, and the reason is the one Bundle.h gives for
    keeping `document.save` apart from the document commands: the document
    holds what someone decided (PRD §4.10), and neither where it lives nor when
    it was last written is a decision about the show. Two sessions opening the
    same bundle hold the same show and different sessions.

    OWNED BY THE VERB, CAPTURED BY REFERENCE. `wfg serve` declares one beside
    its document and hands it to `registerBundleCommands`; the save handler
    writes `savedRevision` and the verb's after-tick reads it. A handler that
    captured it by value would stamp its own copy, the after-tick would compare
    against the original for the life of the process, and the dot would never go
    out - which is the whole difference between a session record that works and
    one that quietly does not (namespace draft §14.10).

    TWO FIELDS, AND THAT IS DELIBERATE. §14.10 draws three more for PR 5.5's
    autosave - `autosavedRevision`, `lastChangeTick`, `lastAutosaveTick` - and
    they arrive with the code that reads them. A field nobody reads yet is a
    field somebody will read wrongly, because the only thing it can hold until
    then is its initialiser, and an initialiser looks exactly like an answer.

    THREADING: the tick thread's, like the document beside it. The save handler
    and the after-tick both run there, which is why neither needs a lock.
*/

#include <wfg/engine/document/ShowDocument.h>

#include <juce_core/juce_core.h>

#include <cstdint>

namespace wfg::doc
{
    struct DocumentSession
    {
        /*  The bundle `document.save` writes to. For `wfg serve` it is the
            folder the show was opened from; for `wfg replay` it is `--out`,
            never the bundle being checked. */
        juce::File folder;

        /*  `ShowDocument::showRevision()` as it stood when the folder last held
            this document's show: stamped once when the verb first knows which
            folder it will write to - at open for `wfg serve`, when `--out` is
            wired for `wfg replay` - and again by every `document.save` that
            reached the disk, and only by one that did.

            Nought is "never", which no document ever reports (its count starts
            at 1), so a session nobody stamped reads as dirty rather than as
            saved. That is the safe way round to be wrong: a false "unsaved"
            costs somebody a save, and a false "saved" costs them a show. */
        std::uint64_t savedRevision = 0;
    };

    /*  `/godot/document/dirty`, in the one place it is defined.

        It asks whether the SHOW HALF has changed since the folder last held
        it, and nothing else - `showRevision()` says why a GO does not count.
        Nor is it a comparison of contents: once undo exists (PR 5.4), an edit
        and its undo will be two changes and will leave this true, because what
        it answers is "is the file on disk this document's history", not "does
        this document differ from the file" (namespace draft §14.10 and
        §14.15). */
    inline bool isDirty (const ShowDocument& document, const DocumentSession& session) noexcept
    {
        return document.showRevision() != session.savedRevision;
    }
}
