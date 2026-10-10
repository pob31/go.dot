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

#include "PlayedMedia.h"

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  What may hold sections: a sound, or a Video whose source is a
            movie (namespace draft §55.5) - a still has no time to cut. */
        bool isSoundOrMovie (const juce::ValueTree& cue)
        {
            return cue.isValid()
                && (cue.hasType ("Media")
                    || (cue.hasType ("Video") && cue["source"].toString() == "movie"));
        }

        std::optional<double> lengthNamed (const std::map<std::string, double>* durations, const std::string& name)
        {
            if (durations == nullptr || name.empty())
                return std::nullopt;

            const auto found = durations->find (name);

            if (found == durations->end() || ! (found->second > 0.0))
                return std::nullopt;

            return found->second;
        }
    }

    std::vector<doc::Section> sectionsIn (const juce::ValueTree& cue)
    {
        std::vector<doc::Section> sections;

        if (! isSoundOrMovie (cue))
            return sections;

        for (const auto& child : cue)
        {
            if (! child.hasType ("Section"))
                continue;

            sections.push_back (doc::sectionFromNode (child));
        }

        return sections;
    }

    std::string editTextOf (const juce::ValueTree& cue)
    {
        return doc::editText (sectionsIn (cue));
    }

    std::optional<double> editedLengthOf (const juce::ValueTree& cue)
    {
        if (! isSoundOrMovie (cue) || ! cue["editSource"].toString().isEmpty())
            return std::nullopt;

        const auto sections = sectionsIn (cue);

        if (sections.empty())
            return std::nullopt;

        return doc::editedLength (sections);
    }

    PlayedMedia playedMediaOf (const juce::ValueTree& cue,
                               const std::map<std::string, double>* durations,
                               const audio::EditRenders* renders)
    {
        PlayedMedia played;

        if (! cue.isValid())
            return played;

        played.name = cue["file"].toString().toStdString();
        played.frozen = isSoundOrMovie (cue) && ! cue["editSource"].toString().isEmpty();

        const auto fileLength = lengthNamed (durations, played.name);
        const auto sections = sectionsIn (cue);

        /*  NO EDIT, A FROZEN ONE, OR ONE THAT IS THE WHOLE FILE AS RECORDED:
            the file, as long as the show knows it to be. */
        if (sections.empty() || played.frozen
              || (fileLength.has_value() && doc::isIdentityEdit (sections, *fileLength)))
        {
            played.lengthKnown = fileLength.has_value();
            played.lengthSeconds = fileLength.value_or (0.0);
            return played;
        }

        /*  AN OPEN EDIT: the render of the edit as it now is, when the
            renderer has made it; as long as the sections put together, whatever
            the render says. */
        played.openEdit = true;
        played.lengthKnown = true;
        played.lengthSeconds = doc::editedLength (sections);
        played.name.clear();

        if (renders != nullptr)
        {
            const auto found = renders->find (cue[idProperty].toString().toStdString());

            if (found != renders->end() && found->second.state == audio::renderState::done
                  && found->second.editText == doc::editText (sections))
                played.name = found->second.file;
        }

        return played;
    }

    double fileSecondOf (const juce::ValueTree& cue, double editedSecond)
    {
        if (! editedLengthOf (cue).has_value())
            return editedSecond;

        const auto sections = sectionsIn (cue);

        if (const auto place = doc::placeOf (sections, editedSecond))
            return place->fileSecond;

        return editedSecond < 0.0 ? sections.front().in : sections.back().out;
    }

    std::optional<double> playedLengthOf (const juce::ValueTree& cue, const std::map<std::string, double>* durations)
    {
        const auto played = playedMediaOf (cue, durations, nullptr);

        if (! played.lengthKnown)
            return std::nullopt;

        return played.lengthSeconds;
    }
}
