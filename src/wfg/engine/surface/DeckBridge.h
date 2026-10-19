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
    THE BRIDGE BETWEEN A STREAM DECK AND THE SHOW (namespace draft §95, SD.4).

    A deck is a surface - `<Surface profile="streamDeck">`, on the Surfaces
    page beside the D700, its status at /godot/surface/<id> - but it is driven
    here and not by SurfaceBridge: a deck has no strips, no motors, no MIDI
    and no SysEx, and what it has - keys that show pictures, a strip of
    touch, dials - is nothing the Mackie bridge carries. What the two share is
    the pages' laws (SurfacePages.h) and the aim (SurfaceTable), so a dial on
    the Stream Deck+ turns a band exactly as a D700 rotary turns it.

    ONE WRITE VERB, AS EVERY HAND. A pad is `cue.press` and `cue.release` - the
    tablet's Cart tab's, so a sampler member presses its strip and any other
    cue fires; GO is `go`, Esc `run.stopAll` and again inside the window
    `run.killAll`, Doh! `go.doh`, the arrows `standby.previous` and
    `standby.next`; a dial is a `node.set` on the aimed cue's row, or a
    `send.create`; the Stream Deck+'s cue arrows are `surface.aim`. Every one
    with the origin `surface:<id>`, so the log says which hand did what and a
    replay needs no deck. Which cart page or which dial page is up is the
    deck's own and no command, as a D700's page is.

    THREADS. Everything here runs on the tick thread: `declare` when the show
    changes, `beforeTick` before the Runner's hook (the hands), `afterTick`
    after the tree is published (the pictures). The deck's own thread is the
    host's (StreamDeck.h); this only asks it.

    NAMES NO JUCE TYPE, so a test drives it end to end with a deck of its own.
*/

#include <wfg/engine/command/Event.h>
#include <wfg/engine/surface/StreamDeck.h>
#include <wfg/engine/surface/SurfaceTable.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::tree
{
    class TreeSnapshot;
}

namespace wfg::surface
{
    /*  What the show declares about one deck, read off the document by the
        caller once per show revision. */
    struct DeckSurfaceSpec
    {
        std::string id;
        bool enabled = true;
        std::string device;     // the deck's serial; empty, the first deck no other surface names
        std::string cart;       // the cart it shows; empty, the show's first
        bool showKeys = false;  // GO, Esc, Doh! and the standby's arrows on it
        int brightness = 70;
    };

    class DeckBridge
    {
    public:
        using Submit = std::function<bool (Event)>;

        DeckBridge (DeckHost& host, SurfaceTable& table);
        ~DeckBridge();

        DeckBridge (const DeckBridge&) = delete;
        DeckBridge& operator= (const DeckBridge&) = delete;

        /*  Tick thread: the decks the show declares. Replaces the declaration;
            a deck the show no longer names lets go of what its hands hold and
            is let go of. Answers whether the table changed. */
        bool declare (std::vector<DeckSurfaceSpec> decks);

        /*  Tick thread, before the Runner's hook: what the decks' hands did
            since the last tick, as commands, and which deck each surface has
            tonight. Answers whether the table changed, so the caller can ask
            the tree to rebuild. */
        bool beforeTick (const Submit& submit, std::int64_t tick);

        /*  Tick thread, after the tree is published: what every open deck
            should show, the keys that changed sent to the host. */
        void afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshot, std::int64_t tick);

        /*  The decks plugged into this machine, as `engine/streamDecks` says
            them: a line each, its model's word then its serial. */
        std::string pluggedText() const;

    private:
        struct State;
        std::unique_ptr<State> impl;
    };
}
