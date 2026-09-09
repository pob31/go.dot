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
#include <functional>
#include <string>
#include <vector>

namespace wfg
{
    class Engine;
}

namespace wfg::midi
{
    struct MidiSink;
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

    /*  One region of the cue's file: §3.24's range, as the audio side needs
        it. Seconds, because that is what the document says and what a file
        length is; the graph converts. */
    struct RangeSpec
    {
        double in = 0.0;
        double out = 0.0;

        /** Passes before playback moves on. Zero is for ever. */
        int loops = 1;
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

        /*  How far into the file it starts, in seconds - the cue's own
            `startOffset`, which had a row and a grammar and no reader at all
            until PR 4.1.

            Carried in the request like the level rather than read at the far
            end, because the far end is the message thread and the document is
            the tick thread's. It applies only to a cue with no ranges: the
            document refuses an offset beside a range list, since a cue with
            ranges plays its ranges and the offset belongs in the first one's
            `in`. Phase 4's load-to-time writes the same field from the RUN
            rather than from the cue, which is why it is a value here and not a
            second lookup. */
        double startOffset = 0.0;

        /** Where it goes. Empty is legal and means a cue routed nowhere yet. */
        std::vector<Coefficient> routing;

        /*  The cue's ranges, in playlist order, EMPTY BEING THE ORDINARY CASE:
            a cue with no ranges plays its whole file, which is most cues and is
            all of Phase 2.

            Copied into the request rather than read at the boundary, because
            the arm crosses to the message thread and a reference into a
            document the tick thread may edit meanwhile is the bug that gets
            found on a show night. The RE-READING §3.24 asks for - a `loops`
            edit taking effect at the next iteration - happens on the tick
            thread at each boundary, and re-arms through a fresh request. */
        std::vector<RangeSpec> ranges;
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

        /** How many ranges of one cue the graph can hold. One with no ranges. */
        virtual int slotCount() const = 0;

        /*  Places a launch at one of Go.dot's own sample positions. Tick
            thread, and the whole of what GO does to the audio side.

            The slot is which RANGE this is - nought for a cue with none, which
            is what Phase 2 did without having to say so. */
        virtual bool launchAtSample (int track, int slot, std::int64_t sample) = 0;

        /*  Stops a track's cue now, whichever of its ranges is sounding. Tick
            thread.

            TRACK-WIDE ON PURPOSE. A stop cue stops the CUE; which range it had
            reached is not something the caller knows, and making it ask would
            put the range job's bookkeeping into every stop path. */
        virtual bool stop (int track) = 0;

        /*  Stops it at one of Go.dot's own sample positions, the way a launch
            is placed. Tick thread.

            Tracktion treats a queued stop exactly as it treats a queued play -
            a beat inside the block splits the block to the sample, a beat
            already past stops for the whole block - so a stop is as placeable
            as a start, which is what lets a hard stop land where the show says
            rather than wherever the next block happened to begin. */
        virtual bool stopAtSample (int track, int slot, std::int64_t sample) = 0;

        /*  The level a track's cue is playing at, in dB. Tick thread, once per
            tick while a fade runs, and one relaxed atomic store. */
        virtual void setLevelDb (int track, double levelDb) = 0;

        /*  Whether that track's cue is sounding, out of any of its slots.
            Tick thread. Track-wide for the reason `stop` is: the question is
            about the cue. */
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

        /*  Samples a second, which is what turns a range's seconds into the
            boundary arithmetic. Zero with no graph.

            IT IS THE AUDIO SIDE'S NUMBER AND NOT THE SCHEDULE'S. `samplesPerTick`
            times the tick rate would give the same answer while the two agree,
            and would give a wrong one the moment a show ran at a tick rate the
            cue layer had not been told about - which is the kind of thing that
            is discovered by a range being a hundredth too long. */
        virtual int sampleRate() const = 0;

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

        /*  The runs, for the one caller outside the Runner that has to ask
            about them: `go`, whose cursor has to know whether a manual group
            still has rounds to play before it lets the pointer out of it.
            Const, because the table's one writer is the command handler and
            that is the whole point of the arrangement. */
        const RunTable& runTable() const noexcept { return runs; }
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

        /*  Where a MIDI cue's bytes go. Null is legal and is what a replay has:
            the run is created and finishes on the ticks the log says, and
            nothing reaches a port - exactly as a mount table's absence leaves a
            network cue's record reproducing with nothing on the wire. */
        void setMidiSink (midi::MidiSink* sink) noexcept { midiOut = sink; }

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

