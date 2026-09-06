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

                            /*  IS IT A CUE. Asked of the parameter table's
                                owner word rather than of a list of element
                                names, because that list has grown twice already
                                and both times this line was not updated with
                                it: Phase 2 added Media, Fade, Stop and Osc, and
                                every one of them was refused here - as
                                `unknown-id`, of a cue the engine had just found
                                - while the SAME write through node.set was
                                accepted, because the document's own door asks
                                only whether the identifier names a child.

                                Nothing caught it because no fixture parks
                                standby on anything but a memo or a group: a
                                show that plays restores its standby from
                                state.xml, which does not take this path. It
                                would have been found by the first person to
                                click a media cue in a UI.

                                `ownerForElement` is the question actually being
                                asked - a Group is a Cue (§3.6) and so is a
                                Media - and it is the same answer the address
                                resolver gives, so a cue that can be addressed
                                at /godot/cue/<id> can be parked on. */
                            if (doc::ShowDocument::ownerForElement (
                                    cue.getType().toString().toStdString()) != "cue")
                                return Outcome::rejected (reason::unknownId);

                            /*  THE MANUAL PATH, not the top level, since PR
                                3.4 - the pointer descends into a manual
                                sequence group (§3.6), so a member of one is a
                                legal place to park.

                                The two refusals are told apart because they
                                send somebody somewhere different. `not-in-list`
                                means the cue belongs to another list.
                                `not-manual-path` means it is in THIS list and
                                is one the machine advances - inside an
                                automatic chain, or in a header - and the remedy
                                is to make the group manual or to park on the
                                group instead. */
                            if (! isOnManualPath (list, cueId))
                            {
                                const auto elsewhere = ! isInList (list, cueId);

                                return Outcome::rejected (elsewhere ? reason::notInList
                                                                    : reason::notManualPath);
                            }

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
                                                  nextStandby (list, standbyOf (list)), args);
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
                                                  previousStandby (list, standbyOf (list)), args);
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
