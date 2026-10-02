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

/*  WHAT IS WAITING TO GO OUT ON A MIDI CABLE, AND WHICH NOTES ARE STILL DOWN.

    `MidiSender`'s queue, taken out of it so it can be tested on every machine:
    JUCE makes no virtual MIDI port on Windows and no CI runner has a MIDI
    interface, so a queue that lived inside the class with the device could
    never be asked what a double Esc left in it. Like `MidiSink.h`, this names
    no JUCE type.

    TWO KINDS OF MESSAGE SHARE THE QUEUE. A cue's (`cue` set, carrying its run)
    and everything else - the surface bridge's LEDs, motor faders and
    displays. A double Esc drops only the first kind (2026-10-02, namespace
    draft §23.10): the bridge sends only what differs from what it last sent,
    so a dropped LED stays wrong until something changes it again, and a
    surface's state is not an action of the show's.

    THE NOTE RECORD IS KEPT HERE, ON THE SENDING SIDE, and fed only by what
    actually leaves (`markLeaving`, from the sending thread, for a message
    whose port has a device). A note-on a double Esc drops never reached a
    synth, so it gets no note-off - "nothing to a synth Go.dot never played"
    (the author, 2026-09-30). Not thread-safe: `MidiSender` holds its queue
    lock around every call.
*/

#pragma once

#include <wfg/engine/midi/MidiSink.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace wfg::midi
{
    /** One message waiting for the cable. */
    struct Outgoing
    {
        std::string port;           // the show's port identifier
        Bytes bytes;
        std::string run;            // the run that sent it - a cue's only
        bool cue = false;           // a cue's message: a double Esc may drop it
    };

    //==========================================================================
    /*  THE NOTES A CUE STARTED AND NOTHING HAS ENDED, one entry per port,
        channel and key (the author, 2026-09-30: a note-off for each, and
        nothing else).

        A SET AND NOT A COUNT (2026-10-02, namespace draft §23.10). Two cues
        sending the same note-on to one synth still need one note-off: a synth
        that stacks voices on a repeated key is answered by the key, not by how
        many times it was pressed. The runs that pressed it are kept beside it,
        oldest first - what Doh! will ask for when it takes back one GO's
        notes and not another's (§24, L26). */
    class NoteLedger
    {
    public:
        /*  A message that has left. Only a cue's, of three bytes or more:
            - a note-on with a velocity above nought starts the key;
            - a note-off, or a note-on at velocity nought - which is how a
              synth reads it, however the cue named it - ends it;
            - All Sound Off (CC 120) and All Notes Off (CC 123) end every key
              on that port and channel.
            Anything else is no note's business. */
        void observe (const Outgoing& message);

        /*  One note-off for every key still down, `0x8n key 0`, in port,
            channel and key order, and the record emptied. They are not a cue's:
            a second double Esc cannot drop them, and the record does not take
            them for a cue's note ending. */
        std::vector<Outgoing> releaseAll();

        /*  Forgets a port's keys: its device was closed, or another was put
            behind its name, and a note-off would reach a synth that never
            heard the note-on. */
        void forgetPort (const std::string& port);

        /** How many keys are down. */
        std::size_t sounding() const noexcept { return notes.size(); }

        /** The runs that pressed a key still down, oldest first; empty for one that is up. */
        std::vector<std::string> runsOf (const std::string& port, std::uint8_t channel,
                                         std::uint8_t note) const;

    private:
        using Key = std::tuple<std::string, std::uint8_t, std::uint8_t>;   // port, channel, key
        std::map<Key, std::vector<std::string>> notes;
    };

    //==========================================================================
    /*  THE QUEUE ITSELF, first in, first out, with the note record beside it. */
    class SendQueue
    {
    public:
        void push (Outgoing message);

        /** The oldest message, taken; false when there is none. */
        bool pop (Outgoing& out);

        /*  The message `pop` gave is going to a device: a cue's is shown to
            the note record. Called by the sending thread under the queue lock,
            between `pop` and the send, so a double Esc either finds the note
            in the record or finds its note-on still in the queue. */
        void markLeaving (const Outgoing& message);

        /*  DOUBLE ESC: every cue message still waiting is dropped, and the
            note record's note-offs go to the FRONT, in order - ahead of a
            surface's SysEx, which can hold the cable for thirty milliseconds a
            message - so each follows the note-on it ends, which the one sending
            thread has already taken. Answers how many messages were dropped. */
        std::size_t dropQueued();

        /** A port's device gone or changed: see `NoteLedger::forgetPort`. */
        void forgetPort (const std::string& port);

        /** Everything still waiting, for the shutdown's last delivery. */
        std::deque<Outgoing> takeAll();

        bool empty() const noexcept { return items.empty(); }
        std::size_t size() const noexcept { return items.size(); }

        const std::deque<Outgoing>& waiting() const noexcept { return items; }
        const NoteLedger& notes() const noexcept { return ledger; }

    private:
        std::deque<Outgoing> items;
        NoteLedger ledger;
    };
}
