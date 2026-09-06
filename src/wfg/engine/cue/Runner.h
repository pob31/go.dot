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
#include <wfg/engine/cue/FadeJob.h>
#include <wfg/engine/cue/GroupJob.h>
#include <wfg/engine/cue/OscJob.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wfg
{
    class Engine;
}

namespace wfg::tree
{
    class MountProbe;
    class MountSender;
    class MountTable;
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

    /*  Seconds as the document spells them, in ticks as the engine counts them.

        ONE PLACE, because there were two and they were both the literal 50. A
        tick rate that is a constant in TickClock and a magic number in the cue
        layer is a rate that only appears to be defined in one place, and the
        rounding rule - nearest, so a 0.02 s wait is one tick rather than none -
        is worth stating once as well.

        Negative seconds are not a wait. The schema already refuses them
        (`preWait` and `postWait` are `0..`), so this is the second net rather
        than the first. */
    int ticksFor (double seconds) noexcept;

    //==============================================================================
    /*  One coefficient of a cue's output stage: from one of the cue's own
        channels to one HARDWARE output channel, at a gain.

        ABSOLUTE OUTPUT CHANNELS, resolved before this struct exists. The
        document says a destination in terms of a bus, because a bus is where
        the author said a channel exists and a show moved to another rig
        re-points buses rather than every cue. The Runner does that resolution -
        it has the document - so the audio side never has to know what a bus is.
    */
    struct Coefficient
    {
        int input = 0;
        int output = 0;
        float gain = 0.0f;
    };

    /*  Everything the audio side needs to make one cue ready.

        A VALUE, carrying no document reference, because it crosses a thread
        boundary: the tick thread fills it in and the message thread acts on it,
        and a reference into a ValueTree that the tick thread may edit meanwhile
        is exactly the bug that would be found on a show night.
    */
    struct ArmRequest
    {
        std::string runId;
        int track = -1;
        std::string mediaFile;

        /** The cue's authored level, in dB. Not what a fade will write. */
        double levelDb = 0.0;

        /** Where it goes. Empty is legal and means a cue routed nowhere yet. */
        std::vector<Coefficient> routing;
    };

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

        /*  Makes a track ready to play a cue, and RETURNS IMMEDIATELY. The work
            is a graph rebuild and a wait on the disk, so it happens somewhere
            else; the implementation reports completion by submitting
            `audio.armed <run> <track>`, which is what moves the run on.

            Called from the tick thread. It must not block there. */
        virtual void requestArm (const ArmRequest&) = 0;

        /*  Places a launch at one of Go.dot's own sample positions. Tick
            thread, and the whole of what GO does to the audio side. */
        virtual bool launchAtSample (int track, std::int64_t sample) = 0;

        /** Stops a track's cue now. Tick thread. */
        virtual bool stop (int track) = 0;

        /*  Stops it at one of Go.dot's own sample positions, the way a launch
            is placed. Tick thread.

            Tracktion treats a queued stop exactly as it treats a queued play -
            a beat inside the block splits the block to the sample, a beat
            already past stops for the whole block - so a stop is as placeable
            as a start, which is what lets a hard stop land where the show says
            rather than wherever the next block happened to begin. */
        virtual bool stopAtSample (int track, std::int64_t sample) = 0;

        /*  The level a track's cue is playing at, in dB. Tick thread, once per
            tick while a fade runs, and one relaxed atomic store. */
        virtual void setLevelDb (int track, double levelDb) = 0;

        /** Whether that track's cue is sounding. Tick thread. */
        virtual bool isPlaying (int track) const = 0;

        /*  Whether the media for that track is actually ready to sound.

            SEPARATE FROM THE ARM BEING ACCEPTED, and the separation is the
            point. Assigning a voice and rebuilding the graph is quick; getting
            the file mapped into the audio cache is a disk, and firing a cue
            before that plays silence for as long as the disk takes with the run
            reporting itself as playing throughout. Asked once a tick rather
            than waited on, so nothing blocks. */
        virtual bool isArmReady (int track) const = 0;

        /** Go.dot's sample counter, now. Tick thread. */
        virtual std::int64_t samplesElapsed() const = 0;

        /** Samples per audio block, for the launch-instant arithmetic. */
        virtual int blockSize() const = 0;

        /** How many channels a track carries, which is a cue's input width. */
        virtual int channelsPerTrack() const = 0;
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

        /*  Where the show's media lives: the bundle's `media/` folder.

            A cue names its file RELATIVE to that, because a show travels
            between machines and an absolute path is a fact about the one it was
            authored on. Resolving it here rather than in the audio side keeps
            the Player free of any idea what a bundle is - it is handed a path
            that exists, or the run fails before it gets there. */
        void setMediaFolder (std::string folder) { mediaFolder = std::move (folder); }

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
            command layer draws it so that the log record carries it.

