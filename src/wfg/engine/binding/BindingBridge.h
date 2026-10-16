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
    (namespace draft §74, BD.9, BD.10; AQX-ARD).

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

    FEEDBACK (ARC): a MIDI binding that asks for it is sent its target's value
    after each tick, as its own control would send it - a motor fader moved, a
    ring lit - on its port and channel (the first channel when it names any;
    nothing when it names no port, since a value cannot go back to "any").
    Only a value that changed, one message a binding a tick, which is how a
    surface is paced; and never the value the control itself just sent, so a
    fader being pushed is not pushed back. Absolute and relative bindings; a
    rate control has no place to show.

    LEARN (ARD): asked for one binding, the next control that moves - any MIDI
    message on any port a surface has not claimed, any datagram over UDP not
    addressed to the tree - becomes its endpoint, written as one `node.setMany`
    by `binding:<id>`, one undo step. The move that is learned moves nothing
    else. Asking for another binding, or for none, stops listening.

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

        /*  Tick thread, after the tree is published: keeps the snapshot,
            arms or lets go every binding of a cue whose run began or ended,
            and sends what feedback is owed (ARC). */
        void afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshot);

        /*  Where feedback goes. Null sends nothing. Set before the ticks start;
            the sink outlives every tick. */
        void sendTo (midi::MidiSink* sink) noexcept;

        /*  Tick thread: listen for the next control that moves, for this
            binding (ARD). Empty stops listening. */
        void learnFor (const std::string& bindingId);

        /*  The binding listening, or empty: `/godot/engine/bindingLearn`.
            Tick thread. */
        const std::string& learning() const noexcept;

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

    /*  THE COMMANDS THE BRIDGE HEARS, as the registry holds them, wrapped so
        the bridge acts once each has been applied: `run.killAll` lets go
        (ARB), `binding.learn` listens (ARD). Called after the Runner has
        specialised the first, in the one process that has a bridge; a replay
        and a rig without one keep the plain commands. */
    void hearCommands (CommandRegistry& registry, BindingBridge& bridge);
}
