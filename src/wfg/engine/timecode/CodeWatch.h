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
    WATCHING AN INCOMING CODE, AND SAYING ONLY ITS TURNS (namespace draft §60,
    TG.5; AIL, AIO, AIQ, AIR).

    TG.3 and TG.4 turn LTC and MTC into observations - this frame began on this
    Go.dot sample. A watcher, one a source, turns them into the one thing the
    Runner acts on, a code map (CodeMap.h), and decides when the map the log
    holds has to change: only at the TURNS, each one `code.heard` record, so a
    replay - no audio, no MIDI - and a backup following its main rebuild the
    same map from a few records an hour (AIO).

    THE LINE. Every observation it believes goes into a window of the last
    second, and a straight line - frame against sample - is fitted through it
    by least squares, its jitter measured from what is left over. The line is
    where the code is and how fast it runs against Go.dot's clock; the jitter
    says how far either can be trusted.

    THE TURNS, each written as a record:
      - `jumped`: the code appeared, came back after a stop, or LOCATED - a
        frame more than two frames off the line (`locateFrames`). A frame off
        the line is not believed on its own: it waits for a second frame, and
        two frames that agree with each other - the later further on by a
        plausible speed, a quarter to four times nominal - are a code that
        really is there (AIR). One corrupt frame, followed by one back on the
        line, never locates; it is forgotten.
      - `stopped`: nothing heard for four frames beyond when the next was due,
        plus `grace` for how late a frame reaches the tick (AIR's freewheel -
        the detector's: chasers keep playing, AIG). A dropout shorter than four
        frames is invisible.
      - `running`: the line as it is now differs from the line LAST WRITTEN by
        more than half a millisecond for LTC, a millisecond for MTC (AIO) - a
        code drifting against Go.dot's clock, or one whose speed changed - or
        the rate it is read at became clearer. Otherwise nothing is written,
        which at fifty parts per million is a handful of records an hour.

    EACH WATCHER KEEPS ITS OWN COPY OF WHAT IT LAST WROTE - never the Runner's
    map - so a backup following its main, whose own records the gate drops
    (AGD), writes as sparsely as the main does, and at a takeover it has a line
    of its own to say at once (`resay`).

    AND IT WRITES ONLY FOR A TIME GROUP (TG.11's faults, part 2): serve tells
    it whether the show holds one on its code (`setWriting`), and while it does
    not, the watcher watches and says nothing - its map is what the readouts
    show (`map`) - and says what it knows at once when one appears.

    THE SPEED A RECORD CARRIES is measured twice and the better kept. A second
    of observations gives a slope that MTC's millisecond of driver jitter
    leaves hundreds of parts per million uncertain; but a code that drifted a
    millisecond from the line written twenty seconds ago has shown its speed
    over twenty seconds, ten times better. At a re-anchor the two are weighed
    by how far each can be trusted - unless they disagree beyond their errors,
    when the code's speed has changed and the last second is what it is now.
    And a speed that cannot be told from one - a code locked to Go.dot's word
    clock - is written as exactly 1, within a part per million or three times
    its own error, whichever is more, so the map is FrameClock's exact rule
    (CodeMap.h).

    THE RATE (AIL). A frame is numbered from its label at the rate the code
    COUNTS by - what its labels count to and whether they drop - and the map's
    rate tells 23.976 from 24 and 29.97 from 30 by the frames' measured period
    (`detectRate`), once the speed is known to a hundred parts per million.
    LTC's labels say what they count to once a second has wrapped; until then,
    and when the period cannot tell, the show's `sync/frameRate` is taken if
    the labels allow it, and otherwise the plain rate they count by. MTC's
    rate code says how it counts at once. A code whose counting turns out other
    than it was read at is read again from its next two frames: `jumped`.

    LATENCY (AIQ). What a member must match is the code as it is HEARD. An LTC
    frame's sample is when it reached the INPUT: the interface took
    `audio/inputLatency` to hand it over, and a sound Go.dot starts on a sample
    is heard `audio/outputLatency` after it, so the sample the code is heard
    to reach a frame on is that frame's sample less both. An MTC piece's
    sample is the one playing when it arrived - nothing of the input in it -
    so it is shifted by the output latency alone. The shift is made in the
    record, so a replay never needs either latency; the watcher's own line
    stays on the samples it heard, and a latency changed re-anchors it.

    THE TICK THREAD'S, AND PURE: no clock of its own, no JUCE, no document.
    Fed in order, asked once a tick; its window allocates as it grows to a
    second of observations and reuses that.
*/

#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/timecode/CodeMap.h>
#include <wfg/engine/timecode/LineFit.h>
#include <wfg/engine/timecode/LtcDecoder.h>
#include <wfg/engine/timecode/MtcReader.h>
#include <wfg/engine/timecode/Timecode.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::timecode
{
    //==========================================================================
    /*  A TURN OF A CODE, as `code.heard` says it. */
    enum class CodeTurn
    {
        jumped,
        running,
        stopped
    };

    std::string_view wordOf (CodeTurn turn) noexcept;
    std::optional<CodeTurn> codeTurnFromWord (std::string_view word) noexcept;

    //  What a watcher says: the turn, and the map from then on, latency shifted.
    struct CodeSaid
    {
        CodeTurn what = CodeTurn::jumped;
        CodeMap map;
    };

    //==========================================================================
    /*  `code.heard s:source s:what s:rate h:frame h:sample d:speed [s:run]...`
        (namespace draft §60.4): where each argument is, so the follower that
        moves a main's sample onto this machine's clock (pair/Follow.cpp) and
        the handler read the same places; the run names TG.8's re-seats hand
        back go after the sixth. */
    namespace codeheard
    {
        inline constexpr std::size_t sourceArg = 0;
        inline constexpr std::size_t whatArg = 1;
        inline constexpr std::size_t rateArg = 2;
        inline constexpr std::size_t frameArg = 3;
        inline constexpr std::size_t sampleArg = 4;
        inline constexpr std::size_t speedArg = 5;
        inline constexpr std::size_t firstRunArg = 6;

        inline constexpr const char* command = "code.heard";
    }

    /*  A source a code is read from, as a record names it and an origin spells
        it: `ltc:<input>`, `mtc:<port>`, or `clock`. */
    bool isCodeSource (std::string_view source) noexcept;

    std::vector<osc::Value> codeHeardArgs (const std::string& source, const CodeSaid& said);

    /*  A `code.heard` record's arguments read back: its source, its turn and
        the map it says, on a clock of `sampleRate` samples a second. Nothing
        for a source, a turn or a rate that is not one, a frame outside the
        day, a speed that is not a number, or an LTC or MTC source with the
        wall clock's unit (or the other way round). */
    struct CodeHeard
    {
        std::string source;
        CodeTurn what = CodeTurn::jumped;
        CodeMap map;
    };

    std::optional<CodeHeard> readCodeHeard (const std::vector<osc::Value>& args, int sampleRate);

    //==========================================================================
    class CodeWatch
    {
    public:
        enum class Kind
        {
            ltc,
            mtc
        };

        explicit CodeWatch (Kind kindToUse) noexcept;

        //======================================================================
        /*  Go.dot's samples a second. A change forgets everything: the samples
            heard so far counted something else. */
        void setSampleRate (int rate) noexcept;

        //  The show's `sync/frameRate`, which a code is read at when nothing better says (AIL).
        void setShowRate (Rate rate) noexcept;

        /*  How far before the sample a code was taken on it is HEARD (AIQ): LTC
            the interface's input and output latency, MTC its output latency. */
        void setLatency (std::int64_t samples) noexcept;

        /*  How late a code can reach the tick after it was heard - a block and a
            tick - before its silence counts towards a stop. */
        void setGrace (std::int64_t samples) noexcept;

        /*  Everything forgotten - the line, what was written, the rate - and the
            code absent: its source changed, or the line it came on restarted. */
        void forget() noexcept;

        /*  WHETHER ITS TURNS ARE WRITTEN: while the show holds a time group on
            this code (namespace draft §60.7, TG.11's faults, part 2). A code is
            written into the log for what reads it - a time group firing and
            chasing on it - and a code arriving at a show that has none is not
            something anyone decided, so it is not the log's (PRD §4.10). Off,
            the watcher goes on exactly as before - the line, its turns, what it
            would have written, which the readouts show (`map`) - and says
            nothing, not even at a takeover. On again, the next `update` says
            at once what it knows, as a takeover's `resay` does: `jumped` on its
            line while the code runs, `stopped` where it stopped, nothing for a
            code never heard - the Runner's map, last written while a group
            was there, or never, is no longer the code's. On by default. */
        void setWriting (bool on) noexcept;

        //======================================================================
        /*  FED IN ORDER, on the tick: an LTC frame as the decoder read it (a
            `lost` entry says nothing - the four-frame rule decides a stop - and
            a new generation is a line started again: forgotten first); an MTC
            observation as the reader made it; and the sample any piece of code
            last arrived on, which keeps a code that is still coming alive while
            its cycles are being put together. */
        void hear (const LtcHeard& frame);
        void hear (const MtcObservation& observation);
        void heardAt (std::int64_t sample) noexcept;

        /*  ONCE A TICK, after the feeding, at Go.dot's sample `now`: the turn to
            write, if this tick has one. At most one. */
        std::optional<CodeSaid> update (std::int64_t now);

        /*  A TAKEOVER (namespace draft §58, RD.7; AIP): what this watcher knows,
            said in full whatever it last wrote - `jumped` with its own line
            while the code runs, `stopped` where it stopped; nothing when it has
            never heard the code. */
        std::optional<CodeSaid> resay();

        //======================================================================
        //  What it last said: absent until the code is first believed.
        CodeState state() const noexcept { return current; }

        /*  THE MAP IT LAST DECIDED ON, latency shifted as a record carries it,
            written or not: absent until the code is first believed. What the
            readouts show of a code the show holds no time group on - the
            Timecode tab shows a code arriving whether or not anything reads it. */
        CodeMap map() const;

        //  The jitter about the line, in samples, measured over the window; nought until three points.
        double jitterSamples() const noexcept;

        //  The observations the line is fitted through now.
        std::size_t windowSize() const noexcept { return window.size(); }

        //======================================================================
        static constexpr double windowSeconds = 1.0;
        static constexpr double locateFrames = 2.0;
        static constexpr double freewheelFrames = 4.0;
        static constexpr double ltcDriftSeconds = 0.0005;
        static constexpr double mtcDriftSeconds = 0.001;

        //  Within this of one, or three times its own error, a speed is written as exactly one.
        static constexpr double unitTolerance = 1.0e-6;

        //  The speeds two frames may imply and still be one code: a quarter of nominal to four times it.
        static constexpr double slowest = 0.25;
        static constexpr double fastest = 4.0;

        /*  The jitter assumed when too few points have come to measure it - two
            LTC frames, a cycle's first pieces - and the least ever assumed, so a
            perfect line still has an error to weigh. LTC's edges are placed to a
            hundredth of a sample (TG.3); MTC's arrival is a driver's and a
            millisecond or so. */
        static constexpr double ltcJitterSamples = 0.05;
        static constexpr double ltcJitterFloorSamples = 0.005;
        static constexpr double mtcJitterSeconds = 0.0005;
        static constexpr double mtcJitterFloorSeconds = 0.00005;

    private:
        /*  A point's position is in frames, unwrapped, at the counting rate;
            the line's slope frames per sample, its residual in frames
            (LineFit.h, shared with the wall clock's watcher). */
        using Point = linefit::Point;
        using Line = linefit::Line;

        //  The line as last written: the map without its latency shift, and its slope and error.
        struct Written
        {
            bool valid = false;
            CodeMap map;                    // its sample the one HEARD on, before the shift
            double position = 0.0;          // the anchor frame, unwrapped as the window is
            double began = 0.0;             // where the line put that frame, before the anchor's sample was rounded up
            double slope = 0.0;             // frames per sample, as written
            double positionError = 0.0;     // frames, at the anchor, when it was written
            std::int64_t latency = 0;
        };

        void restartCounting (Rate counting) noexcept;
        void noteField (int field) noexcept;
        Rate ltcCounting (bool drop) const noexcept;

        void place (double sample, double frameOfDay);
        void candidate (Point point);
        void confirm();

        void trim();
        void fit();
        double sigmaFrames() const noexcept;
        double slopeError() const noexcept;
        double positionError (double atSample) const noexcept;

        double nominalSlope (Rate rate) const noexcept;
        Rate rateFor (double slope, double error) const noexcept;
        double snapped (double slope, double error, Rate rate) const noexcept;

        CodeSaid say (CodeTurn turn, double slope, Rate rate);
        CodeSaid sayJump();
        CodeSaid sayStopped();
        CodeSaid shifted (const CodeMap& raw, CodeTurn turn) const;

        //  `update` and `resay` whether or not their turns are written.
        std::optional<CodeSaid> turnAt (std::int64_t now);
        std::optional<CodeSaid> resayOwn();

        Kind kind;
        int sampleRate = 0;
        Rate showRate = Rate::fps25;
        std::int64_t latency = 0;
        std::int64_t grace = 0;

        //  How the frames are numbered now - what the labels count to, and whether they drop.
        Rate countingRate = Rate::fps25;
        bool haveCounting = false;

        //  LTC: the frames field's largest, the one before, and the nominal a second's wrap showed.
        int largestField = -1;
        int previousField = -1;
        int knownNominal = 0;
        bool generationSeen = false;
        std::uint64_t generation = 0;

        CodeState current = CodeState::absent;
        bool locked = false;
        bool pendingJump = false;
        bool jumpWhileRunning = false;

        //  Whether turns are written, and whether what it knows is owed at once because they now are.
        bool writing = true;
        bool owedAtOnce = false;

        std::deque<Point> window;
        std::vector<Point> candidates;
        Line live;
        Written written;

        //  The sample a piece of the code was last heard whole on, and the last point believed.
        std::int64_t lastHeard = -1;
        Point lastPoint {};
        bool havePoint = false;
    };
}
