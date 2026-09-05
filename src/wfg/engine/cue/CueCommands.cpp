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

#include <wfg/engine/cue/CueCommands.h>

#include <string>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };
        const juce::Identifier standbyProperty { "standby" };

        std::string standbyOf (const juce::ValueTree& list)
        {
            return list[standbyProperty].toString().toStdString();
        }

        /*  Moves the focused list's standby to `wanted`.

            Everything goes through ShowDocument::setAttribute, which is the
            document's single write path and where the referential invariant
            lives - so a command cannot put the standby somewhere a client's
            node.set could not. */
        Outcome moveStandbyTo (doc::ShowDocument& document, const juce::ValueTree& list,
                               const std::string& wanted, const std::vector<osc::Value>& args)
        {
            const auto listId = list[idProperty].toString().toStdString();
            const auto edit = document.setAttribute (standbyAddressOf (listId), wanted);

            return edit.ok ? Outcome::ok (args) : Outcome::rejected (edit.reason);
        }
    }

    //==============================================================================
    void registerCueCommands (CommandRegistry& registry, doc::ShowDocument& document,
                              Focus& focus)
    {
        //----------------------------------------------------------------------
        registry.add ({ "standby.set",
                        "Parks the focused list's standby on a cue. GO acts on whatever this"
                        " names.",
                        { { "cue", 's', false } },
                        true,
                        [&document, &focus] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            const auto& cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            /*  Told apart on purpose. A cue that does not exist
                                is a different mistake from one that exists in
                                another list or inside a group, and an operator
                                reading the log at 2 a.m. wants to know which. */
                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto element = cue.getType().toString();

                            if (element != "Cue" && element != "Group")
                                return Outcome::rejected (reason::unknownId);

                            if (! isTopLevelChild (list, cueId))
                                return Outcome::rejected (reason::notInList);

                            return moveStandbyTo (document, list, cueId, args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "standby.clear",
                        "Parks the focused list's standby nowhere. An empty standby is a resting"
                        " state, not a failure.",
                        {},
                        true,
                        [&document, &focus] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            return moveStandbyTo (document, list, {}, args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "standby.next",
                        "Moves the focused list's standby to the next cue. A group is one cue;"
                        " at the end, and from nowhere, it stays put.",
                        {},
                        true,
                        [&document, &focus] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            /*  Applied even when it does not move. There is a
                                list and the command did what it does; having
                                nowhere to go is an answer, not a refusal, and
                                a log full of R records every time an operator
                                reaches the end of a list would bury the
                                rejections that matter. */
                            return moveStandbyTo (document, list,
                                                  nextOf (list, standbyOf (list)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "standby.previous",
                        "Moves the focused list's standby to the previous cue. At the start, and"
                        " from nowhere, it stays put.",
                        {},
                        true,
                        [&document, &focus] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            return moveStandbyTo (document, list,
                                                  previousOf (list, standbyOf (list)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "list.focus",
                        "Chooses the list the standby commands act on. Exactly one, by"
                        " construction: it is one value.",
                        { { "list", 's', false } },
                        true,
                        [&document, &focus] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            /*  Exclusive without an exclusivity rule, because
                                there is one value rather than a flag per list.
                                Nothing has to be un-focused and nothing can end
                                up with two. */
                            if (! focus.request (document, args[0].getString()))
                                return Outcome::rejected (reason::unknownId);

                            return Outcome::ok (args);
                        } });
    }
}
