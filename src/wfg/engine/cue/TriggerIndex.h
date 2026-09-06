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

/*  WHAT FIRES A CUE WITHOUT SOMEBODY PRESSING ANYTHING.

    §3.7 lets a cue carry triggers: a message on Go.dot's own OSC port, a MIDI
    event on an input it was told to open, a time of day. GO is not one of them.
    GO is the operator, and it is the only thing that moves the standby (§3.5) -
    a trigger fires a cue and leaves the pointer exactly where it was, which is
    the whole reason a background list can be driven by something other than a
    person without the person losing their place.

    WHY THIS IS A SEPARATE, IMMUTABLE THING RATHER THAN A LOOKUP IN THE DOCUMENT.

    The matching happens where the input arrives, and none of those places is
    the tick thread: a UDP datagram is decoded on a socket thread, a MIDI event
    on a driver callback thread, and the wall clock is read in the serve loop.
    The document belongs to the tick thread and is not safe to read from any of
    them - which is exactly the rule the parameter tree already lives by, and
    this is the same answer applied to triggers: an immutable snapshot,
    published when the document changes and read by anybody, for ever, without
    a lock.

    AND IT MAKES THE MATCHERS PURE. `matchOsc`, `matchMidi` and `clockCrossings`
    are functions of an index and an input, with no socket, no port, no clock
    and no engine anywhere in them - so they can be tested exhaustively in a
    unit suite on a machine with no MIDI interface, which is every CI runner.
    What is left in the wiring is the part that cannot be tested that way, and
    it is three lines each.
*/

#pragma once

#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <memory>
#include <string>
#include <vector>

namespace wfg::cue
{
    /** The kinds of thing that can fire a cue. GO is deliberately not one. */
    namespace triggerKind
    {
        inline constexpr const char* osc   = "osc";
        inline constexpr const char* midi  = "midi";
        inline constexpr const char* clock = "clock";
    }

    /** MIDI event types a trigger can listen for - the four a surface sends. */
    namespace triggerType
    {
        inline constexpr const char* noteOn        = "noteOn";
        inline constexpr const char* noteOff       = "noteOff";
        inline constexpr const char* programChange = "programChange";
        inline constexpr const char* controlChange = "controlChange";
    }

    /*  One trigger, flattened: everything a matcher needs and nothing it does
        not. The cue is here so that a match answers with what to fire without
        a second lookup in a document the matching thread may not read. */
    struct Trigger
    {
        std::string id;
        std::string cue;
        std::string kind;
        bool enabled = true;

        //  osc
        std::string address;
        std::string value;          ///< an atom as the log writes one, or empty for any

        //  midi
        std::string port;           ///< a declared port, or empty for any input
        int channel = 0;            ///< 1..16, or 0 for any
        std::string type;
        int number = 0;
        int data = -1;              ///< velocity or value, or -1 for any

        //  clock
        int secondOfDay = -1;       ///< parsed from `at`; -1 when it is unusable
    };

    /*  A MIDI event, said in the engine's own terms rather than in JUCE's.

        The matcher takes this rather than a `juce::MidiMessage` so that the
        engine's cue layer needs no JUCE audio headers to be tested, and so that
        a test can state the case it means - "note on, channel 3, note 60,
        velocity 0" - instead of building a message and hoping. The wiring
        converts, in four lines, where the driver hands one over. */
    struct MidiEvent
    {
        std::string port;
        std::string type;
        int channel = 1;
        int number = 0;
        int data = 0;
    };

    /*  Every trigger in the show, in document order, immutable once built.

        Held by shared_ptr and swapped whole, exactly as the tree snapshot is:
        a reader takes a copy of the pointer and is then reading something
        nothing can change under it. */
    struct TriggerIndex
    {
        std::vector<Trigger> triggers;

        /** Reads them out of a document. Tick thread only. */
        static std::shared_ptr<const TriggerIndex> build (const doc::ShowDocument& document);
    };

    //==========================================================================
    /*  THE MATCHERS, and they are the whole of the interesting part.

        Each answers with the identifiers of the triggers that fired, in
        document order, so that a show with two triggers on one address fires
        both - which is what a list of triggers means. */

    /** The triggers a message on Go.dot's own OSC port fires. */
    std::vector<std::string> matchOsc (const TriggerIndex& index,
                                       const std::string& address,
                                       const std::vector<osc::Value>& args);

    /** The triggers a MIDI event fires. */
    std::vector<std::string> matchMidi (const TriggerIndex& index, const MidiEvent& event);

    /*  The clock triggers crossed between two readings of the wall clock, each
        a second of the day, 0..86399.

        BETWEEN, rather than "equal to now", because a tick is 20 ms and a
        second is fifty of them: asking whether the clock reads 19:30:00 would
        fire a cue fifty times, and asking on a tick that happened to be late
        would miss it. A crossing is asked about an interval, which is the shape
        that is right at any rate and for any lateness.

        Midnight is handled: `previous` greater than `now` is the day turning
        over, and the interval is then the two ends of it. */
    std::vector<std::string> clockCrossings (const TriggerIndex& index,
                                             int previousSecondOfDay, int nowSecondOfDay);

    /*  `HH:MM:SS` as a second of the day, or -1 if it is not one. Public
        because the load-time check and the index both ask it. */
    int secondOfDayFor (const std::string& text);

    /*  Whether an OSC argument matches what a trigger asked for, where the
        trigger's `value` is an atom written the way the log writes one -
        `f:1`, `i:3`, `T`, `s:go`. Empty matches anything, including a message
        with no arguments at all.

        THE LOG'S SPELLING, deliberately, and not a second syntax: an operator
        who has read one line of a log knows how to write one of these, and the
        parser is the one the log reader already has. */
    bool valueMatches (const std::string& wanted, const std::vector<osc::Value>& args);
}
