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
    WHAT A PERFORMANCE HAS THAT ITS SHOW'S TEMPLATE HAS NOT, in plain values
    (namespace draft §25).

    A Show is a container - the media every performance shares, the
    performances, and, if someone made one, a template cue list: the show
    folder's own .wfg. A Performance is a cue list with the media recorded at
    it. What changed in a performance can be brought back into the template,
    cue by cue and field by field, when the performance is closed or when it
    is asked for (author, 2026-10-02). These are the changes as the window
    lists them and the picks it hands back - STD ONLY, as Console.h is, so the
    client holds them without naming the document's types
    (scripts/check-client-boundary.py).

    The work is document/Template.h's.
*/

#include <string>
#include <vector>

namespace wfg
{
    /*  One field of a cue that differs: an attribute by its name ("level"),
        a kind of child element taken whole ("child:Route", every route the
        cue has), or "position" - its place in the list. */
    struct TemplateField
    {
        std::string name;
        std::string label;      ///< what the window says: "level", "routing", "place in the list"
    };

    struct TemplateChange
    {
        enum class Kind
        {
            added,      // in the performance and not the template: a cue, a section, a list
            changed,    // in both, with fields that differ
            removed,    // in the template and deleted in the performance
            settings    // a show-wide setting: outputs, devices, surfaces...
        };

        Kind kind = Kind::changed;

        /*  The element's identifier - for a setting, its container's name
            ("Audio", "Mounts"), which a show has one of. */
        std::string id;

        std::string label;                    ///< "2 After the scene", "Audio and outputs"
        std::vector<TemplateField> fields;    ///< `changed` only

        /*  Sounds a picked change would bring that only the performance has -
            in its own media/, not the show's. Asked about each time (the
            author's choice): copy them into the show's media/, or send the
            cues without them. Relative to media/, as a cue names them. */
        std::vector<std::string> localSounds;
    };

    struct TemplateComparison
    {
        bool ok = false;
        std::string problem;                  ///< why there is no comparison, when not ok
        std::string showFolder;               ///< the folder around the performance
        bool hasTemplate = false;             ///< whether that folder holds a template cue list
        std::vector<TemplateChange> changes;
    };

    /*  A change the window brings back: by its id, and for a `changed` one the
        fields ticked - all of them when the list is empty. */
    struct TemplatePick
    {
        std::string id;
        std::vector<std::string> fields;
    };

    struct TemplateUpdate
    {
        bool ok = false;
        std::string said;                     ///< a sentence for the foot, success or not
    };
}
