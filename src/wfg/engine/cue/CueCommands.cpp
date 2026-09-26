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
#include <wfg/engine/cue/LiveEdits.h>
#include <wfg/engine/document/Schema.h>

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

            /*  PARKING THE POINTER UNDOES THE END OF THE LIST, whatever it is
                parked on - including nowhere, which is somebody deliberately
                disarming a list rather than a list that ran out. Written
                unconditionally, because a flag that only went false on some
                parks would be one nobody could reason about. */
            document.setAttribute (finishedAddressOf (listId), "false");

            return edit.ok ? Outcome::ok (args) : Outcome::rejected (edit.reason);
        }
    }

    //==============================================================================
    void registerCueCommands (CommandRegistry& registry, doc::ShowDocument& document,
                              Focus& focus, LiveEdits* live)
    {
        //----------------------------------------------------------------------
        registry.add ({ "standby.set",
                        "Parks the focused list's standby on a cue, including one inside a group."
                        " A cue of a sampler group, a header or a footer parks on the group holding"
                        " it. GO acts on whatever this names.",
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

                            /*  ANYWHERE THE POINTER MAY STAND, which has widened
                                twice. PR 3.4 took it from the list's top level
                                to the manual path, because the pointer descends
                                into a manual sequence group (§3.6). 2026-09-16
                                took it to any enabled cue this list holds, at
                                any depth: the author asked to be able to select
                                a cue within a group and start from that level,
                                so a member of a timeline or an automatic group
                                is a legal place to park too.

                                THIS DOOR IS NOW WIDER THAN THE WALK, on purpose.
                                `standby.next` will not carry the pointer into a
                                timeline group - it steps onto the group's row
                                and GO there fires the scene - but `standby.set`
                                will put it there, and that is the whole of what
                                was asked for. A surface offering "start from
                                this cue" sends this command.

                                The two refusals are told apart because they send
                                somebody somewhere different. `not-in-list` means
                                the cue belongs to another list. `not-a-stop`
                                means it is in THIS list and nothing here can
                                stand for it - a cue in a persistent section, or
                                one somebody switched off. A header's, a footer's
                                or a sampler group's cue has a group that does,
                                and lands there (below). */
                            if (! mayStandOn (list, cueId))
                            {
                                if (! isInList (list, cueId))
                                    return Outcome::rejected (reason::notInList);

                                /*  AND THE GROUP STANDS FOR WHAT IT HOLDS
                                    (author, 2026-09-26): a sampler member, a
                                    header's cue or a footer's cue parks on the
                                    group holding it rather than being refused.
                                    What is left refused is a persistent bed
                                    and a disabled cue - `nearestStop` says why. */
                                const auto group = nearestStop (list, cueId);

                                if (group.empty())
                                    return Outcome::rejected (reason::notAStop);

                                return moveStandbyTo (document, list, group, args);
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
        /*  THE FLAT BUTTON, AND THE DOUBLE-CLICK ON A ROTARY (Phase 9a): every
            one of a media cue's twenty-three EQ rows back to its default, in
            ONE command - so it is one transaction on the show's history and
            Undo takes the whole reset back as one step, where twenty-three
            node.set records from a client would be twenty-three. The defaults
            are the table's own, read off the rows, so this can never disagree
            with what a fresh cue is - every band's switch back on with the
            rest (2026-09-25). */
        registry.add ({ "eq.reset",
                        "Puts a media cue's EQ back to flat: every band in and at nought, both"
                        " filters out, on. One transaction, so Undo takes the whole reset back at"
                        " once.",
                        { { "cue", 's', false } },
                        true,
                        [&document, live] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();
                            const auto cue = document.findById (id);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            if (! cue.hasType ("Media"))
                                return Outcome::rejected (reason::badValue);

                            /*  UNDER THE LOCK, FLAT RIDES LIVE with the rest of
                                the EQ (2026-09-25): each row the show has
                                somewhere else is held at its default in the
                                layer, and each it already has at its default
                                rides nothing. Unlocked, what rode live on this
                                cue is let go first, or it would hide the reset. */
                            if (live != nullptr && document.isLocked())
                            {
                                for (const auto* row : doc::Schema::rowsForOwner ("media"))
                                {
                                    const std::string name { row->name };

                                    if (name.rfind ("eq", 0) != 0)
                                        continue;

                                    const auto saved = document.getAttribute ("/godot/cue/" + id + "/" + name);

                                    if (saved.value_or (std::string {}) == row->defaultText)
                                        live->dropRow (id, name);
                                    else
                                        live->setRow (id, name, std::string (row->defaultText));
                                }

                                return Outcome::ok (args);
                            }

                            for (const auto* row : doc::Schema::rowsForOwner ("media"))
                            {
                                const std::string name { row->name };

                                if (name.rfind ("eq", 0) != 0)
                                    continue;

                                if (live != nullptr)
                                    live->dropRow (id, name);

                                const auto edit = document.setAttribute ("/godot/cue/" + id + "/" + name,
                                                                         std::string (row->defaultText));

                                if (! edit.ok)
                                    return Outcome::rejected (edit.reason);
                            }

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "standby.next",
                        "Moves the focused list's standby to the next cue. A group the machine"
                        " runs is one cue from outside and member by member from inside; at the"
                        " end, and from nowhere, it stays put.",
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
