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
    THE BRIDGE BETWEEN A CONTROL IN THE ROOM AND THE NUMBER IT MOVES
    (namespace draft §74, BD.9; AQX-ARB).

    What the show's bindings hear becomes `node.set` before each tick: the one
    write verb a hand uses (§16.4), with the origin `binding:<id>`, so the log
    says which binding wrote, a turn joins one undo step as a fader's move does,
    and under the lock the row's own door decides what rides live (ARA).

    THREE THREADS, AS THE SURFACE BRIDGE HAS THEM.
      - `midiArrived` runs on the MIDI callback thread and `oscArrived` on the
        OSC socket's. Each matches against the index the tick last published
        and keeps what it heard in an inbox, under one mutex: the latest place
        of an absolute or a rate control, the steps of a relative one summed.
        No document, no tree.
      - `publish`, `beforeTick`, `afterTick` and `letGo` run on the tick
        thread, which owns everything else: each binding's pickup, the push a
        rate control is held at, which cues are running.

    ONE WRITE A BINDING A TICK. A fader sends hundreds of places a second and
    the tree shows one a tick, as a surface's fader is coalesced. A rate control
    writes every tick while it is pushed, by the time a tick lasts.

    WHEN A BINDING IS LIVE (AQY): one with no cue while the show is open; one of
    a cue while a run of that cue has started and not finished - waiting,
    playing, stopping or holding its post-wait - so Esc's graceful ending lets
    it go when the run ends, as it would have at the end. Armed and let go, its
    pickup starts again and a push it was held at is forgotten. What a binding
    hears while it is not live is dropped.

    A DOUBLE ESC STOPS WHAT A BINDING IS DOING, NEVER THE BINDING (ARB): a rate
    control held over at the press writes nothing more until it comes back to
    its rest and is pushed again, and what was heard and not yet written is
    dropped. A pickup already caught stays caught: the press moved no value. A
    control moved after the press is a hand moving a control (§23.11).

    NAMES NO JUCE TYPE, so a test drives it with bytes in and events out.
*/

#include <wfg/engine/binding/BindingIndex.h>
#include <wfg/engine/command/Event.h>
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/osc/OscValue.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg
{
    class CommandRegistry;
}

namespace wfg::tree
{
    class TreeSnapshot;
}

namespace wfg::binding
{
    class BindingBridge
    {
    public:
        using Submit = std::function<bool (Event)>;

        BindingBridge();
        ~BindingBridge();

        BindingBridge (const BindingBridge&) = delete;
        BindingBridge& operator= (const BindingBridge&) = delete;

        /*  Tick thread, whenever the show changes: the bindings to match, for
            the input threads and for the tick. A binding no longer in the show
            is forgotten with everything it was holding. */
        void publish (std::shared_ptr<const BindingIndex> index);

        /*  MIDI callback thread: one complete message on a declared port. True
            when a binding took it. It is never consumed: a trigger on the same
            controller fires as it always did. */
        bool midiArrived (const std::string& portId, const midi::Bytes& message);

        /*  OSC socket thread: a datagram's message on Go.dot's own port. True
            when a binding took it - and then the caller writes it to no node,
            as a trigger's address is written to none. */
        bool oscArrived (const std::string& address, const std::vector<osc::Value>& args);

        /*  Tick thread, before the Runner's hook: what the bindings heard since
            the last tick, and every pushed rate control, as one `node.set` a
            binding, read against the snapshot the last afterTick kept. */
        void beforeTick (const Submit& submit);

        /*  Tick thread, after the tree is published: keeps the snapshot, and
            arms or lets go every binding of a cue whose run began or ended. */
        void afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshot);

        /*  Tick thread: a double Esc (ARB). See above. */
        void letGo();

        /*  WITHIN A STEP OF THE VALUE, on the control's travel: one step of a
            seven-bit controller. Where a pickup catches (AQZ), and how far the
            value may have moved before something else is taken to have moved
            it. */
        static constexpr double pickupStep = 1.0 / 127.0;

    private:
        struct State;
        std::unique_ptr<State> impl;
    };

    /*  AND THE DOUBLE ESC HEARD (ARB): `run.killAll` as the registry holds it,
        wrapped so the bridge lets go once it has been applied. Called after the
        Runner has specialised it, in the one process that has a bridge. */
    void letGoOnDoubleEsc (CommandRegistry& registry, BindingBridge& bridge);
}
