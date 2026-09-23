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
    THE BRIDGE BETWEEN A BOX OF FADERS AND THE SHOW (namespace draft §16.6).

    What a surface's hands do becomes commands before each tick; what the tree
    says after it becomes motor moves, scribble strips, LEDs, rings and colour.
    Nothing else: the codec (McuCodec) owns the bytes, the profile table
    (SurfaceProfile) owns what a button means, and the engine owns every
    decision - a fader lifted from the bottom starting a clip is the Runner's
    rule over a trim, and the bridge only writes the trim.

    ONE WRITE VERB. A fader is `node.set` on its strip's `target`, a touch is
    `node.touch`/`node.release` on it, a pad is `strip.press`/`strip.release`
    - exactly what the virtual panel and the page send - with the origin
    `surface:<id>`, so the log says which hand did what and the touch table
    gates this surface as it gates every other client.

    THREE THREADS, AND ONE LOCK.
      - `arrived` runs on the MIDI callback thread. It asks whether a declared
        surface owns the port and pushes the raw bytes into an inbox, under one
        mutex, and does nothing else: no decoding, no document, no tree.
      - `declare`, `beforeTick` and `afterTick` run on the tick thread, which
        owns everything else here - the per-strip caches, the last snapshot,
        the table - exactly as it owns the model.
      - Every byte leaves through `MidiSink::send`, which queues it for the
        sender's worker thread: a SysEx busy-waits on Windows, and it does so
        there rather than on the thread that publishes the tree.

    WHAT IT COSTS A TICK. Before: the inbox's messages, and one coalesced write
    per strip that moved. After: about ten lookups in the snapshot per strip of
    each connected hardware surface, compared with what was last sent, and
    bytes only for what differs. Addresses are composed when a strip's cue, DCA
    or holder changes, never per tick; the caches are made at `declare`.

    NAMES NO JUCE TYPE, so a test with no MIDI interface in the room drives it
    end to end with bytes in and bytes out.
*/

#include <wfg/engine/command/Event.h>
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/surface/SurfaceTable.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::tree
{
    class TouchTable;
    class TreeSnapshot;
}

namespace wfg::surface
{
    /*  What the show declares about one surface, read off the document by the
        caller once per show revision. */
    struct SurfaceSpec
    {
        std::string id;
        std::string profile;              // the word: virtual | mcu | d700 | midiPads
        std::vector<std::string> ports;   // port ids, bank order: port 0 is strips 0..7, port 1 is 8..15
        bool enabled = true;
        int channel = 0;                  // midiPads: 0 for any, else 1..16
        int firstNote = 36;               // midiPads: the first pad's note
        std::vector<std::string> strips;  // strip ids, in index order
    };

    /*  What the machine knows about one declared port tonight. */
    struct PortState
    {
        bool bound = false;
        bool rx = true;
        bool tx = true;
        std::string name;                 // the show's name for it, for the sentence
    };

    //==========================================================================
    /*  A COLOUR AS THE D700 TAKES IT: three components, 0..127 each. */
    struct Rgb
    {
        int red = 0;
        int green = 0;
        int blue = 0;

        bool operator== (const Rgb&) const = default;
    };

    /** A cue's authored `#RRGGBB`, each eight-bit component halved to 0..127
        (control guide §4.4). Nothing for anything that is not that. */
    std::optional<Rgb> colourFromHex (std::string_view text);

    /*  A run's `timbre` - "h s l", the hue in degrees and the saturation and
        lightness from nought to one - as HSL turned into RGB, 0..127. A silent
        frame, "0 0 0", is black, which is off. Nothing for an empty or
        malformed text: a run whose analysis has not arrived has no colour yet. */
    std::optional<Rgb> colourFromTimbre (std::string_view text);

    /*  EIGHT LEVELS A COMPONENT, which is what a colour is compared in: a
        colour is written again only when it moved a step somebody could see.
        What is written is the colour itself, not its level. */
    Rgb colourLevels (Rgb colour) noexcept;

    //==========================================================================
    class SurfaceBridge
    {
    public:
        using Submit = std::function<bool (Event)>;
        using PortStates = std::function<PortState (const std::string& portId)>;

        SurfaceBridge (midi::MidiSink& sink, SurfaceTable& table);
        ~SurfaceBridge();

        SurfaceBridge (const SurfaceBridge&) = delete;
        SurfaceBridge& operator= (const SurfaceBridge&) = delete;

        /*  Tick thread, when the show changes or a port is bound again: the
            surfaces the show declares, and a way to ask about a port.

            REPLACES THE DECLARATION. A surface whose ports are all bound with
            rx and tx on is CONNECTED, and the table says so; one that has
            just become connected is re-asserted in full on the next afterTick
            - asked who it is, then every display, LED, fader, ring and colour
            painted, because the firmware and whatever ran before may have left
            anything on it. Otherwise it is not connected and the table carries
            one sentence naming the first thing wrong: "the port \"D700 bank
            1\" has no device behind it", "... has rx turned off", "... has tx
            turned off", "a d700 surface needs its ports: none is declared",
            "it is switched off in the show", or a port another surface already
            carries. A virtual surface is always connected, and nothing is sent
            to it or read from it here.

            A surface NOT CONNECTED IS NEITHER DRIVEN NOR HEARD (§16.6), and a
            surface that stops being connected - or disappears - lets go of
            whatever its hands were holding: its held pads and gates are
            released and its touches given back (`node.releaseAll`), on the
            next beforeTick. A surface whose shape is unchanged keeps its
            caches, so a show edit repaints nothing.

            Answers whether the table changed, so the caller can ask the tree
            to rebuild. */
        bool declare (std::vector<SurfaceSpec> surfaces, const PortStates& portState);

        /*  MIDI callback thread: one complete message arrived on a port.

            True when a declared, enabled hardware surface owns that port - so
            the trigger matcher does not also see it - and false otherwise. A
            message for a surface that is not connected (a port unbound, rx off
            or tx off) is owned and dropped. Takes the inbox mutex for a push
            and nothing else. */
        bool arrived (const std::string& portId, const midi::Bytes& message);

        /*  Tick thread, before the Runner's hook (plan decision 13): drains the
            inbox in arrival order and submits what the hands did, reading
            targets and values off the snapshot the last afterTick kept.

            FADERS ARE COALESCED to one `node.set` per strip per tick, the latest
            position winning, and so are an encoder's detents and a pad's
            pressure: a moving fader sends hundreds of positions a second and
            the tree shows one a tick. Touches, presses, releases and buttons
            are submitted as they arrive, so a touch always reaches the queue
            before that tick's write.

            Answers whether the surface table changed - a surface said who it
            is - so the caller can ask the tree to rebuild. */
        bool beforeTick (const Submit& submit, std::int64_t tick);

        /*  Tick thread, after the tree is published: what each connected,
            enabled Mackie surface should show, compared with what it was last
            sent, and the difference sent. Keeps the snapshot for the next
            beforeTick. */
        void afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshot,
                        const tree::TouchTable& touches, std::int64_t tick);

        /*  How many SysEx messages were NOT sent because `isSafeSysEx` refused
            them. Every one is a programming error - a command byte outside the
            six the D700 is known to survive - and nought is the only right
            answer. */
        std::size_t refusedSysEx() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> impl;
    };
}