        /*  What GO does, which is more than firing a cue once the pointer can
            be inside a group.

            A member of a manual sequence group is not a cue that plays on its
            own: it plays AS PART OF the group, so the group's run has to exist
            to be its parent, its header has to have run, and its footer will
            run when the members are done. If the operator's pointer is three
            levels down and none of those groups is live, GO creates the whole
            chain - outermost first - and then the member.

            THE IDENTIFIERS ARE SUPPLIED OR DRAWN, in that order, which is what
            makes it replayable: a `go` record carries EVERY identifier it
            created rather than only one, and a replay hands them back in the
            same order. The record is variadic because the number is a property
            of how deep the pointer was, not a constant. */
        std::vector<std::string> fireStandby (Engine& engine, std::int64_t tick,
                                              const juce::ValueTree& list,
                                              const std::string& cueId,
                                              const std::vector<std::string>& supplied);

        /*  Arms a cue without firing it: the standby path. Same return. */
        std::string arm (Engine& engine, std::int64_t tick, const std::string& cueId,
                         const std::string& runId);

        /*  THE HORIZON REACHING A BLOCK: what `run.prepare` calls.

            PRD §3.12. The pointer landing on a cue is the moment to get ready,
            and getting ready is more than an arm when the cue sits inside a
            scene: every group between the pointer and the list is created,
            OUTERMOST FIRST and each parented to the level above, its run in
            state `preparing` and its job in the phase of the same name - which
            runs whatever of that group's header can be run ahead. Then what the
            innermost would launch first is armed underneath it.

            OUTERMOST FIRST BECAUSE THAT IS THE ORDER A GO WOULD RUN THEM IN. A
            chain prepared inside-out would position a source and then have an
            outer header move it again, and the desk would end up holding the
            wrong one of two correct values.

            IT DOES NOT REACH THE NEXT SIBLING GROUP. §3.12 extends the horizon
            "from one row to a block", and two scenes prepared at once would
            hold two scenes' worth of slots - in a mechanism whose whole subject
            is that slots are scarce.

            Identifiers supplied or drawn, in that order, exactly as
            `fireStandby` does and for the same reason: the record carries every
            one it made and a replay hands them back. */
        std::vector<std::string> prepareStandby (Engine& engine, std::int64_t tick,
                                                 const juce::ValueTree& list,
                                                 const std::string& cueId,
                                                 const std::vector<std::string>& supplied);

        /*  THE POINTER MOVED AWAY BEFORE ANYBODY PRESSED ANYTHING: what
            `run.revoke` calls.

            ANTICIPATION IS ONLY AS GOOD AS ITS REVOCATION (§13.1), and this is
            the second half of the bargain. A horizon reserves voices and claims
            slots on the strength of where the pointer is; a pointer that has
            gone somewhere else makes every one of those a resource held for a
            scene nobody is about to run - and §3.9e's whole subject is that
            they are scarce.

            THE HANDLER FINISHES THE RUNS ITSELF rather than asking `run.ended`
            to, and that is deliberate. No existing path ends an armed,
            never-launched run: `observeEdges` ends one on `sawPlaying &&
            ! playing`, and a run that never played never saw either. PR 4.1
            closed that leak for `run.kill`; this is the same finish, reached
            from the other side. And it carries a REASON - `warning = revoked` -
            which `run.ended` has nowhere to put, since `error` is documented as
            failed-only and a revocation is not a failure.

            Depth first, children before parents, so nothing watching ever sees
            a finished group with live members underneath it. */
        void revokePrepared (Engine& engine, std::int64_t tick, const std::string& runId);

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

        /*  Whether this cue is a group whose members the OPERATOR advances -
            a sequence, set to manual, which is what both attributes default to.

            Asked by `cue.fire` and, from PR 3.7, by a trigger: firing one by
            name would leave it waiting for a GO that is never coming. */
        bool isManualGroup (const juce::ValueTree& cue) const;

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

        /*  A media cue's ranges, in playlist order, empty when it has none.

            READ AT THE ARM, and copied into the request. §3.24 says an edit
            takes effect at the next iteration, which is a re-read at the
            boundary rather than a live reference - and a live reference into
            the document would be the tick thread handing the message thread a
            tree it may edit meanwhile. */
        std::vector<RangeSpec> rangesOf (const juce::ValueTree& cue) const;

