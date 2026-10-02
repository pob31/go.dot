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
    A PERFORMANCE AND ITS SHOW'S TEMPLATE (namespace draft §25, stage 3).

        Hamlet/                     the show: a container
          media/                    the sounds every performance shares
          Hamlet.wfg  show.xml ...  the template cue list - optional
          2026-10-03 Paris/         a performance: its cue list, its recordings
          2026-11-12 Lyon/

    A document is a performance when the folder around it holds a .wfg or a
    media/ - the same "around" a sound is found in (audio/MediaInfo.h). The
    show has a template when its folder holds a .wfg.

    BOTH DOCUMENTS ARE READ FROM THE DISK, by Bundle::open, into documents of
    this call's own: nothing here touches a live show or any thread but the
    caller's, so the window and `wfg template` share it. What a performance
    has not yet saved is not in the comparison; the window offers to save
    first.

    MATCHED BY IDENTIFIER. A cue keeps its id from the template into every
    performance copied from it, and back - so a cue brought into the template
    is the same cue the next comparison finds, and finds unchanged.
*/

#include <wfg/engine/TemplateChanges.h>

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace wfg::doc::Template
{
    /*  The folder around `document` when it is a show's - it holds a .wfg or
        a media/ - and an empty File when `document` is not a performance. */
    juce::File showFolderOf (const juce::File& document);

    /*  Whether that folder holds a template cue list: a .wfg of its own. */
    bool hasTemplate (const juce::File& showFolder);

    /*  What `performance` has that the template has not, cue by cue: added,
        changed (each field that differs), removed, and the show-wide
        settings. Not ok when `performance` is no performance, a document will
        not open, or there is no template - `hasTemplate` says which. */
    TemplateComparison compare (const juce::File& performance);

    /*  Brings `picks` into the template, and writes it.

        Removed cues go first, then settings, then added cues in the
        performance's order - each after its neighbour there, matched by id,
        or at the end of its parent when the neighbour is not in the template -
        then the picked fields of changed cues. Ids are kept. `copySounds`
        copies each picked change's local sounds into the show's media/; else
        the cues name sounds the template's performances will report missing
        until someone puts them there.

        REFUSED, and nothing written, when the template is open in another
        Go.dot window (app/OpenShows.h) - a second writer would lose one of
        the two - or when the result does not validate. */
    TemplateUpdate update (const juce::File& performance, const std::vector<TemplatePick>& picks,
                           bool copySounds);

    /*  A show with no template gets one: `performance`'s cue list and
        settings, written into the show folder as its own .wfg. Refused when
        there is one already. */
    TemplateUpdate makeTemplate (const juce::File& performance);

    /*  One line per change, for `wfg template diff`: "+ added", "~ changed:
        fields", "- removed", "= settings". */
    std::vector<std::string> describe (const TemplateComparison& comparison);
}
