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
    A show on disk: a folder, not a file.

        MyShow/
          MyShow.wfg      the manifest - the file you double-click
          show.xml        what someone decided (PRD §4.10)
          state.xml       where the engine had got to
          namespaces/     the OSCQuery descriptions the mounts read

    A FOLDER RATHER THAN AN ARCHIVE, because everything in it is text that
    someone will eventually want to diff, grep, or put under version control -
    and because a show that has crashed mid-write should lose one file, not all
    of them. It follows WFS-DIY's project folder, which works.

    NOTHING IN A BUNDLE RECORDS WHEN OR WHERE IT WAS WRITTEN. No timestamp, no
    writer version, no machine name. That is what makes `open` then `save`
    byte-identical, which in turn is what lets a replay compare its result
    against the saved bundle directly instead of through a normaliser that
    strips the parts that were always going to differ. spatcore's XmlPersistence
    writes a `<!-- Created: -->` header and its harness has to strip it; this
    does not have the problem to solve.

    THE MANIFEST CARRIES A VERSION AND NOTHING ELSE. It exists so that a
    double-click, a file association and a recent-documents list have something
    to point at, and so a folder can say what it is without being opened. Any
    other content would be a second place to look for something show.xml
    already says.
*/

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/ShowDocument.h>

#include <juce_core/juce_core.h>

namespace wfg::doc
{
    namespace Bundle
    {
        /** `<folder>/<folder name>.wfg`. */
        juce::File manifestFile (const juce::File& folder);
        juce::File showFile (const juce::File& folder);
        juce::File stateFile (const juce::File& folder);
        juce::File namespacesFolder (const juce::File& folder);

        /*  Reads a bundle into `document`.

            `ok` means THE SHOW LOADED and the document is usable. `problems`
            can be non-empty even so, and the two are not the same question:
            a missing state.xml is silent, a stale entry in one is a problem
            worth printing that costs nobody their show, and an unreadable
            show.xml is neither - it sets ok false and the document is left
            exactly as it was.

            The asymmetry is deliberate and it is the whole design of this
            layer: refuse what risks someone's work, report what does not. */
        ReadResult open (const juce::File& folder, ShowDocument& document);

        /*  Writes the manifest, show.xml and state.xml, creating the folder if
            it is not there.

            It does NOT touch `namespaces/`: those files describe other people's
            programs, Go.dot only reads them, and a save that rewrote them would
            be claiming an authorship it does not have. A bundle opened and
            saved keeps whatever was in that folder, untouched. */
        ReadResult save (const juce::File& folder, const ShowDocument& document);
    }
}
