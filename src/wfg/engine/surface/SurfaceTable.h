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

            /** A Stream Deck's model, as the show spells one ("mk2", "plus"),
                and empty for anything else or a deck not found (§95). */
            std::string model {};
        };

        /** Replaces what is known about one surface. Answers whether anything
            a reader could see changed, so the caller knows whether to ask the
            tree for a rebuild. */
        bool set (const std::string& surfaceId, const Status& status)
        {
            auto& held = table[surfaceId];

            const auto changed = held.connected != status.connected
                                   || held.problem != status.problem
                                   || held.serial != status.serial
                                   || held.model != status.model;
            held = status;
            return changed;
        }

        void forget (const std::string& surfaceId)
        {
            table.erase (surfaceId);
            pages.erase (surfaceId);
            keys.erase (surfaceId);
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
        /*  WHAT THE MASTER DIAL TURNS (author, 2026-09-26): the number last
            clicked or touched in the window's inspector or foot panel, by its
            address, with what its row says about how it moves - carried here
            so the bridge turns it without asking the document on the tick.
            One for every surface, like the aim; set by `surface.dial` only,
            and it stays on that cue's row until another field is clicked or
            the dial's click lets go. Never stored (PRD §4.10). */
        struct Dial
        {
            std::string address;            ///< empty: the dial is free
            bool integer = false;
            bool hasMinimum = false;
            double minimum = 0.0;
            bool hasMaximum = false;
            double maximum = 0.0;
            std::string unit;
            bool hasRest = false;
            double rest = 0.0;              ///< the row's default: where the double click puts it
        };

        const Dial& dial() const noexcept                 { return dialed; }
        void setDial (Dial wanted)                        { dialed = std::move (wanted); }

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

            /** How many times a hand moved the page - a page key, a rotary or
                dial turned or pressed: what tells the window which surface
                moved last (namespace draft §101). A page that is up only
                because a deck is plugged in has moved nothing. */
            int moves = 0;

            bool operator== (const Page&) const = default;
        };

        void setPage (const std::string& surfaceId, const Page& page)  { pages[surfaceId] = page; }

        /** The page a surface shows, or the Show page for one nobody set. */
        Page pageOf (const std::string& surfaceId) const
        {
            const auto found = pages.find (surfaceId);
            return found != pages.end() ? found->second : Page {};
        }

        //==============================================================================
        /*  WHAT A STREAM DECK'S KEYS SAY (namespace draft §95): a line a key,
            in the hand's order, as `DeckBridge` writes it - what the key does,
            then its words and its colour - so a client draws the deck and a
            test reads it with no deck in the room. Runtime, like a page: it
            moves with no command at all. */
        void setKeys (const std::string& surfaceId, std::string text)  { keys[surfaceId] = std::move (text); }

        std::string keysOf (const std::string& surfaceId) const
        {
            const auto found = keys.find (surfaceId);
            return found != keys.end() ? found->second : std::string {};
        }

        //==============================================================================
        /*  THE CUE A STREAM DECK KEY IS BEING LEARNT FOR (namespace draft §104):
            set by `surface.learnKey` while the inspector's Key line listens,
            emptied when it stops or when a deck's key was taken. The next key
            pressed on a deck of keys is that cue's, and fires nothing. Tonight's,
            never stored. */
        const std::string& learningKey() const noexcept  { return learnKeyFor; }
        void setLearningKey (std::string cueId)            { learnKeyFor = std::move (cueId); }

    private:
        std::string learnKeyFor;
        Dial dialed;
        std::map<std::string, Status> table;
        std::map<std::string, Page> pages;
        std::map<std::string, std::string> keys;
        std::string aimed;
    };
}
