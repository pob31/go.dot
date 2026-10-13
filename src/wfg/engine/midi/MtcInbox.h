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
    MIDI TIME CODE COMING IN: from the driver's thread to the tick (namespace
    draft §60, TG.4; AIM, AIR).

    WHY IT IS TAKEN OUT OF THE INPUT AT ALL. Quarter frames and full frames
    are not trigger events - `eventFrom` drops them, as it always did - and
    a time group whose source is MTC needs every one of them, with the moment
    it arrived: the code's position is read from WHEN each piece came as much
    as from what it said. So `MidiInputs` hands each one that arrives on the
    show's `sync/mtcIn` to the inbox here, stamped, before the consumer chain
    sees it - a port a surface claims would otherwise swallow it - and the
    tick takes them all once a tick.

    TWO THREADS, THE HEARD BOX'S SHAPE (tree/HeardBox.h). The driver's thread
    pushes, the tick takes everything at once into a vector it reserved. The
    mutex is a leaf, held for one push or one take and nothing else: nothing
    is asked for while it is held, nothing waits on the tick, and the driver's
    thread never waits for more than a copy of a few hundred bytes. A full
    ring drops the OLDEST - the newest is where the code is now, which is what
    a reader that fell behind needs - and counts it.

    THE TICK'S SIDE (`MtcInput`) reads the show once a revision - which port,
    and whether its `rx` is on (AIM: a port that does not listen hears no
    code) - feeds its reader's clock line the tick's pair first, and reads what
    was taken in order. TG.4 keeps the observations of the last take and does
    nothing more with them: TG.5's watcher fits its line through them.
*/

#include <wfg/engine/timecode/MtcReader.h>
#include <wfg/engine/timecode/MtcSchedule.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::midi
{
    /*  Whether a message is MIDI Time Code this side reads: a quarter frame,
        `F1 xx`, or a full frame, `F0 7F <device> 01 01 hh mm ss ff F7`. What
        the inbox takes out of the input and the triggers never see. */
    bool isMtcMessage (const std::uint8_t* bytes, std::size_t size) noexcept;

    /*  ONE MESSAGE TAKEN OFF THE INPUT: the port it came in on (the show's
        identifier for it), its bytes - ten at most, a full frame - and the
        steady clock's nanoseconds when it arrived. */
    struct MtcEntry
    {
        std::string portId;
        std::array<std::uint8_t, timecode::mtcFullFrameSize> bytes {};
        std::size_t size = 0;
        std::int64_t arrivalNanos = 0;
    };

    class MtcInbox
    {
    public:
        //  Two seconds of quarter frames at 30, a tick's worth a hundred times over.
        static constexpr std::size_t capacity = 256;

        /*  THE DRIVER'S THREAD (or `inject`): one message kept. Anything
            longer than a full frame is not MTC and is not kept. */
        void push (const std::string& portId, const std::uint8_t* bytes, std::size_t size, std::int64_t arrivalNanos);

        /*  THE TICK: everything kept, oldest first, into `into` - cleared
            first; reserve it `capacity` once and the take allocates nothing. */
        void takeAll (std::vector<MtcEntry>& into);

        //  How many were dropped because the ring was full, since the start. Any thread.
        std::uint64_t droppedCount() const noexcept { return dropped.load (std::memory_order_relaxed); }

    private:
        std::mutex guard;
        std::array<MtcEntry, capacity> ring {};
        std::size_t head = 0;
        std::size_t held = 0;
        std::atomic<std::uint64_t> dropped { 0 };
    };

    /*  WHERE THE SHOW SAYS MTC COMES IN: the declared port `sync/mtcIn` names
        - empty when it names nothing, or something that is not a `<Port>` - and
        whether that port's `rx` is on. Read through the document's getters,
        once a show revision, on the tick. */
    struct MtcInSetting
    {
        std::string port;
        bool listening = false;

        bool operator== (const MtcInSetting&) const = default;
    };

    MtcInSetting mtcInOf (const doc::ShowDocument& document);

    /*  THE TICK'S SIDE: the reader, its clock line, and what the last take
        read. Owned by the tick thread alone. */
    class MtcInput
    {
    public:
        MtcInput();

        /*  The show's setting, read again at a new revision. A change of port,
            or of `rx`, forgets the cycle part-read: what comes next does not
            follow it. */
        void configure (const MtcInSetting& settingNow);
        const MtcInSetting& setting() const noexcept { return current; }

        //  The tick's clock pair - the one the MTC lane is fed - before each drain.
        void feedClock (std::int64_t sample, std::int64_t nanos, int sampleRate) noexcept;

        /*  ONCE A TICK: everything the inbox kept, taken, and what came on the
            port while it listens read in order. What came while it did not
            listen, or on a port the show has since moved away from, is taken
            and dropped, so a port switched back on reads from then and not
            from what queued while it was off. */
        void drain (MtcInbox& inbox);

        //  What the last drain read, in order (TG.5's).
        const std::vector<timecode::MtcObservation>& observations() const noexcept { return heard; }

        const timecode::MtcReader& reader() const noexcept { return reading; }

        //  Messages taken and read, and taken and dropped unread, since the start.
        std::uint64_t readCount() const noexcept     { return readSoFar; }
        std::uint64_t ignoredCount() const noexcept  { return ignoredSoFar; }

        /*  THE SAMPLE THE LAST MESSAGE READ ARRIVED ON, or -1: a piece of code
            still coming, whether or not it has completed a cycle yet - what
            keeps TG.5's watcher from calling a running code stopped while
            its cycles are being put together (namespace draft §60, AIR). */
        std::int64_t lastHeardSample() const noexcept
        {
            return lastHeardNanos > 0 ? reading.sampleAt (lastHeardNanos) : -1;
        }

    private:
        MtcInSetting current;
        timecode::MtcReader reading;
        std::vector<MtcEntry> taken;
        std::vector<timecode::MtcObservation> heard;
        std::uint64_t readSoFar = 0;
        std::uint64_t ignoredSoFar = 0;
        std::int64_t lastHeardNanos = 0;
    };
}
