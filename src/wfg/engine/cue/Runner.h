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
    GO, and everything that has to be true for it to make a sound on time.

    THE SHAPE. A cue reaching standby is ARMED: a voice is reserved and its
    media is made ready, which is slow, touches a Tracktion ValueTree and
    therefore happens on the message thread. GO is then only a placed instant -
    two atomic stores - which is what lets PRD §4.1 say GO never blocks and mean
    it. The work is done before the operator's hand moves, not after.

    THE LAUNCH INSTANT IS A SAMPLE, DECIDED BY GO.DOT. Not "as soon as
    possible": a launch placed at a beat that has already passed does not simply
    start late, because Tracktion renders the block in hand from the head of the
    file and only back-dates the blocks after it - so the cue is late AND has a
    hole in it. Placing it far enough ahead is therefore a correctness
    requirement, and how far is arithmetic rather than taste. See
    launchLatencyTicks.

    WHY THE RUNNER OBSERVES RATHER THAN IS TOLD. Tracktion has no callback that
    would reach the tick thread safely, so the Runner polls the launch handles
    once a tick and turns edges into commands: run.started, run.ended. It does
    it BEFORE the tick's commands are drained, so what it saw is applied on the
    tick it saw it - from the after hook the log would say every cue started one
    tick after it did, faithfully, for ever.

    NOTHING HERE TOUCHES TRACKTION DIRECTLY. The Runner holds a Player, which is
    the whole of the audio side as the cue layer sees it: no Tracktion type, and
    a null Player is a complete implementation. That is what makes `wfg replay`
    reproduce a performance on a machine with no sound card - the Runner runs,
    the same commands are applied, and only the sound is missing.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>

#include <cstdint>
#include <string>

namespace wfg
{
    class Engine;
}

namespace wfg::cue
{
    /*  HOW FAR AHEAD A LAUNCH MUST BE PLACED, in ticks.

        Derivation, because the number is not obvious and the cost of getting it
        wrong is a hole in every cue:

          - The tick thread wakes when the sample counter has REACHED a tick, so
            what it observes can be up to blockSize - 1 samples past it.
          - A block may already be in flight when the launch is queued, so the
            instant must clear one whole block beyond that.
          - The audio thread reads the queue through a try-lock and simply does
            not see a queued launch on a block where it fails, so a second block
            must be cleared too.

        Requiring `ticks * samplesPerTick - (blockSize - 1) >= 2 * blockSize`
        gives `ticks >= ceil((3 * blockSize - 1) / samplesPerTick)`, and one more
        tick is added as a guard against a tick thread that overslept.

        The plan's rule - one tick plus the blocks a tick spans - agrees at
        small block sizes and is WRONG from 1024 up, where it leaves less than
        one block of clearance. Measured rather than argued: at 48 kHz with
        1024-sample blocks it gives 1857 samples of lead where 2048 are needed.
    */
    int launchLatencyTicks (int blockSize, int samplesPerTick) noexcept;

    //==============================================================================
    /*  The audio side, as the cue layer sees it.

        NAMES NO TRACKTION TYPE, deliberately: this header is included by the
        command layer and by tests, and a null implementation is a complete one.
        A show replayed with no Player still creates runs, still advances
        standby, still writes the same log - it just makes no sound.
    */
    class Player
    {
    public:
        virtual ~Player() = default;

        /** The polyphony ceiling. Zero when the show has no audio. */
        virtual int trackCount() const = 0;

        /*  Makes a track ready to play a file, and RETURNS IMMEDIATELY. The
            work is a graph rebuild and a wait on the disk, so it happens
            somewhere else; the implementation reports completion by submitting
            `audio.armed <run> <track>`, which is what moves the run on.

            Called from the tick thread. It must not block there. */
        virtual void requestArm (const std::string& runId, int track,
                                 const std::string& mediaFile) = 0;

        /*  Places a launch at one of Go.dot's own sample positions. Tick
            thread, and the whole of what GO does to the audio side. */
        virtual bool launchAtSample (int track, std::int64_t sample) = 0;

        /** Stops a track's cue now. Tick thread. */
        virtual bool stop (int track) = 0;

        /** Whether that track's cue is sounding. Tick thread. */
        virtual bool isPlaying (int track) const = 0;

        /** Go.dot's sample counter, now. Tick thread. */
        virtual std::int64_t samplesElapsed() const = 0;

        /** Samples per audio block, for the launch-instant arithmetic. */
        virtual int blockSize() const = 0;
    };

    //==============================================================================
    /*  Owns what happens between a cue and a sound.

        Tick thread only, all of it. The Player is null by default, which is a
        working configuration and not a degraded one.
    */
    class Runner
    {
    public:
        Runner (const doc::ShowDocument& document, RunTable& runs,
                doc::IdRegistry& runIds, Focus& focus);

        /** Null is legal and means a show with no audio side. */
        void setPlayer (Player* player) noexcept { audio = player; }
        Player* player() const noexcept          { return audio; }

        /** How many samples make a tick. Set once, from the tick schedule. */
        void setSamplesPerTick (int samples) noexcept { samplesPerTick = samples; }

        /** The published `/godot/engine/launchLatencyTicks`, or 0 with no audio. */
        int latencyTicks() const noexcept;

        /*  Called on the tick thread immediately before the tick's commands are
            drained, so that what it observed is applied on the tick it observed
            it. Everything it wants to change, it changes by submitting. */
        void beforeTick (Engine& engine, std::int64_t tick);

        //======================================================================
        /*  What `go` and `cue.fire` do, once the command layer has decided
            which cue. Returns the run identifier that was launched or created,
            empty when the cue is not one that plays.

            `runId` is the identifier to use when one has to be created - the
            command layer draws it so that the log record carries it. */
        std::string fire (Engine& engine, const std::string& cueId,
                          const std::string& runId);

        /*  Arms a cue without firing it: the standby path. Same return. */
        std::string arm (Engine& engine, const std::string& cueId,
                         const std::string& runId);

    private:
        std::string armInternal (Engine& engine, const std::string& cueId,
                                 const std::string& runId, bool fireAtOnce);

        void launchIfDue (Engine& engine, std::int64_t tick);
        void observeEdges (Engine& engine);

        const doc::ShowDocument& document;
        RunTable& runs;
        doc::IdRegistry& ids;
        Focus& focus;

        Player* audio = nullptr;
        int samplesPerTick = 0;
    };

    //==============================================================================
    /*  Adds `go` and `cue.fire`, both bound to `runner`.

        `go` fires the focused list's standby and ADVANCES it (§3.5);
        `cue.fire` fires a named cue and leaves standby alone (§4.11), which is
        what a button on a surface does.
    */
    void registerGoCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                             doc::ShowDocument& document, Focus& focus,
                             doc::IdRegistry& runIds);
}
