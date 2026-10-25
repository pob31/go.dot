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

/*  WHERE A MIDI CUE'S BYTES GO, as the cue layer sees it.

    NAMES NO JUCE TYPE, deliberately, and for the same reason `cue::Player`
    names no Tracktion one: this header is included by the Runner and by tests,
    a null implementation is a complete one, and a show replayed with no sender
    creates the same runs and writes the same log with nothing reaching a port.

    IT IS ALSO WHAT MAKES THE BYTES TESTABLE AT ALL. JUCE creates virtual MIDI
    ports on macOS and Linux and not on Windows, so a test that wanted to hear
    what a cue sent would run on two platforms of three. A recording sink runs
    on all of them and on every CI runner, which have no MIDI hardware at all.

    THE PORT IS THE SHOW'S NAME FOR IT - "Lights", "The desk" - and never a
    device. Binding that name to a cable is `wfg serve --midi-out=<name>=<device>`
    and is a fact about the building (§4.10).
*/

#pragma once

#include <wfg/engine/timecode/Timecode.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wfg::midi
{
    /*  The bytes of one MIDI message, as the wire carries them.

        A vector rather than a fixed array because a system-exclusive dump is
        whatever length the manual says, and PRD §3.10 asks for sysex by name.
    */
    using Bytes = std::vector<std::uint8_t>;

    /** Why a send did not happen. Empty means it did. */
    namespace sendError
    {
        /** The show named a port nothing was bound to. */
        inline constexpr const char* noPort = "no-port";

        /** The cue's own fields do not make a message anybody could send. */
        inline constexpr const char* badMessage = "bad-message";
    }

    struct MidiSink
    {
        virtual ~MidiSink() = default;

        /*  Queues one message for a declared port. Returns the reason it could
            not be, or empty when it was taken.

            CALLED FROM THE TICK THREAD, at the flush, and it must not block
            there: on Windows `sendMessageNow` for a system-exclusive message
            busy-waits the calling thread until the port has taken every byte -
            about thirty milliseconds for a hundred bytes at MIDI baud, which is
            a tick and a half of the thread that owns the model. So an
            implementation that talks to hardware queues here and sends
            somewhere else. */
        virtual std::string send (const std::string& port, const Bytes& bytes) = 0;

        /*  A CUE'S MESSAGE, which is the one kind a double Esc may drop
            (2026-10-02, namespace draft §23.10): the same queue as `send`,
            marked as the show's output and carrying the run that sent it. A
            surface's feedback goes through `send` and is never dropped - its
            LEDs and motor faders would be left showing what the bridge thinks
            it last said - nor counted as a note the show started, since its
            LEDs are note-ons.

            Its own name and not an overload of `send`, which the strict build
            refuses (-Woverloaded-virtual). A sink with nothing to drop - a
            recording one, a null one - is complete with this default. */
        virtual std::string sendForRun (const std::string&, const std::string& port, const Bytes& bytes)
        {
            return send (port, bytes);
        }

        /*  DOUBLE ESC (PRD §4.4, "drops all actions"; the author, 2026-09-30,
            for the notes): every cue message not yet gone is dropped, and one
            note-off goes for each note a cue started on a port, a channel and
            a key that nothing has ended since - only those, no all-notes-off,
            and nothing to a synth Go.dot never played. Tick thread; never waits
            on a port. Answers how many were dropped. */
        virtual std::size_t dropQueued() { return 0; }

        //======================================================================
        /*  MIDI TIME CODE (namespace draft §59, TC.5): one program at a time,
            sent by a thread of the sink's own at each message's moment rather
            than queued at a tick's flush, since quarter frames come every 8 to
            10 ms and the tick is 20 (AHQ). Every one of these does nothing by
            default, so a recording sink, a null one and every fake is complete
            without them, and a replay with no sender sends nothing.

            START: program `program` - a number the Runner gives each run, never
            nought - has frame `startFrame` of `rate` (counted from 00:00:00:00)
            begin on Go.dot's sample `zeroSample`, at `sampleRate`, on `port`.
            It replaces whatever program is sending from that sample on: a
            takeover or a jump, with a full frame first (AHL, AHO). Tick
            thread; never waits on a port.

            ANSWERS EMPTY, OR `no-port` when the port has no device behind it
            and this machine does not hold: a held machine answers as taken -
            held means counted as sent (§58, AGG) - and runs the program held,
            so a full frame leaves on the tick the hold lets go. */
        virtual std::string startTimecode (const std::string&, const std::string&, std::uint64_t,
                                           timecode::Rate, int, std::int64_t, std::int64_t)
        {
            return {};
        }

        /*  AND HOW IT ENDS (AHK): nothing due at or after the first frame
            boundary at or after `atSample` leaves; with `cut`, nothing at or
            after `atSample` itself - a double Esc. An `atSample` of nought or
            less is as soon as can be, and is never counted late. A stop for a
            program whose start has not come takes that start back. Tick
            thread. */
        virtual void stopTimecode (std::uint64_t, std::int64_t, bool) {}

        /*  TX OFF ON THE MTC PORT (AHR): nothing leaves while muted, and the
            code picks up again at the next cycle with a full frame (AHO). */
        virtual void muteTimecode (bool) {}

        /*  WHERE GO.DOT'S SAMPLES WERE AND WHEN, once a tick: the pairs the
            sending thread draws its line between samples and moments from
            (clock/ClockEstimate.h). `nanos` is the machine's steady clock;
            the last, where the show will stand once held (PRD §3.34), or -1. */
        virtual void feedClock (std::int64_t, std::int64_t, int, std::int64_t = -1) {}

        /*  The interface's output latency in samples, by which MTC is held
            back so it leaves when its sample is heard (AHP). */
        virtual void setTimecodeOutputLatency (int) {}

        /*  The readouts (§59.3): messages that left more than 2 ms after their
            moment or were dropped for lateness (`sync/mtcLate`), the latest of
            any in milliseconds (`sync/mtcWorst`), and the program sending now,
            nought for none. Any thread. */
        virtual std::uint32_t timecodeLate() const { return 0; }
        virtual double timecodeWorstMs() const { return 0.0; }
        virtual std::uint64_t timecodeProgram() const { return 0; }
    };
}
