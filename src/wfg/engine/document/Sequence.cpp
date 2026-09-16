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

#include <wfg/engine/document/Sequence.h>

namespace wfg::doc
{
    namespace
    {
        /*  The pooled identifier, rather than asking with a literal each time:
            `hasProperty ("id")` builds a juce::Identifier, and this is called
            once per child of every container the tree publishes. */
        const juce::Identifier idProperty { "id" };
    }

    bool isSequenceChild (const juce::ValueTree& child)
    {
        /*  An unidentified child is nothing a client can name, so it cannot be
            a member of anything and cannot hold a position. Today that is the
            containers — <Lists>, <Mounts>, <Rack> — which carry what a
            collection holds and nothing of their own. */
        if (! child.hasProperty (idProperty))
            return false;

        const auto element = child.getType().toString();

        return element != "Header" && element != "Footer" && element != "Persistent";
    }

    int rawIndexForPosition (const juce::ValueTree& parent, int position)
    {
        const auto numChildren = parent.getNumChildren();
        auto remaining = position;

        for (int raw = 0; raw < numChildren; ++raw)
        {
            if (! isSequenceChild (parent.getChild (raw)))
                continue;

            if (remaining <= 0)
                return raw;

            --remaining;
        }

        /*  PAST THE LAST MEMBER IS PAST THE LAST CHILD, and not "just after the
            last member", which would have been the tidier-looking answer. The
            two put a new member in the same place in `order`; they differ only
            in where a <Footer> ends up in the file, and a footer is already
            wherever it was created — `createRole` appends it, so a group that
            got its footer after its cues has carried it last since long before
            this file existed. The simple rule is the one that stays true. */
        return numChildren;
    }
}
