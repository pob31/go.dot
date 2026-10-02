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

#include <wfg/client/model/TemplateReview.h>

#include <algorithm>
#include <utility>

namespace wfg::client::model
{
    TemplateReview::TemplateReview (std::vector<TemplateChange> changesToReview)
        : all (std::move (changesToReview))
    {
        for (const auto& change : all)
        {
            const auto on = change.kind == TemplateChange::Kind::added || change.kind == TemplateChange::Kind::changed;
            ticks.push_back (on);
            fieldTicks.emplace_back (change.fields.size(), on);
        }
    }

    bool TemplateReview::isTicked (std::size_t change) const
    {
        return change < ticks.size() && ticks[change];
    }

    bool TemplateReview::isFieldTicked (std::size_t change, std::size_t field) const
    {
        return change < fieldTicks.size() && field < fieldTicks[change].size() && fieldTicks[change][field];
    }

    void TemplateReview::tick (std::size_t change, bool on)
    {
        if (change >= ticks.size())
            return;

        ticks[change] = on;
        std::fill (fieldTicks[change].begin(), fieldTicks[change].end(), on);
    }

    void TemplateReview::tickField (std::size_t change, std::size_t field, bool on)
    {
        if (change >= fieldTicks.size() || field >= fieldTicks[change].size())
            return;

        fieldTicks[change][field] = on;
        ticks[change] = std::any_of (fieldTicks[change].begin(), fieldTicks[change].end(), [] (bool b) { return b; });
    }

    std::vector<TemplatePick> TemplateReview::picks() const
    {
        std::vector<TemplatePick> picked;

        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (! ticks[i])
                continue;

            TemplatePick pick { all[i].id, {} };

            if (all[i].kind == TemplateChange::Kind::changed)
                for (std::size_t f = 0; f < all[i].fields.size(); ++f)
                    if (fieldTicks[i][f])
                        pick.fields.push_back (all[i].fields[f].name);

            picked.push_back (std::move (pick));
        }

        return picked;
    }

    std::vector<std::string> TemplateReview::localSounds() const
    {
        std::vector<std::string> sounds;

        for (std::size_t i = 0; i < all.size(); ++i)
        {
            if (! ticks[i])
                continue;

            //  A changed cue brings its sound only when its file is one of the fields ticked.
            if (all[i].kind == TemplateChange::Kind::changed)
            {
                bool fileTicked = false;

                for (std::size_t f = 0; f < all[i].fields.size(); ++f)
                    fileTicked = fileTicked || (fieldTicks[i][f] && all[i].fields[f].name == "file");

                if (! fileTicked)
                    continue;
            }

            for (const auto& sound : all[i].localSounds)
                if (std::find (sounds.begin(), sounds.end(), sound) == sounds.end())
                    sounds.push_back (sound);
        }

        return sounds;
    }
}