        /*  Counts the pass a ranged run is on and places the boundary out of
            it, once, when it comes into the placement horizon.

            Tick thread, below the null-player gate, because everything it does
            is arithmetic on the sample counter. A replay reaches the same
            answers by re-injecting the `run.range` records this submits - and
            reaches no answer at all about which PASS, which is right: a pass is
            a readout and §3.15 says readouts do not replay. */
        void advanceRanges (Engine& engine);

        /*  Recomputes every live run's effective level from its own and its
            ancestors', and hands the media ones to the audio side.

            ONE PLACE, AFTER THE FADES, and that is the point of it existing at
            all rather than each fade writing the voice itself. A fade knows what
            IT changed; only something that walks the tree knows what that means
            for a member three levels down whose own fade is not running. Written
            as a pass, a group trim reaches every descendant on the tick it
            moves, including the ones nothing else is touching. */
        void applyLevels();

        /*  What arming the standby means when the pointer is on a GROUP.

            §3.6's chain has to be gapless, and the disk is 0.4 s for a local
            file (§11.8): a group whose first member is armed only when GO
            arrives pays that after the operator's hand has come down. So the
            pointer arms what the group WOULD LAUNCH FIRST - the first member of
            a sequence, or every member of a timeline that starts at offset
            zero - and recursively, because that member may itself be a group.

            A vector rather than one identifier, because a timeline group starts
            several things at once and arming one of them would be a scene that
            is gapless in one channel. */
        std::vector<std::string> armablesFor (const juce::ValueTree& cue) const;

        /*  The outermost group of the block the pointer is in, or empty when it
            is not in one. What a horizon prepares, and therefore what a moving
            pointer leaves behind. */
        std::string horizonRootFor (const juce::ValueTree& list, const std::string& cueId) const;

        /*  The groups between a cue and its list, outermost first, with the cue
            itself last when it is a group. What a GO would create, and
            therefore what a horizon prepares.

            Shared by `fireStandby` and `prepareStandby` so the two cannot come
            to disagree about the shape of a descent - which they would, since
            adoption is the act of one recognising the other's work. */
        std::vector<juce::ValueTree> descentTo (const juce::ValueTree& list,
                                                const std::string& cueId) const;

        /*  Whether this cue has anything that can be done before GO, and what.

            PER PARAMETER AND NEVER PER CUE (§3.12): a media cue's arm and
            claims always; an osc cue only where its node is `anticipatable` AND
            its mount can be asked, because a value on a mount that cannot
            answer has nothing to put back; a midi cue never, because MIDI has
            no read-back at all. A fade, a stop and a memo have nothing to
            prepare - a fade cannot take over a level before it is time to. */
        bool isPreparable (const juce::ValueTree& cue) const;

        /*  The cues of a group's header that have a preparation, in header
            order. Empty for a group with no header, or one whose header is
            entirely un-anticipatable - which is a `partial` block and not a
            failure. */
        std::vector<std::string> preparableIn (const juce::ValueTree& group) const;

        /*  Starts a prepared group's `preparing` phase, or answers false when
            there is nothing to prepare and the job should go straight to the
            hold.

            `drawId` HANDS OUT THE IDENTIFIERS, supplied by a replay or drawn
            fresh, because this is reached from a command HANDLER and a handler
            never submits (§12.1). It creates the children itself and the
            `run.prepare` record carries every one of them, exactly as `go`
            carries the runs a press makes. Submitting `run.spawn` from here
            instead put those records in the log twice on a replay: once from
            the log and once from the handler re-running. */
        bool beginPreparation (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                               const std::function<std::string()>& drawId,
                               std::vector<std::string>& used);

        /*  Whether everything a `preparing` phase issued has arrived: a media
            arm armed, a network cue finished. */
        bool preparationSettled (const GroupJob& job) const;

        /** Which word from `preparedness` a settled preparation ended on. */
        const char* settledWord (const GroupJob& job, const juce::ValueTree& group) const;

        /*  A prepared group run becoming a live one: the run turns `playing`,
            it is told where the pointer entered, and its job leaves the hold.

            ONE FUNCTION FOR THREE DOORS, because GO reaches a prepared group
            three ways - through `fireStandby`'s descent when the pointer is
            inside it, through `armInternal` when the pointer is on it, and
            through `fireKind` when its own parent's job launches it - and three
            copies of this would be three things to keep in step in the one
            place where being out of step means a scene created twice.

            `enters` IS THE WHOLE OF THE THIRD DOOR. Only the OUTERMOST group a
            press touches starts; the rest are told where the pointer entered
            and left standing, exactly as `fireStandby` has always left the
            groups it created. An inner group that started here would spawn its
            member on the next tick while its parent was still running the
            header that is supposed to come first - the scene beginning from the
            inside out. */
        void adoptPrepared (const std::string& runId, const std::string& entersAt,
                            bool enters);

