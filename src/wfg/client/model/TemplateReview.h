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
    WHAT GOES BACK INTO THE SHOW'S TEMPLATE, as ticks (namespace draft §25).

    A performance's changes against its show's template, each with a tick,
    and a changed cue's fields each with their own (the author: "only what I
    pick"). Ticked to start with: what was added and what was changed - the
    work of the night someone will want again. Not ticked: what was removed,
    since taking it out of the template takes it out of every performance to
    come, and the show-wide settings, which are usually the venue's.

    STD ONLY, as the model half is, so a test reads it with no window.
*/

#include <wfg/engine/TemplateChanges.h>

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::client::model
{
    class TemplateReview
    {
    public:
        explicit TemplateReview (std::vector<TemplateChange> changesToReview);

        const std::vector<TemplateChange>& changes() const noexcept { return all; }

        bool isTicked (std::size_t change) const;
        bool isFieldTicked (std::size_t change, std::size_t field) const;

        /*  A change, and for a changed cue every one of its fields with it. */
        void tick (std::size_t change, bool on);

        /*  One field; the cue is ticked while any of its fields is. */
        void tickField (std::size_t change, std::size_t field, bool on);

        /*  What the update is handed: every ticked change, a changed cue with
            the fields ticked. */
        std::vector<TemplatePick> picks() const;

        /*  The sounds only the performance has that the ticked changes would
            bring - what the window asks about before it updates. */
        std::vector<std::string> localSounds() const;

    private:
        std::vector<TemplateChange> all;
        std::vector<bool> ticks;
        std::vector<std::vector<bool>> fieldTicks;
    };
}
