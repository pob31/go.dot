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
    THE SURFACES' RUNTIME STATE: what the machine found about each surface the
    show declares - whether it is being talked to, why not, and the serial it
    gave when it was asked - and, since 2026-09-25, what the surfaces are doing
    with their rotaries: the cue they are aimed at, and the page each shows.

    The `PortTable` shape exactly, and for its reason: the show says "the D700
    on these two ports" (a decision, PRD §4.10), and whether anything answers
    tonight is a fact about this building. So the decision is in the document,
    this is beside it, and the parameter tree publishes both at
    /godot/surface/<id> without either being a copy of the other.

    NAMES NO JUCE TYPE, so the tree, the bridge and a test with no MIDI in the
    room can all hold one.

    THE AIM IS ONE CUE FOR EVERY SURFACE: the cue whose EQ and sends the
    rotaries edit when a page is up (author, 2026-09-25). A SELECT on a sample
    strip sets it, and so does a click on a running cue's name in the window -
    both through `surface.aim`, so it is a named command, logged, and a replay
    reproduces it. It is not the client's pick and not the list GO acts on:
    the cue list's pick moving does not move it. Never stored (PRD §4.10): a
    surface arrives knowing nothing (§4.9).

    A PAGE IS EACH SURFACE'S OWN, and it is not a command: which page a
    controller shows is what its hands are looking at, like the client's
    selection. What a page DOES is ordinary commands - a turn is a node.set -
    so a replay reproduces every effect of one without knowing it was up.
    It is here only so a client can see it: the window opens the foot panel
    on the aimed cue while a surface is adjusting it.

    THREADING: none of its own. The tick thread fills it (the bridge, in the
    after-tick; `surface.aim`, applied on the tick) and the tick thread reads
    it (the tree) - the model's thread, like the port table. The status and
    the aim are read from the tree's cached document half, which every
    applied command rebuilds; a status change that should reach a client asks
    for a rebuild. The pages are read by the runtime half at every publish,
    because they change with no command at all.
*/

#include <map>
#include <string>
#include <utility>

namespace wfg::surface
{
    class SurfaceTable
    {
    public:
        struct Status
        {
            bool connected = false;

            /** Why not, in one sentence; empty when it is, and empty when
                nobody has looked yet. */
            std::string problem;

            /** What the hardware said when it was asked who it is. */
            std::string serial;
        };

        /** Replaces what is known about one surface. Answers whether anything
            a reader could see changed, so the caller knows whether to ask the
            tree for a rebuild. */
        bool set (const std::string& surfaceId, const Status& status)
        {
            auto& held = table[surfaceId];

            const auto changed = held.connected != status.connected
                                   || held.problem != status.problem
                                   || held.serial != status.serial;
            held = status;
            return changed;
        }

        void forget (const std::string& surfaceId)
        {
            table.erase (surfaceId);
            pages.erase (surfaceId);
        }

        /** What is known, or a default Status - not connected, no sentence -
            for a surface nobody has looked at. */
        Status statusOf (const std::string& surfaceId) const
        {
            const auto found = table.find (surfaceId);
            return found != table.end() ? found->second : Status {};
        }

        //==============================================================================
        /** The cue the rotaries edit, or empty. Set by `surface.aim` only. */
        const std::string& aim() const noexcept          { return aimed; }
        void setAim (std::string cueId)                   { aimed = std::move (cueId); }

        //==============================================================================
        /** What one surface's rotaries are showing. */
        struct Page
        {
            /** show, eq or send - the words of surface/page. */
            std::string word = "show";

            /** Which page of that kind, from nought, and how many it has. */
            int index = 0;
            int count = 1;

            /** The address the page last wrote, or empty: what a client reads
                to know the surface is adjusting, and which control it moved. */
            std::string edited;

            bool operator== (const Page&) const = default;
        };

        void setPage (const std::string& surfaceId, const Page& page)  { pages[surfaceId] = page; }

        /** The page a surface shows, or the Show page for one nobody set. */
        Page pageOf (const std::string& surfaceId) const
        {
            const auto found = pages.find (surfaceId);
            return found != pages.end() ? found->second : Page {};
        }

    private:
        std::map<std::string, Status> table;
        std::map<std::string, Page> pages;
        std::string aimed;
    };
}