        /*  Whether `runId` is `ofRun` itself or one of its ancestors, by
            walking `parent` upwards. What "held under a run in the spawning
            run's own ancestry" means, written down. */
        bool inAncestryOf (const std::string& runId, const std::string& ofRun) const;

        /** Whether any group job has taken charge of this run. */
        bool claimedByAJob (const std::string& runId) const;

        /*  SOMEBODY ASKED FOR THIS RUN: it stops being a promise about a GO
            that has not happened and becomes a cue that is going to sound.

            It clears `prepare`, and that is not only a readout. A phase takes
            charge of the children of its own cues and LAUNCHES them, and a run
            the horizon armed ahead is a child of exactly that shape - so
            without a mark, a manual group would start the member the pointer
            was merely sitting on, with nobody having pressed anything. The mark
            is `prepare` itself, which already means "this is ready for a GO
            that has not happened", and this is the moment that stops being
            true. */
        void askedFor (const std::string& runId);

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

        /*  The slots a cue's `Feed` and `Insert` children name, claimed for its
            run (PRD §3.9b, §3.9e).

            ISSUED ABOVE THE NULL-PLAYER RETURN in `armMedia`, the way §12.1's
            hooks sit above `beforeTick`'s: a claim is derived from the document
            alone - the cue's children against the declared pool - so it needs
            no Player, and `wfg replay` and `wfg serve` without `--hosted` take
            it exactly as a hosted session does. What stays below that return is
            the half that does need one. */
        void claimSlotsFor (const juce::ValueTree& cue, const std::string& runId);

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
        void fireMidi (const juce::ValueTree& cue, const std::string& runId);

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
        void armStandby (Engine& engine);
        void advanceGroups (Engine& engine);

        /*  Starts a group's header, members or footer, and answers whether
            there was anything to start. False lets the caller fall through to
            the next phase, so a group with no header does not spend a tick in
            one. */
        bool beginPhase (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                         const char* phase);

        /*  The end of a round: starts the next one, or moves the group on.

            A round ending is not the group ending, which is what `loops` buys -
            and the count is of ROUNDS rather than of playbacks (§3.6), so three
            loops of "two of five" is six cues. Only the members loop; a header
            and a footer are preparation and release, and running either twice
            would undo something that was only done once. */
        void endOfRound (Engine& engine, GroupJob& job, const juce::ValueTree& group);

        /** Moves a group on to its next phase, or ends it. */
        void finishPhase (Engine& engine, GroupJob& job, const juce::ValueTree& group);

        /** The cues a group runs, in order: its enabled children. */
        std::vector<std::string> membersOf (const juce::ValueTree& group) const;

        /*  Draws this group run's next round and REPORTS IT, returning what it
            drew so the caller can schedule against it at once.

            The report is the point. A shuffled round is a decision taken with a
            random number generator, and a decision nobody wrote down is a
            session that cannot reproduce - so `run.round` carries the seed and
            every identifier in the order they were drawn, and a replay reads
            the round back rather than drawing one. The generator is consulted
            on the night and never again.

            Empty when there is nothing left to play: every member disabled or
            pruned away, or a group with none. §3.6 says an emptied round
            completes the group rather than spinning. */
        std::vector<std::string> drawRound (Engine& engine, const juce::ValueTree& group,
                                            const std::string& runId);

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

        /*  The cue the standby was last seen on, so that arming it is asked for
            ONCE rather than on every tick it sits there.

            Engine state and not a model input: a replay never sets it, because
            a replay runs no hooks - and it does not need to, because the arm it
            would have asked for is a record in the log. */
        std::string armedStandby;

        /*  The tick being processed, so a stop fired inside a command
            handler can be scheduled against the same clock the tick hook
            reads. Set by beforeTick, which runs before the handlers do. */
        std::int64_t currentTick = 0;
        std::vector<OscJob> sending;

        tree::MountTable* mounts = nullptr;
        tree::MountSender* sender_ = nullptr;
        midi::MidiSink* midiOut = nullptr;

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