            `tick` IS THE COMMAND'S OWN TICK and not the Runner's, because a
            pre-wait is a deadline and a deadline computed during a replay must
            land where it landed live. The hooks that set `currentTick` do not
            run in a replay; `CommandContext::tick` is right in both. */
        std::string fire (Engine& engine, std::int64_t tick, const std::string& cueId,
                          const std::string& runId);

        /*  Arms a cue without firing it: the standby path. Same return. */
        std::string arm (Engine& engine, std::int64_t tick, const std::string& cueId,
                         const std::string& runId);

        /*  A run whose pre-wait has elapsed, doing what firing it would have
            done had there been no wait. What the `run.fire` command calls.

            SEPARATE FROM `fire` because by now the run exists, its identifier
            is in the log, and its cue may have been edited since - so this
            takes a RUN and not a cue, and re-reads nothing that was already
            decided. */
        void fireNow (Engine& engine, std::int64_t tick, const std::string& runId);

        /*  A group spawning one of its members: the child run is created,
            armed if it is media, and left ready. What `run.spawn` calls.

            SPAWNING IS NOT LAUNCHING, and the two are separate records because
            they are separate moments. An auto sequence spawns the next member
            while the current one is still playing - which is what pays the disk
            before the chain reaches it - and launches it when the current one
            reports done. Returns the child's run identifier. */
        std::string spawnChild (Engine& engine, const std::string& parentRun,
                                const std::string& cueId, const std::string& runId);

        /*  A run that was spawned and is now to begin: its pre-wait starts, or
            it fires at once when it has none. What `run.launch` calls. */
        void launchRun (Engine& engine, std::int64_t tick, const std::string& runId);

        /** Every group in flight. Diagnostics and tests; the Runner drives them. */
        const std::vector<GroupJob>& groups() const noexcept { return scheduled; }

        /** Whether the run table has this identifier. What `run.fire` asks
            before it acts, so that an unknown one is REJECTED rather than
            quietly doing nothing. */
        bool knowsRun (const std::string& runId) const { return runs.find (runId) != nullptr; }

        /*  Where a cue's destinations land on the rig, resolved through the
            buses the show declares.

            Public because it is worth testing on its own: it is the one piece
            of arithmetic between "the designer said main and foldback" and "the
            matrix multiplies these numbers", and getting it wrong sends a cue
            somewhere nobody asked for.

            `problem` is empty when it resolved. It is filled in rather than
            thrown because a cue that cannot be routed fails its RUN - the
            request was legal and the show cannot honour it - and never the
            load. */
        std::vector<Coefficient> resolveRouting (const juce::ValueTree& cue,
                                                 int trackChannels,
                                                 std::string& problem) const;

        /** Every fade in flight. Diagnostics and tests; the Runner drives them. */
        const std::vector<FadeJob>& fades() const noexcept { return running; }

        /*  The mounted namespaces and the socket that serves them, which is
            what a network cue needs and nothing else does.

            BOTH NULL IS A COMPLETE CONFIGURATION, exactly as a null Player is:
            `wfg replay` has no socket and must still create the run, advance
            standby and write the same log - only the datagram is missing. They
            are two pointers and not one because a table with no sender is also
            real (a tree dump reads mounts and sends nothing), while a sender
            with no table has nothing to address. */
        void setMounts (tree::MountTable* table, tree::MountSender* sender,
                        tree::MountProbe* probe = nullptr) noexcept
        {
            mounts = table;
            sender_ = sender;
            asker = probe;
        }

        /** Every network cue in flight. Diagnostics and tests. */
        const std::vector<OscJob>& sends() const noexcept { return sending; }

    private:
        std::string armInternal (Engine& engine, std::int64_t tick,
                                 const std::string& cueId,
                                 const std::string& runId, bool fireAtOnce);

        /*  A CUE'S ATTRIBUTE, WITH ITS DEFAULT APPLIED.

            Not `cue[Identifier (name)]`, which is what every one of these reads
            used to be and is wrong in a way that only shows on a cue somebody
            has not filled in. The canonical writer OMITS an attribute holding
            its default and the reader leaves it absent, so an untouched cue has
            no such property at all - and asking the ValueTree answers with the
            type's zero. For most rows in the table that IS the default and it
            looks like it works. For `fade/@level` (-120) and `osc/@timeout` (5)
            it is not, and both are quiet: a fade nobody filled in would have
            gone UP to unity, and a verified cue would have given up before it
            asked.

            `ShowDocument::getAttribute` resolves the row and supplies the
            default, which is what having one door is for. */
        std::string textOf (const juce::ValueTree& cue, const char* name) const;
        double numberOf (const juce::ValueTree& cue, const char* name) const;

