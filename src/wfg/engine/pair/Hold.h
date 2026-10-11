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
    THE HOLD: WHILE IT IS ON, NOTHING LEAVES FOR A DEVICE (namespace draft §58,
    RD.1; PRD §3.15).

    The backup's half of redundancy. A backup that follows its main plays the
    same show at the same moment, and every byte it would send a console, a
    lighting desk, a processor or a synth is a byte the main has already sent:
    the author's words, "we need to mute network and MIDI commands while the
    connection is alive so the target devices don't get twice the commands".
    On its own - `wfg serve --hold` - it is a spare that runs the show beside a
    rehearsal and says nothing to anybody. From RD.7 the pair's seat moves it
    (pair/Leadership.h): on while a machine follows or a backup waits for its
    main, off when it leads, is alone, or sends beside the other (AFQ) - and
    `--hold` keeps it on whatever the seat.

    HELD MEANS COUNTED AS SENT (AGG). A held send is answered exactly as one
    that left, and it is answered BEFORE anything that could refuse it - an
    unbound MIDI port, a closed connection, a serial port with nothing behind
    it - because a backup is the machine whose devices are NOT plugged in, and
    its cues must still end as the main's do: `done`, no `not-sent`, no
    `no-port`, no `send-failed`. A run that ended differently on the two
    machines would be the first thing a following backup disagreed about.

    WHAT IS HELD, each at its own last door: the device sender's one funnel to
    a datagram, a serial port and a connection (MountSender::deliver, and its
    MIDI wire); the MIDI sender's queue (MidiSender::enqueue - MIDI cues, a
    patch's [noteout], the MIDI wire, Doh!'s rollback); a serial port's lines
    and packets (SerialTable); the answers to a processor (RawSender); the
    questions a verified cue and the observation sweep ask (Runner::askTarget,
    MountProbe), a processor's description fetched (MountFetcher); and in
    serve, the device connections left closed (AGW), the keep-alives skipped,
    LISTEN asked of nobody, a patch's own [netsend] (Pd's x_net.c, amended by
    the build).

    WHAT IS NOT (AGH): video - NDI, Spout, Syphon are changed over downstream,
    as the sound is - a control surface's MIDI port, which is a client's and is
    driven by the machine it is plugged into, the WebSocket and the page, and
    opening anything: serial and MIDI ports stay open, since they are this
    machine's own hardware and only their writes are the show's.

    NOT A ROW OF THE SHOW, and not the per-object `tx` switches either: those
    are decisions somebody made about the show, they dirty it and they are in
    the log, and a backup whose log differed from its main's because it held
    would be a backup that could not follow. This is the machine's, published
    as `pair/sending` and stored nowhere (§4.10).

    ONE ATOMIC AND NOTHING ELSE. It is read on the tick thread, on Pd's threads
    through the C hook, and from RD.6 written from the link's; relaxed is
    enough, since nothing is published through it - a send either side of the
    moment the switch moves is a send either side of the moment the link was
    lost. Owned by whoever runs the session (serve) and handed down by pointer;
    a null pointer is never held, which is what every rig and `wfg replay` have.
    Never a singleton: RD.6's tests run a main and a backup in one process.
*/

#include <atomic>

namespace wfg::pair
{
    class Hold
    {
    public:
        Hold() = default;
        explicit Hold (bool heldAtStart) noexcept : on (heldAtStart) {}

        Hold (const Hold&) = delete;
        Hold& operator= (const Hold&) = delete;

        /** Whether nothing leaves for a device. Any thread. */
        bool held() const noexcept { return on.load (std::memory_order_relaxed); }

        /** Holds, or lets go. Any thread. */
        void set (bool hold) noexcept { on.store (hold, std::memory_order_relaxed); }

        /*  WHERE A DEVICE'S ANSWER TO ONE OF THIS MACHINE'S SENDS COMES FROM -
            the seam a cue that waits for one asks (a `verified` wait, a
            pre-send reading what the desk held before writing).

              fromDevice     - nothing is held: the device is asked, and answers.
              fromElsewhere  - held, and the answer arrives all the same, from
                               somewhere that did send: RD.6's following backup,
                               which waits for the main's streamed
                               `mount.readback` rather than asking.
              none           - held, and nobody will answer: RD.1's lone spare.
                               The cue does not wait; its held write is taken
                               for the answer, as a `sent` wait would take it.

            RD.1 has no main, so a held machine answers `none`; RD.6 makes a
            following backup answer `fromElsewhere`, and this is the one place
            that changes. */
        enum class Answer { fromDevice, fromElsewhere, none };

        Answer answer() const noexcept
        {
            if (! held())
                return Answer::fromDevice;

            return elsewhere.load (std::memory_order_relaxed) ? Answer::fromElsewhere : Answer::none;
        }

        /*  WHILE A BACKUP FOLLOWS ITS MAIN (RD.6), a device's answer to one of
            its held sends comes from elsewhere: the main asked the device, and
            its `mount.readback` arrives in the stream. Set by serve on the tick
            the backup starts and stops following; it says nothing while the
            machine does not hold. Any thread. */
        void setAnswersFromElsewhere (bool fromMain) noexcept
        {
            elsewhere.store (fromMain, std::memory_order_relaxed);
        }

    private:
        std::atomic<bool> on { false };
        std::atomic<bool> elsewhere { false };
    };

    /** Whether a hold that may be absent holds: null never does. */
    inline bool isHeld (const Hold* hold) noexcept
    {
        return hold != nullptr && hold->held();
    }

    /** `Hold::answer` for a hold that may be absent: null is never held. */
    inline Hold::Answer answerOf (const Hold* hold) noexcept
    {
        return hold != nullptr ? hold->answer() : Hold::Answer::fromDevice;
    }
}