        /*  The kind's own fire path, once every wait is out of the way: a media
            cue asks for a voice, a fade or a stop takes over a level, a network
            cue writes a node, a memo has nothing to do and says so next tick. */
        void fireKind (Engine& engine, std::int64_t tick, const juce::ValueTree& cue,
                       const std::string& kind, const std::string& runId);

        /*  A media cue reserving a voice and asking for its media. Reports
            `run.failed` when the show cannot honour it, which it may do because
            it is reached from a hook or from a handler that a replay runs with
            no audio side - see the note in armInternal. */
        void armMedia (Engine& engine, const juce::ValueTree& cue,
                       const std::string& runId);

        /*  A fade or a stop cue firing. Both act on a run that already exists,
            which is what makes them different from a media cue: they create a
            run of their own to report what they did, and they change one that
            somebody else started.

            NO ENGINE, and that is the signature carrying a rule rather than an
            omission. These three run inside a command handler, and a handler
            that reported would produce a record twice on replay - once from the
            log and once from itself. Not being able to reach the engine is how
            that stays true when somebody adds the next case. */
        void fireFade (const juce::ValueTree& cue, const std::string& runId);
        void fireStop (const juce::ValueTree& cue, const std::string& runId);

        /*  A network cue firing: one write to a mounted node, queued for the
            end of this tick. No Engine here either, and for the same reason. */
        void fireOsc (const juce::ValueTree& cue, const std::string& runId);

        /*  `selfCueId` is the fade or stop cue being fired; `targetCueId` is
            the cue it acts on. They are two arguments and not one because the
            run being created belongs to the FIRST - a run says which cue it
            instantiates - while the level being moved belongs to the second.
            Conflating them made liveRunOf answer with the fade's own run. */
        /*  What the fades already on a target meant, once they have been
            resolved and removed. Two answers, deliberately: which runs are over
            is a question about LIFETIME, and which stop is inherited is a
            question about TIME. */
        struct Takeover
        {
            bool keepStopping = false;
            std::int64_t stopsAtTick = 0;
        };

        Takeover resolveTakeover (const std::string& targetId);

        void beginFade (const std::string& selfCueId,
                        const std::string& targetCueId,
                        const std::string& selfRunId, const std::string& kind,
                        double toDb, double seconds, FadeCurve, bool stopWhenDone);

        void advanceFades (Engine& engine, std::int64_t tick);
        void advanceSends (Engine& engine);
        void advanceWaits (Engine& engine, std::int64_t tick);
        void advanceGroups (Engine& engine);

        /** The cues a group runs, in order: its enabled children. */
        std::vector<std::string> membersOf (const juce::ValueTree& group) const;
        void enforceStops();

        void launchIfDue (Engine& engine, std::int64_t tick);
        void observeEdges (Engine& engine);

        const doc::ShowDocument& document;
        RunTable& runs;
        doc::IdRegistry& ids;
        Focus& focus;

        Player* audio = nullptr;
        int samplesPerTick = 0;
        std::string mediaFolder;

        std::vector<FadeJob> running;

        /*  Runs with nothing left to do, ending on the next tick.

            A MEMO IS THE WHOLE POPULATION and it is here rather than nowhere
            because §3.6 needs every kind of cue to report done: a sequence group
            whose second member is a note to the operator has to know when to
            move to the third. It ends on the tick AFTER it fired, exactly as a
            network cue with `wait: none` does, because that is when a report is
            allowed to leave and not because anything was waited for. */
        std::vector<std::string> finishing;

        /*  One per group run in flight. A vector like every other job list
            here, and drained by the same `remove_if` on a retired flag. */
        std::vector<GroupJob> scheduled;

        /*  The tick being processed, so a stop fired inside a command
            handler can be scheduled against the same clock the tick hook
            reads. Set by beforeTick, which runs before the handlers do. */
        std::int64_t currentTick = 0;
        std::vector<OscJob> sending;

        tree::MountTable* mounts = nullptr;
        tree::MountSender* sender_ = nullptr;

        /*  Who asks a target what a value is. Null everywhere a replay or
            a tree dump runs, and a verified cue there finishes on its own
            records rather than on an answer nobody went and got. */
        tree::MountProbe* asker = nullptr;

        /*  Fades taken over by another fade since the last tick, whose runs
            have still to be ended. A queue rather than a submission at the
            takeover, because only the tick hook reports - see advanceFades. */
        std::vector<std::string> supersededRuns;
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
