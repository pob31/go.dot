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

#include <wfg/engine/audio/EqSettings.h>
#include <wfg/engine/audio/EditRenderTable.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/FadeJob.h>
#include <wfg/engine/cue/GroupJob.h>
#include <wfg/engine/cue/ListState.h>
#include <wfg/engine/cue/CurveJob.h>
#include <wfg/engine/cue/CurveTable.h>
#include <wfg/engine/tree/MountListener.h>
#include <wfg/engine/cue/OscJob.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/Solver.h>
#include <wfg/engine/cue/LaneRecording.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/TakeTable.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/plugin/PluginTable.h>
#include <wfg/engine/plugin/Catalogue.h>
#include <wfg/engine/process/ProcessHost.h>
#include <wfg/engine/serial/SerialTable.h>
#include <wfg/engine/video/VideoSink.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
    class TouchTable;
    class MountProbe;
    class MountSender;
    class MountTable;
    class TreeSnapshot;
}

namespace wfg::cue
{
    class DcaTable;
    class LiveEdits;

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

        /*  WHETHER IT BOUNCES (namespace draft §41): out and back between its
            points, a pass each way, rather than from its in-point again. */
        bool pingPong = false;

        /*  Which range it is, by its identifier: a slice moved while it sounds
            is followed by this rather than by its place in the list, which an
            edit elsewhere in the list can change under it (namespace draft
            §33). Empty where a range is made up rather than read. */
        std::string id;
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

        /*  THE SLICE THE RUN ENTERS FIRST, and how far into its loop that
            slice's clip starts, in seconds (2026-10-02, K8's review, namespace
            draft §23.17): a bed Esc paused inside a looping slice carries on at
            the same point of its loop. Nought for every other arm - a slice
            launches at its in-point, as it always has. Only the first slice
            takes it; every slice after is entered at its in-point. */
        int startSlot = 0;
        double sliceOffset = 0.0;

        /*  The cue's EQ, read through the schema at the arm (Phase 9a) and
            applied while the voice is silent, as the routing is. A value for
            the reason everything else here is one. */
        audio::EqSettings eq;

        /*  And its inserts, one per entry of the set in chain order (PR
            9a.8): switched in or not, and the values the cue sets. */
        std::vector<FxSetting> fx;

        /*  A LIVE INPUT (Phase 9b, namespace draft §18.5): no file - the
            source is `inputWidth` logical inputs from `firstInput`, through the
            rack channel whose track this is, and `fx` is that channel's chain.
            The gate stays shut until the launch. */
        bool live = false;
        int firstInput = -1;
        int inputWidth = 1;

        /*  THE CUE'S MODE (namespace draft §22.2): timestretch when true, the
            speed then changing how long the sound takes and not its pitch;
            varispeed when false. Read at the arm and applied there, because a
            mode is on Tracktion's rebuild list (DV). The speed itself is not in
            the request: it is placed on the voice at the launch, and moves
            after it (Player::placeRate). */
        bool stretch = false;
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

        /*  AN OUTPUT'S GAIN, in dB, on the logical channels it occupies
            (namespace draft §38): its trim and its DCA's, after everything
            that reaches it. Tick thread, every tick, a relaxed store a channel;
            the audio side ramps to it. Nothing by default - a show replayed
            with no audio side has no outputs to trim. */
        virtual void setOutputGainDb (int firstChannel, int width, double gainDb)
        {
            (void) firstChannel;
            (void) width;
            (void) gainDb;
        }

        /*  A VOICE'S SPEED, placed ahead (namespace draft §22.4): from the last
            breakpoint the voice holds, its speed moves in a straight line to
            `rate` at `sample` - one is the file's own - and two at one sample
            are a step. The Runner places each change a launch horizon ahead,
            as it places a launch, so what a block plays is decided before it
            plays. Tick thread, never blocking.

            The default does nothing, which is a voice at one: a replay, and
            every rig that plays no speed. */
        virtual bool placeRate (int, std::int64_t, double) { return true; }

        /*  THAT GO.DOT ENDS THIS TRACK'S CLIPS ITSELF (namespace draft §41): a
            clip that has played backwards no longer ends where its source has
            played its length, and the Runner places its stop. Cleared at the
            next arm. Nothing by default. */
        virtual void endsOutside (int, bool) {}

        /*  A SLICE'S LOOP POINTS MOVED UNDER IT (namespace draft §33): from the
            slot's reader position `from` on - seconds of the file, counted on
            through every pass - the file is at `fileAt`, plays to `loopOut` and
            loops `loopIn`..`loopOut`; a `crossfade` is a jump, faded. The Runner
            places it a launch horizon ahead, as it places a boundary. Returns
            the move's number, or nought for a player that does not move loops -
            a replay's, a test's - and then the Runner's clock stays where the
            voice is. Tick thread, never blocking for long. */
        struct LoopMove
        {
            double from = 0.0;
            double fileAt = 0.0;
            double loopIn = 0.0;
            double loopOut = 0.0;
            double crossfade = 0.0;
            int direction = 1;          ///< -1: the file backwards from `from` on (§41)
            bool pingPong = false;      ///< the loop bounces between its points (§41)
        };

        virtual std::uint64_t placeLoop (int, int, const LoopMove&) { return 0; }

        /*  And a move the slot's reader met only after it had read past its
            `from`, applied from where the reader was instead: which move, from
            where, and the file's second there. Nothing by default. */
        struct LoopTaken
        {
            std::uint64_t move = 0;
            double from = 0.0;
            double fileAt = 0.0;
        };

        virtual std::optional<LoopTaken> loopTaken (int, int) { return std::nullopt; }

        /*  The fastest a time-stretched cue can play at this graph's rate:
            the stretcher's buffer is its latency long, and it takes 256 x
            speed frames a chunk. The Runner holds a stretched cue's speed to
            it, so its clock and the voice's agree. */
        virtual double stretchSpeedLimit() const { return 20.0; }

        /*  Where a SOUNDING cue's channels go, changed under it. Tick thread,
            on an edit and never per tick.

            SEPARATE FROM AN ARM, and the separation is the whole point. An arm
            happens while the voice is silent, so it snaps every smoother to its
            target; doing that here would take a fade that is halfway down and
            put it back wherever the document says, in the middle of the sound.
            This writes the coefficients and touches neither the level nor the
            smoothers, so the 50 ms slew carries the change - which is what
            makes a send fader something you can move against the sound rather
            than something you set and then fire the cue to hear.

            Every cell is written, the silent ones included: whatever the last
            routing reached has to be taken back, and clearing first and setting
            after would leave the whole matrix at nought for any block that fell
            between the two passes. */
        virtual void setRouting (int track, const std::vector<Coefficient>&) = 0;

        /*  A sounding cue's EQ, changed under it (Phase 9a). Tick thread, on an
            edit and never per tick; on the audio side it is atomics. A no-op
            by default rather than pure, so a Player that plays no EQ - a
            replay's, a test's - is still a complete configuration. */
        virtual void setEq (int, const audio::EqSettings&) {}

        /*  A sounding cue's inserts, changed under it (Phase 9a, PR 9a.8):
            one entry switched in or out, one value moved - or `-1`, which
            is "back to the preset". Tick thread, on an edit and never per
            tick; atomics on the audio side. No-ops by default, as setEq. */
        virtual void setFxEnabled (int, int, bool) {}
        virtual void setFxParameter (int, int, int, float) {}

        /*  How wide the cue is at one insert, changed under it (2026-09-26):
            the channels it sends and how many come back (FxSetting). Tick
            thread; atomics on the audio side. A no-op by default. */
        virtual void setFxShape (int, int, int, int) {}

        /*  A cue's whole state for one entry changed after its arm and before
            its launch (the author's decision of 2026-09-25): load that one
            instead, and hold the launch until it is in. The tick thread; a
            player that hosts no plugin has nothing to do. */
        virtual void requestFxState (int, int, const std::string&) {}

        /*  Whether that track's cue is sounding, out of any of its slots.
            Tick thread. Track-wide for the reason `stop` is: the question is
            about the cue. */
        virtual bool isPlaying (int track) const = 0;

        /*  The loudest sample the track's output stage has sent since the
            last take, linear, and counting starts again (2026-09-25, a strip's
            post-fader meter). Tick thread, once a tick for each sounding
            track; one atomic exchange on the audio side. Silence by default,
            so a replay's player and a test's are still complete. */
        virtual float takeOutputPeak (int) { return 0.0f; }

        /*  THE INPUTS' SIDE (Phase 9b, namespace draft §18.2): how many logical
            inputs the interface hands the graph, and the loudest sample on one
            since the last take, linear, the count starting again - the
            soundcheck's meter, taken once a tick. None, and silence, by
            default: the answer for a player with no interface. */
        /*  THE LIVE RACK (Phase 9b, namespace draft §18.5). The track a rack
            channel was built as, or -1 for one the graph does not have; a mic
            cue's launch, its channel's gate opened at a sample over its
            fade-in; a stop's fade taken by the input, the tail left to ring;
            and a kill, which is silence at once with nothing left ringing - on
            a rack channel its input shut, and on a voice (2026-10-01, §23.6)
            its stop with its output silenced and its EQ emptied, and its
            inserts too unless it was silent already, where it used to be the
            stop alone. Defaults a test's player is complete with: the default
            kill is the stop. */
        virtual int rackTrackOf (const std::string&) const { return -1; }
        virtual bool openLive (int track, std::int64_t sample, double) { return launchAtSample (track, 0, sample); }
        virtual void shutLive (int, double) {}
        virtual bool kill (int track) { return stop (track); }

        /*  A DOUBLE ESC'S SWEEP OF GO.DOT'S OWN PROCESSING (PRD §4.4, namespace
            draft §23.6), asked once a press, on the tick thread: every voice
            silenced, every EQ in the graph emptied and every rack channel's
            inserts reset - those of runs that have already ended among them;
            one still sounding by its own kill, a tick later; a voice's inserts
            by its silence and its next arm - but nothing on the tracks in
            `ready`, which the press leaves armed for the next GO. Nothing by
            default, so a replay's player and a test's are complete. */
        virtual void resetEffects (const std::vector<int>&) {}

        virtual int inputCount() const { return 0; }
        virtual float takeInputPeak (int) { return 0.0f; }

        /*  A SAMPLING CHANNEL'S TAKE (Phase 9c, namespace draft §19.6): a press
            placed at one of Go.dot's samples, its loop points in seconds; whether
            the channel sounds its input as well as its loop; what the audio
            thread did by itself since the last ask, for `take.closed`; and where
            each loop is playing, for the picture. Tick thread. Defaults a player
            with no recorder is complete with - a replay's, a test's. */
        struct TakeReport
        {
            std::string channel;
            std::string how;            ///< pressed, full, held
            double seconds = 0.0;
        };

        virtual bool postTake (const std::string&, TakeVerb, std::int64_t, double, double) { return false; }
        virtual void setTakeThrough (const std::string&, bool) {}
        virtual std::vector<TakeReport> takeReports (const std::vector<std::string>&) { return {}; }
        virtual double takePlayhead (const std::string&) const { return 0.0; }

        /*  KEEP (Phase 9c, §19.8): a channel's closed take made a file under
            the show's media, off the tick thread - `stem` the channel's name,
            which the file is called after - and what finished since the last
            ask, for `take.kept`: the file, or why there is none. */
        struct KeptReport
        {
            std::string channel;
            std::string file;
            std::string error;
        };

        virtual bool keepTake (const std::string&, const std::string&, const std::string&) { return false; }
        virtual std::vector<KeptReport> keptTakes() { return {}; }

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

        /** Lets go of the run table's release listener, which calls into this. */
        ~Runner();

        /** Null is legal and means a show with no audio side. */
        void setPlayer (Player* player) noexcept { audio = player; }

        /*  THE PICTURE SIDE (Phase 8a, namespace draft 35): null is legal and
            means a show that draws nothing - a replay, a rig - as a null
            Player is one that plays nothing. */
        void setVideo (video::Sink* sink) noexcept
        {
            videoSink = sink;

            //  A new picture side has been told nothing to read ahead yet.
            preloadsSent.clear();
            revisionPrepared = 0;
            banksPrepared = 0;
        }

        /*  THE CLOCK A VIDEO POINT IS PLACED ON, now: the audio's samples when
            there is a player, the tick's otherwise, -1 with no tick schedule -
            what the renderer is told, so it reads the points on their own
            clock. And that clock's rate. Tick thread. */
        std::int64_t videoClockNow() const noexcept;
        int videoSampleRate() const noexcept;

        /*  THE TAKES' ACCOUNT (Phase 9c), which the take verbs move too: a
            transport cue's press, a mic cue's GO and its channel let go move it
            here, and the hook places the presses. None, and a sampling
            channel's takes are left alone. */
        void setTakes (TakeTable* table) noexcept { takes = table; }

        /*  THE FADERS FLIPPED TO A CUE (namespace draft §34, after §20.9): the
            table `lane.*` moves, which the hook `recordLane` reads each tick -
            every lane's ride, and during a pass each armed hand sampled
            against the file's clock. Absent - a replay, a tree dump - nothing
            is recorded, and the lanes a pass ended in arrive from the log
            instead. */
        void setLanes (LaneTable* table) noexcept { lanes = table; }

        /*  THE OSC CURVES ARMED FOR RECORDING (namespace draft 45, O.9), which
            the pass's hook samples into and `advanceCurves` reads to leave a
            curve the device is reporting unsent. */
        void setCurves (CurveTable* table) noexcept { curveTable = table; }

        /*  A PASS ON AN OSC CUE'S CURVES: the cue fired, its clock moved to
            `from`, the run made or found - `curve.record`'s, as
            `startLanePass` is `lane.record`'s. Empty when nothing could fire. */
        std::string startCurvePass (Engine& engine, std::int64_t tick, const std::string& cueId,
                                    double from, const std::string& runId);

        /*  WHAT TO LISTEN TO (O.10, YB): the addresses of the armed curves, by
            device, for each device that can be asked over OSCQuery and whose
            `rx` is on - nothing while no curve is armed, or under the lock. */
        tree::MountListener::Wanted listenWanted() const;
        void resetAudioPreparation() { armedStandby.clear(); }

        /*  WHAT A CLOCK MOVE GAVE BACK, handed to the hooks to put back
            (2026-10-02, K5, namespace draft §23.16; the author's ruling 6d).
            `audio.clockMoved` revokes the prepared runs in its handler - it
            must, before the show is rebuilt on the new graph - and a handler
            cannot put back what they pre-sent: the `node.set`s would reach a
            replay twice, once from the log and once from itself. So it hands
            the runs it revoked here, and `armStandby` writes their values back
            once the engine would take a write again - the outage over - and
            then makes the standby ready afresh, after them. Hook state: a
            replay takes the `node.set`s from the log and never reads this.
            The settings operations (`audio.apply`, `audio.setup`,
            `plugin.load`) hand theirs over the same way since K5's extension. */
        void putBackWhenWritable (const std::vector<std::string>& revokedRuns);

        /*  The runs, for the one caller outside the Runner that has to ask
            about them: `go`, whose cursor has to know whether a manual group
            still has rounds to play before it lets the pointer out of it.
            Const, because the table's one writer is the command handler and
            that is the whole point of the arrangement. */
        const RunTable& runTable() const noexcept { return runs; }

        /*  Each logical input's loudest sample over the last tick, in decibels,
            -120 for silence - taken from the player once a tick, and empty with
            no player (Phase 9b). What the tree publishes as each named input's
            meter. */
        const std::vector<double>& inputMetersDb() const noexcept { return inputMeters; }
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

        /*  Where a cue's plugin states live: the bundle's `plugins/` folder,
            which a cue's `fx/stateFile` names a file under - as `media/` is to
            `media/file`, and for the same reason. */
        void setPluginsFolder (std::string folder) { pluginsFolder = std::move (folder); }

        /*  Where a MIDI cue's bytes go. Null is legal and is what a replay has:
            the run is created and finishes on the ticks the log says, and
            nothing reaches a port - exactly as a mount table's absence leaves a
            network cue's record reproducing with nothing on the wire. */
        void setMidiSink (midi::MidiSink* sink) noexcept { midiOut = sink; }

        /*  WHETHER THE AUDIO IS OUT (PRD §6.2), asked by `go.doh`'s handler so
            a press accepted in an outage can say at once what waits for the
            clock (Doh! D4's review, OJ). Handler state: the answer is the audio
            status the `audio.connection` records write, which a replay
            re-applies - `registerAudioSettingsCommands` hands it in, for `serve`
            and `replay` alike. Unset is never out. */
        void setOutage (std::function<bool()> isOut) { outage = std::move (isOut); }

        /*  THE PROCESS CUES' PATCHES (namespace draft §51): what runs each running
            process cue's Pure Data patch, one to a thread. Null - a replay, a rig,
            a tree dump - and a process run plays and ends as the log says, with no
            patch run: what a patch made Go.dot do is in the log as its own records. */
        void setProcesses (process::ProcessHost* host) noexcept { processHost = host; }

        /*  THE TREE AS LAST PUBLISHED, for the rows of the show a patch hears
            ([r /godot/...]): handed in by serve after each publish, as the
            surfaces are. Tick thread. */
        void noteSnapshot (std::shared_ptr<const tree::TreeSnapshot> snapshot) { lastSnapshot = std::move (snapshot); }

        /*  WHAT ARRIVED ON THE MIDI PORTS (PC.3), for the patches whose cue
            listens on a port: filled by the input thread, taken once a tick. */
        void setProcessMidi (process::MidiInbox* inbox) noexcept { processMidi = inbox; }

        /*  THE SHOW'S SERIAL PORTS, OPEN (namespace draft §51, ACR; PC.10): where
            a patch's line to `/godot/serial/<id>/out` goes. Serve only; null -
            a replay, a rig - and the line goes nowhere, as a device write does. */
        void setSerialPorts (serial::SerialTable* ports) noexcept { serialPorts = ports; }

        /*  A LINE A SERIAL PORT READ, as `serial.heard` applies it: what the
            patches hear at the next tick and the tree's last line - in a replay
            too, which opens no port. */
        void noteSerialLine (const std::string& port, const std::string& line, std::int64_t tick)
        {
            serialHeard.note (port, line, tick);
        }

        const serial::HeardLines& heardLines() const noexcept { return serialHeard; }

        /*  THE SPACEMOUSE'S SIX AXES THIS TICK (PC.3), or none when the puck is
            not live - handed in by serve before `beforeTick`, for a patch that
            hears `/godot/puck`. And whether one does: serve opens the puck for
            it as for an armed curve. */
        void notePuck (std::optional<std::array<double, 6>> axes) noexcept { puckAxes = axes; }
        bool processesWantPuck() const noexcept { return puckWanted; }

        /*  `process.send`'s handler (PC.3): atoms for a name a running patch
            hears, handed over at the next tick. False when the run is not a
            running process cue. */
        bool queuePatchInput (const std::string& runId, process::Input input);

        /*  WHAT EACH DCA IS TRIMMING BY (PRD §3.28), added to the level of
            every run whose cue - or whose group's cue - is marked with it, and
            moved by a fade that names one. Null where nothing was handed in, in
            which case every DCA trims nothing: the Runner's arithmetic is
            unchanged for a show that declares none. */
        void setDcas (DcaTable* table) noexcept { dcas = table; }

        /*  THE EQ AND SENDS A LOCKED SHOW IS RIDING (LiveEdits.h, 2026-09-25):
            asked before the show wherever a cue's EQ or routing is read - at
            the arm, and for a sounding cue whenever the layer moves. Null
            where nothing was handed in, which is every tool but serve and
            replay. */
        void setLiveEdits (const LiveEdits* layer) noexcept { liveLayer = layer; }

        /*  WHICH SET ENTRY IS WHICH SLOT OF THE GRAPH (2026-09-26): a cue's
            inserts are sent by slot, and the slots are the graph's, fixed when
            it was built - never counted off the set as it stands now. Null, or
            a table with no graph, and the set's own order is the slots, which
            is what a replay and a test rig have. */
        void setPlugins (const plugin::PluginTable* table) noexcept { pluginTable = table; }

        /** The catalogues a fade on a plugin value reads a default from (§26). */
        void setCatalogues (const plugin::CatalogueStore* store) noexcept { catalogues = store; }

        /*  WHO IS HOLDING WHICH NODE, for the fader edges (PRD §3.9a): a
            fader-start counts only from a fader released at the bottom, and a
            fader-stop only when the hand lets go there, and the touch table is
            where "released" is known. Read in the tick hook, which is the
            thread that owns it. Null in a replay, which runs no hooks and so
            reads no edges: the presses and releases they caused are records. */
        void setTouches (const tree::TouchTable* table) noexcept { touches = table; }

        //======================================================================
        /*  SAMPLER STRIPS (PRD §3.27, Phase 6). A press and a release on a
            strip, applied - the handlers of `strip.press` and `strip.release`,
            which a surface, the virtual panel, the page and the fader edges
            below all send. Answer a refusal word, or empty when applied
            (which includes a press on a strip with nothing on it: a pad hit
            between banks is not a mistake).

            `velocity` is 1 to 127, or -1 for a press that carries none - a
            fader lifted, a button, a cue fired by name. */
        std::string pressStrip (Engine& engine, std::int64_t tick, const std::string& stripId,
                                int velocity, const std::string& origin);
        std::string releaseStrip (Engine& engine, std::int64_t tick, const std::string& stripId,
                                  const std::string& origin);

        /*  A sampler member waiting for a voice takes one: `run.arm`'s
            handler. The hook sends it when a track has come free; anyone may,
            as with every engine-origin command. */
        void armAgain (Engine& engine, const std::string& runId);

        /*  A SAMPLER MEMBER FIRED BY NAME - `cue.fire`, a trigger, a start
            cue from the live recorder - is a press on the strip it holds.
            Answers the refusal word when there is none (`needs-strip`), or
            empty when the press was made; and whether the cue was a sampler
            member at all, so every other cue fires as it always did. */
        bool isSamplerMember (const std::string& cueId) const;
        std::string pressMember (Engine& engine, std::int64_t tick, const std::string& cueId,
                                 const std::string& origin);

        /*  The sampler strips of every surface, in the order a group fills
            them: surfaces in document order, then each strip's index. Read
            once per show revision. */
        const std::vector<std::string>& samplerStrips();

        /*  THE HAND'S EDGES ON FADER STRIPS, in one place each.

            A START IS A TOUCH (author, 2026-09-23): the fader has flown to the
            member's `initialLevel` and waits there, and a hand landing on it
            starts the clip at wherever the fader is. `touchDwellTicks` is how
            many ticks a touch must last before it counts - nought, so it counts
            on the tick it arrives. It is a number because the D700's faders
            report touches nobody meant: 58 of 81 in one capture landed within
            150 ms of a nearby button press, a hand reaching past (PRD §3.16).
            If reaching for the master section fires samples, this is what the
            bench raises, at the price of that much latency on every start.

            A STOP is §3.9a's fader-stop: a hold clip's trim at or below
            `parkedDb` with nobody touching it - released at the bottom. */
        struct FaderEdge
        {
            static constexpr double parkedDb = -118.0;
            static constexpr int touchDwellTicks = 0;
        };

        /*  The level a velocity or a pressure byte asks for, on one scale:
            127 is 0 dB, 1 is `floor`, straight between them in dB; nought is
            the floor too (a pressure of nought is ignored before it gets
            here). One function for both, because a pad is a fader without a
            motor and both bytes are where its fader would be. */
        static double levelForByte (int byte, double floor) noexcept;

        /*  A FADER SOMEBODY PULLED TO THE BOTTOM, lifted for a sound somebody
            asked to hear: the run's trim, at or below `parkedDb`, goes to the
            cue's `initialLevel`, or to unity when that is the bottom too. What
            a press that is not the touch does (`pressStrip`), and what a level
            lane's pass does at its start (`startLanePass`, namespace draft
            §30.4) - a pass at -120 dB records a ride nobody can hear. A trim
            above the bottom is left where the hand put it. */
        void liftParkedFader (Run& run, const juce::ValueTree& cue) const;

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

        /*  THE JUMP: what `list.loadToTime` calls.

            PRD §3.13. An operator asks for the show as it was, and this makes
            it so - one record whose applied arguments carry every run
            identifier it drew, in the order it drew them, because a replay
            never draws one of its own.

            IT ENDS WHAT THE JUMP ABANDONS BEFORE IT BUILDS ANYTHING, the way
            `run.kill` ends a run: every descendant, AND NO FOOTER. A footer is
            arbitrary and need not be an inverse - which is exactly why §3.13
            recomputes forward rather than unwinding - so running one here would
            be arbitrary work fighting the values the jump is about to send, and
            a footer that blocks on a fade would make the jump wait for it. The
            claims come back in the same drain, so the plan's land rather than
            queue behind runs this has just ended.

            TWO THINGS IT DOES NOT TOUCH: the runs of OTHER lists, because a
            jump is scoped to its own list and two lists can be live at once - a
            slot held by another list is precisely the "something else" a
            pending claim means - and, when it exists, the persistent section,
            whose cues are re-asserted rather than restarted.

            Returns every identifier it drew. */
        std::vector<std::string> loadToTime (Engine& engine, doc::ShowDocument& editable,
                                             std::int64_t tick, const std::string& listId,
                                             const std::vector<std::string>& supplied);

        /*  A SEEK: what `run.seek` calls (author, 2026-09-18: "I'd like to be
            able to scrub active cues and groups").

            A MEDIA RUN moves to that second of its file: the voice is stopped
            and asked for again at the new offset, on the same track, at the
            level it was playing at, and the run stays the run it was - same
            identifier, same row, same fade aimed at it. Answers false for a
            run that is not media or is over.

            A GROUP RUN is re-seated at that second of its own timeline: what
            it held is ended the way a jump ends what it abandons - no footer -
            and its members are built again under the SAME group run, each
            over, sounding at its offset or waiting for its due tick, from the
            solver's own answer for the scene at that second. That is how a
            member already finished comes back when the hand scrubs to before
            it. Nothing beside the group is touched: a bed the operator started
            earlier keeps sounding as it was. Returns every identifier it drew,
            in order, for the record. A group the walk cannot time - a manual
            sequence, a loop, a shuffle - returns nothing and changes nothing.

            (K9, 2026-10-02, namespace draft §23.18:) A SCENE THE WALK CANNOT
            TIME IS SOUGHT IN THE ROUND IT IS IN, once one has begun: a timeline
            or an automatic sequence that loops, shuffles, plays some of its
            members or has a header. `seconds` are the scene's own, as its
            `position` reads; the round's second is that less where the round
            began, clamped to the round, and a second inside a header changes
            nothing. A manual group and a sampler bank still change nothing.

            (2026-10-05, namespace draft §30.4:) A MEDIA RUN WITH RANGES LANDS AT
            THE SECOND ASKED, inside the range that holds it, by K8's
            `sliceFrom` - not at that range's in-point. In the range it was
            already in it keeps the pass it was on when `keepPass` says so (a
            scrub); every other landing is the range's first pass. A second in
            a gap between ranges is the next range's start, and one at or past
            the last out-point is a hair inside the last range's end - never
            the top of the file. */
        bool seekMedia (Engine& engine, std::int64_t tick, const std::string& runId,
                        double seconds, bool keepPass);

        /*  A MOVIE SOUGHT (namespace draft §47, AAC): its playhead stepped to
            `seconds` of the file a horizon ahead, landing by §30.4's rules over
            its Ranges as a sound's does - inside a range at that second, on the
            pass it was on when `keepPass` asks; in a gap at the next range's
            in point; past the last out point a hair inside it; with none,
            within the file - and the sounds locked to it sought to the same
            second. Until those sounds are playing again the picture holds the
            frame it was sought to, and runs on from the sample they start on,
            so picture and sound leave the seek together. False for a video run
            that is not a movie - a fill, a picture, a capture has no second to
            go to. */
        bool seekMovie (Engine& engine, std::int64_t tick, const std::string& runId,
                        double seconds, bool keepPass);

        /*  A LEVEL LANE'S PASS STARTS (namespace draft §20.9; 2026-10-05,
            §30.4): what `lane.record` calls. The cue is fired as `cue.fire`
            fires it, and the run it hands back plays from `from` - nought being
            the cue's own start, its start offset or its first range's in-point
            - whatever it was doing: one already sounding (a sampler member
            muted at the bottom of its fader plays on) is moved there, and one
            still on its way out from the last pass's stop is taken back from
            it, as a seek takes it. A fader pulled to the bottom is lifted as a
            press lifts it (`liftParkedFader`), so the pass is heard. Returns
            the run, empty when the cue does not play. */
        std::string startLanePass (Engine& engine, std::int64_t tick, const std::string& cueId,
                                   double from, const std::string& runId);
        std::vector<std::string> seekGroup (Engine& engine, std::int64_t tick,
                                            const std::string& runId, double seconds,
                                            const std::vector<std::string>& supplied);

        /*  WHETHER A SEEK SENT NOW WOULD MOVE THIS RUN (K9): the gate both
            `seekGroup` and `seekMedia` keep, read from handler state and the
            document alone, so the handler may ask it. A media run that is not
            over and Doh! has not taken back; a group run the machine paces - a
            timeline or an automatic sequence - that the walk times as written
            (in its own pre-wait too), or that has begun a round. Published as
            `run/seekable` through `mirrorSeekable`, so the clients offer the
            scrub where this says yes and nowhere else. */
        bool seekableNow (const Run& run) const;

        /*  Whether a seek on this scene takes the walk's road - fired to play
            once, its round the members as written - rather than its round's
            (K9's review, MX): the run's own record, and the walk's shape. */
        bool seeksAsWritten (const Run& run, const juce::ValueTree& group) const;

        /*  Where the media lengths live, for the solve behind a jump.

            Held by pointer and not owned, exactly as the parameter tree holds
            it: absent - a replay, a test, a bundle with no media folder - every
            length is unknown, which the solver reports as a confused entry
            rather than a guess. */
        /*  As the tree's, and for the same reason: a length learned after the
            show opened swaps the map, so what is held is the POINTER TO THE
            OWNER and the map is asked for where it is used. A test may hand in
            a map of its own instead, and `wfg replay` hands in the lengths its
            log recorded. */
        void setMediaDurations (const std::map<std::string, double>* durationsToRead) noexcept
        {
            fixedDurations = durationsToRead;
            durations = durationsToRead;
        }

        /*  THE RENDERS OF THE OPEN EDITS (namespace draft §55, ADM, ADP), asked
            for at the top of each tick as the lengths are and held for that
            tick. A snapshot that moved is a render that landed: the standby
            is made ready again when its edit was waiting for one, and an
            armed cue whose played file changed is armed again. Nothing
            handed in - a replay, a test - is no renders: an open edit then
            plays nothing and is as long as its sections. */
        void setEditRenders (std::function<std::shared_ptr<const audio::EditRenders>()> source)
        {
            renderSource = std::move (source);
        }

        /** The object that learns them, when there is one. */
        void setMediaInfo (const audio::MediaInfo* infoToRead) noexcept
        {
            mediaInfo = infoToRead;
        }

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
            reports done. Returns the child's run identifier.

            `fromItsRecord` is `run.spawn`'s own call (namespace draft §24, GZ):
            a spawn a group's job decided before a Doh! that brought the group
            back to life in that same tick is born done. Every other road - a GO,
            the horizon - is decided after the Doh, and spawns as ever. */
        std::string spawnChild (Engine& engine, const std::string& parentRun,
                                const std::string& cueId, const std::string& runId,
                                std::int64_t tick, bool fromItsRecord = false);

        /*  A run that was spawned and is now to begin: its pre-wait starts, or
            it fires at once when it has none. What `run.launch` calls. */
        void launchRun (Engine& engine, std::int64_t tick, const std::string& runId);

        /** Every group in flight. Diagnostics and tests; the Runner drives them. */
        const std::vector<GroupJob>& groups() const noexcept { return scheduled; }

        /*  WHERE EACH LIST IS BEING POINTED, and where the last jump landed.

            On the Runner because it is the tick thread's own state about cues -
            PRD §4.10 keeps it out of the document, and a show reopened tomorrow
            correctly has no aim. The parameter tree reads it to publish
            `list/aim`, `list/solve` and `list/statePosition`; `list.aim` writes
            it; and PR 4.8's load-to-time will write the second half. */
        ListState& listState() noexcept { return lists; }
        const ListState& listState() const noexcept { return lists; }

        /*  The list a cue belongs to, by climbing to the top, or empty.

            Asked by the handlers that append a step - a cue fired by name or by
            a trigger names the cue and not its list - and by the jump, which is
            scoped to one list. */
        std::string listOfCue (const std::string& cueId) const;

        /** How many observation questions have been asked. Diagnostics, and M21. */
        std::uint64_t observationsAsked() const noexcept { return asked; }

        /*  WHERE A PAUSED BED CARRIES ON FROM (2026-10-02, K8, namespace draft
            §23.17): a second of its file for a cue with no slices; for a cue
            with slices, the slice it was in and - since K8's review - how far
            into it, in seconds of the file since the slice's first pass began,
            the passes not wrapped, so a looping slice carries on inside its
            loop and in the same pass. What `run.assert` carries when the
            assertion puts back a bed Esc paused. */
        struct ResumePoint
        {
            double from = 0.0;
            int range = -1;
        };

        /*  Fires a persistent cue the assertion found missing, and marks the run
            as the machine's rather than anybody's. `run.assert`'s handler. With
            a resume point, a media run is armed there rather than at the top. */
        std::string assertCue (Engine& engine, std::int64_t tick, const std::string& cueId,
                               const std::string& runId,
                               std::optional<ResumePoint> resume = std::nullopt);

        /*  ESC ON THE PERSISTENT SECTION (2026-10-02, K8, the author's ruling;
            PRD §3.29, §4.4): called by `run.stopAll`'s handler before anything
            is brought down.

            - Every persistent media run it finds sounding - not finished, not
              already asked to stop, not taken back by Doh! - is PAUSED: which
              run it was, and the file it played. WHERE it had got to is its
              playhead at the press (the author, 2026-10-02, K8's review), read
              by a hook on the next tick (`notePausedPlayheads`) and carried on
              the `run.assert` record, so a replay - which has no playhead -
              arms the same second from the log. What this handler writes
              beside it is the second for a session with no playhead to read
              (no audio side): counted from the tick `run.started` was applied
              on at the cue's own speed, from where the run's arm began. A mic
              run is remembered with nothing to carry on from: its resume is a
              relaunch.
            - The persistent pass a step before the press opened is taken back,
              as the double Esc's is (§23.10): the next step is what puts the
              section back, and its pass carries the paused beds on.

            Handler state, so a replay pauses the same runs; the pass is hook
            state, which a replay never reads. Nothing is submitted. */
        void pausePersistent (std::int64_t tick);

        /*  WHETHER AN ESC WAS APPLIED EARLIER IN THIS DRAIN (K8): a pass a hook
            decided before the press can drain behind it, and `run.assert` asks
            this, as it asks `killedInDrain`, and does nothing. */
        bool escapedInDrain (std::int64_t tick) const noexcept { return escapedAtTick >= 0 && escapedAtTick == tick; }

        /*  Whether Esc left a persistent cue paused at a second to carry on
            from, and where: the playhead when one was read, the handler's count
            otherwise. Tests. */
        std::optional<ResumePoint> pausedAt (const std::string& cueId) const;

        /** Whether a persistent cue is suspended for this session. Tests and the console. */
        bool isSuspended (const std::string& cueId) const
        {
            return suspended.find (cueId) != suspended.end();
        }

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
        /*  How wide a media cue is after the inserts it switches in
            (cue/InsertChain.h): what its routing reads (2026-09-26). */
        int chainChannelsOf (const juce::ValueTree& cue) const;

        /*  `moved` is a run's values a fade moved (namespace draft §26): a
            send it holds is heard at that level, and a bus the cue has no
            send into is a send of the run alone. Null at an arm.
            `laneOffsets` is what the run's send lanes ask for, by bus
            (namespace draft §28): added to each send's level after the
            lock's ride and the fade's moved value (PY). Null for none. */
        std::vector<Coefficient> resolveRouting (const juce::ValueTree& cue,
                                                 int trackChannels,
                                                 std::string& problem,
                                                 int chainChannels = 0,
                                                 const std::map<std::string, double>* moved = nullptr,
                                                 const std::map<std::string, double>* laneOffsets = nullptr) const;

        /*  HOW FAR AHEAD A SEND'S LANE IS READ (namespace draft §28, PZ): one
            coefficient glide, `audio::CueMatrix::slewSeconds` - repeated here
            because the cue layer names no audio type, and checked equal to it
            by `GoTests`. */
        static constexpr double sendLaneLeadSeconds = 0.05;

        /** Every fade in flight. Diagnostics and tests; the Runner drives them. */
        const std::vector<FadeJob>& fades() const noexcept { return running; }

        /*  THE OSC CUES WHOSE CURVES ARE PLAYING (namespace draft 45), for a
            test and a readout. A finished one stays until the next GO tidies. */
        const std::vector<CurveJob>& curves() const noexcept { return curving; }

        /*  Whether a run's curves are playing - which is what makes it seekable
            and what keeps the voiceless playhead from overwriting its clock. */
        bool isCurving (const std::string& runId) const;

        /*  A SEEK OF A CURVE RUN: its clock reads `seconds` from this tick,
            within its duration, and the next tick writes what the curves say
            there. Handler-safe: it moves state a replay rebuilds the same way. */
        void seekCurves (const std::string& runId, double seconds, std::int64_t tick);

        /*  ESC, THE GRACEFUL WAY, AS A FADE (author, 2026-09-28: "a 'Panic'
            fade duration that fades out all playing cues. It seems the Panic
            cuts everything with no fade time").

            What `run.stopAll` does to the sound before it asks every root to
            stop: every run that is sounding is faded to silence over the show's
            `audio/panicFade` and stopped when it gets there - the job a stop
            cue's `fade` verb runs, one per voice, with no cue behind it. A
            group is asked to stop as it always was; its members are already
            fading, so its footer runs when they have gone, which is §4.4's
            "same code path as normal completion, entered early" at the speed
            the show chose. Nought is the cut Esc used to be.

            A run already on its way out keeps its own stop when that lands
            first. `tick` is the command's own, for the reason `fire`'s is. */
        void beginPanicFade (std::int64_t tick);

        /*  ONE VOICE BROUGHT DOWN THE WAY ESC BRINGS IT DOWN: from where its
            level is to silence over `ticks`, then stopped - a mic cue's input
            shut instead, its tail left to ring. Esc's per-voice job, shared with
            Doh!'s own fade (§24). Nothing is pushed when a stop due sooner
            already holds the voice; the caller asks the stop. */
        void panicShapedFade (const std::string& runId, std::int64_t tick, int ticks, double seconds);

        /*  AND A DOUBLE ESC DURING IT (§4.4, "drops all actions"): every job
            that is holding a voice for a stop still to come lets go, so the
            runs `run.killAll` marks are cut on this tick by `enforceStops`
            rather than faded to the end. Without it the second Esc would be
            waited out - and, for a stop cue's fade, the target was put back to
            `playing` by the killed fade and never stopped at all. */
        void dropStopFades();

        /*  AND THE SWEEP THAT GOES WITH IT (2026-10-01, namespace draft §23.6):
            the audio side asked, once a press, to silence every voice and
            empty every effect Go.dot's graph holds - but nothing on the voices
            this press leaves armed for the next GO, which it is told. A Player
            call and nothing else - no record, no change to a run - so a replay,
            which has no Player, is the same with it or without. */
        void resetEffects();

        /*  AND WHAT IS STILL WAITING TO LEAVE (2026-10-02, H4, namespace draft
            §23.10): the double Esc's half that reaches the wire, called by its
            handler after the run table's half has marked the roots, so it
            knows which runs the press killed and which it spared.

            - The network sender's queue is emptied - a value a rate cap holds
              back included - but for the pre-sends of what the press leaves
              ready (the standby's prepared scene), which go on their turn.
            - The MIDI sink drops every cue message not yet gone and ends each
              note a cue started (`MidiSink::dropQueued`).
            - A start cue's fire still to be submitted is dropped.
            - Every osc run the press killed in the very drain that launched it
              is marked `sendDropped` (§24, HQ, L31): its message was queued
              there and is gone, so it never left.

            - The persistent pass a GO before the press opened is taken back:
              the next GO restores the section (PRD §3.29).

            The mark is decided from handler state alone - the run's kind, its
            launch tick against `tick`, the kill marks - so a replay, which has
            no sender and no sink, marks the same runs. Nothing is submitted
            and the press's record is not changed. */
        void dropOutputs (std::int64_t tick);

        /*  A DOUBLE ESC FLIPS THE FADERS BACK (2026-10-02, K4, namespace draft
            §23.15, the author: "double Esc would throw away the fader
            association"; §34). What `lane.free` does, from the press's
            handler: every strip rides what it rode before, the cue and its REC
            choices are forgotten, and a pass under way is dropped with its
            rides (DM) - nothing is written, and no `lane.stop` follows, there
            being no pass left to end. Handler state only, so a replay frees it
            in the same record. Every other end of a pass leaves the faders
            flipped (2026-10-06, UM). */
        void freeLane() noexcept;

        /*  WHETHER A DOUBLE ESC WAS APPLIED EARLIER IN THIS DRAIN (the review
            of H4, 2026-10-02, namespace draft §23.10). The engine's own fires
            that a hook decided before the press - a start cue's target, the
            persistent pass - can drain behind it in the same tick, and would
            start something fresh after the press that drops every action; their
            handlers ask this and do nothing. Handler state: the press's handler
            writes it, so a replay asks the same. */
        bool killedInDrain (std::int64_t tick) const noexcept { return killedAtTick >= 0 && killedAtTick == tick; }

        /*  THE LEAST TIME BETWEEN TWO GOs (PRD §3.7's GO debounce, a show
            setting since 2026-09-28): whether a GO at `tick` falls inside the
            show's `list/goDebounce` of the last GO that fired something. The
            handler asks, and says `too-soon` when it does; `beginGo` notes the
            GO that fired (since 2026-10-01; `noteGo` did until then). Handler
            state, so a replay - which runs the handler - refuses the same GOs
            the night did. */
        bool goTooSoon (std::int64_t tick) const;

        //======================================================================
        /*  DOH! - TAKING BACK THE LAST GO (PRD §3.32, namespace draft §24;
            D1, 2026-10-01).

            THE GO RECORD. Every GO that fires something - past the empty
            standby and the GO debounce - is counted, and the count is its
            SERIAL: what it makes and what it adopts carry it (`Run::goSerial`),
            so the Doh finds that GO's runs in one scan. `beginGo` opens the
            record before the fire and `endGo` closes it after, both in the `go`
            handler and both from handler state alone, so a replay - which runs
            the handler and no hook - keeps the same record.

            `goDoh` is the command's handler: the refusal word, or empty when
            applied. It decides from handler state and logged records only - the
            replay rule of §24 - and submits nothing; it moves the pointer back,
            the list's `finished`, the GO debounce and the history, brings what
            the GO started down (heard, over the panic fade; not heard, at once),
            gives back what the horizon made after it, brings back to life an
            older act the GO ended, and remembers what the GO had sent to a
            device left to its operator, which the corrected GO then sends
            nothing of. */
        void beginGo (std::int64_t tick, const std::string& listId, const std::string& standby,
                      bool finishedBefore);

        /*  `carriedOn` is the run a resume launched or brought back (D2): the
            list's resume is spent on it rather than revoked. */
        void endGo (Engine& engine, std::int64_t tick, const std::vector<std::string>& made,
                    const std::string& carriedOn = {});
        std::uint64_t goInHand() const noexcept { return currentGo; }

        /*  `supplied` and `drawn` (D3): the identifiers its put-back makes - a
            cue the GO stopped, made again where it would be now; a scene, a
            bank fired again - drawn in that order, and carried on the applied
            record so a replay makes the same. */
        std::string goDoh (Engine& engine, doc::ShowDocument& editable, std::int64_t tick,
                           const std::vector<std::string>& supplied, std::vector<std::string>& drawn);

        /*  THE ONE WRITE A FIRE MAKES ON THE DOCUMENT ITSELF (namespace draft
            §27): an enable or disable cue's switch, and the standby a cue
            switched off is stood on moving off it. The runner otherwise holds
            the document read-only, and a fire is deep inside a handler with no
            writable one to hand; deferring the switch to the next tick, as a
            start cue's fire is, would let a cue fired in the same tick run on
            the old answer. Given once, by `registerGoCommands`. A fire is a
            logged record, so a replay switches the same cues. */
        void setOverrideDocument (doc::ShowDocument& editable) noexcept { overrideDocument = &editable; }

        /*  A JUMP CUE'S MOVE, as `standby.jump`'s handler makes it (§27, PM):
            standby onto `target` on its list, and with `andGo` that cue fired as
            GO fires it under `cause`, the GO that fired the jump cue. Answers a
            refusal, or empty; `made` is every identifier the fire drew, in
            order, which a replay hands back as `supplied`. */
        std::string jumpStandby (Engine& engine, doc::ShowDocument& editable, std::int64_t tick,
                                 const std::string& listId, const std::string& target,
                                 bool andGo, std::uint64_t cause,
                                 const std::vector<std::string>& supplied, std::vector<std::string>& made);

        /*  A SCENE THE GO STOPPED, PUT BACK ONCE IT HAS ENDED (2026-10-03, D3,
            namespace draft §24.13): `go.dohRelaunch`'s handler. The hook submits
            it the tick it sees the scene's run finished; the handler relaunches
            the scene at the second it would have reached now and draws the
            identifiers the record carries - and does nothing when no pending
            put-back names the run any more (a GO on its list came first). */
        void dohRelaunch (Engine& engine, std::int64_t tick, const std::string& groupRun,
                          const std::vector<std::string>& supplied, std::vector<std::string>& drawn);

        //======================================================================
        /*  CARRYING ON WHAT A DOH PAUSED (2026-10-02, D2, PRD §3.32, namespace
            draft §24): what a GO started that had been HEARD - a media or mic
            cue, a scene - is paused by the Doh, and the next GO on that cue
            carries it on from where it was at the press, arriving over a 0.1 s
            de-click (the author, 2026-09-30). What the list's mark holds of it
            is its ROOT: the run paused, where its file had got to - the
            playhead the press's own tick read, reported by a hook on the next
            tick (`go.dohPlayhead`), the handler's count until then and with no
            audio side - its level, and for a scene the plan it is seated
            from. All handler state, so a replay carries the same on.

            `markFor` is the mark on a list whose root the standby names, or
            nothing; `resumeStandby` is the `go` handler's road when there is
            one: in place inside the Doh fade, warm through the arm the standby
            made at the point, cold or as a re-seated scene otherwise. */
        struct SeatResume
        {
            /*  How what a seat carries on arrives: each sounding media cue at
                the level it had at the press over the de-click, carrying on
                into the rest of a fade of its own scene still moving then
                (HG), inside the slice it was in at the point its playhead had
                reached (K8's `sliceFrom`); each sounding mic and group seated
                at the level it had. */
            std::map<std::string, double> levels;
            std::map<std::string, FadeSegment> carryOn;
            std::map<std::string, double> sliceFrom;

            /*  A carried plain fade on a sounding mic, pushed as a job of its
                own once the mic is seated: the mic's arrival is its gate, not a
                job, so there is nothing for it to ride. */
            std::map<std::string, FadeSegment> micFades;

            /*  How many ticks each sounding cue takes to arrive, nought for the
                de-click (2026-10-03, D3): a cue the GO had STOPPED, put back by
                a Doh, fades back in over the panic fade (the author, 2026-09-30,
                (d)) - a sound and a mic's gate alike. */
            int arrivalTicks = 0;
        };

        struct DohRoot
        {
            enum class Kind { media, mic, tree };

            Kind kind = Kind::media;
            std::string cue, run;

            /*  The older live manual group it sat in - the act a member was
                fired into - or empty at the top of the list. */
            std::string underRun;

            /*  A media root: where in its file it carries on from, or the slice
                it was in and how far into it (K8's `sliceFrom`). */
            double offset = 0.0;
            int range = -1;
            double sliceFrom = 0.0;

            double levelDb = 0.0;
            std::int64_t landsAt = 0;

            /*  A scene: one row per cue of it, how each sounding cue arrives,
                and the run of each media cue sounding, whose playhead the hook
                reports. */
            std::vector<PlannedRun> plan;
            SeatResume arrival;
            std::map<std::string, std::string> heardRuns;
        };

        struct DohMark
        {
            /*  The cue whose GO a Doh took back, when the mark carries a root:
                the cue the next GO carries on; and when. `stepTick` is the
                erased `g`'s tick, as a seek may have moved it. */
            std::string cue;
            std::int64_t goTick = -1, dohTick = -1, stepTick = -1;
            std::optional<DohRoot> root;

            /*  WHAT A DOH LEFT WITH DEVICES' OPERATORS, ON ONE LIST (D1): per
                marked cue - the cue whose GO the Doh took back - the cues of
                that GO whose sends were left. Each entry lives by its marked
                cue (§24, HP): a GO that reaches the cue consumes it, a GO past
                it, a jump or a deliberate fire forgets it, a GO before it keeps
                it. */
            std::map<std::string, std::vector<std::string>> left;
        };

        const DohMark* markFor (const std::string& listId, const std::string& standby) const;

        /** The list's mark, or nothing. Tests. */
        const DohMark* markOf (const std::string& listId) const
        {
            const auto found = marks.find (listId);
            return found == marks.end() ? nullptr : &found->second;
        }

        struct Resumed
        {
            std::vector<std::string> made;
            bool resumed = false;
            std::string carriedOn;
        };

        Resumed resumeStandby (Engine& engine, std::int64_t tick, const juce::ValueTree& list,
                               const DohMark& mark, const std::vector<std::string>& supplied);

        /*  WHERE A RUN THE DOH PAUSED HAD GOT TO, read off its playhead by the
            hook on the tick after the press: `go.dohPlayhead`'s handler, which
            writes it into the root - and starts the cue from its top instead
            when the length the log knows says it was all but over there. */
        void notePlayheadOf (Engine& engine, std::int64_t tick, const std::string& runId,
                             double from, int range);

        /*  A SEEK OF THE RESUME ARM (GN, namespace draft §24.12): the hand put the cue somewhere and
            it plays there, so the resume is spent - not revoked - and what was
            left with devices' operators stays. Asked before the seek launches
            it. */
        void seekingRun (Engine& engine, std::int64_t tick, const std::string& runId);

        /*  WHETHER A RECORD DECIDED BEFORE A DOH, ON THE STATE IT UNDID, IS ONE
            TO IGNORE (GZ): a run the Doh handed back in this very tick. */
        bool handedBackIn (const std::string& runId, std::int64_t tick) const;

        /*  THE LENGTHS THE LOG'S HEADER RECORDED (§24, GM), which a Doh's
            decisions read so that a session and its replay agree to the bit:
            `serve` hands them in after writing the header; with none - a test,
            or a replay, which hands its own in as the media durations - the
            media durations are read. */
        void setLoggedDurations (std::map<std::string, double> lengths)
        {
            loggedDurations = std::move (lengths);
            haveLoggedDurations = true;
        }

        /*  WHAT ELSE THE RECORD HEARS. A fire by name or a trigger on its list
            - a pad's included, and a hand on a pad of the bank the GO armed -
            makes the Doh refuse (the author, 2026-09-30, evening): the GO is
            being played on. An engine fire whose cause is the record's own GO
            is that GO's (a start cue). Esc since the GO leaves the runs to Esc. */
        void noteFireOnList (const std::string& cueId, const std::string& from, std::uint64_t cause);
        void noteEscape() noexcept;

        /*  A START CUE'S TARGET FIRED UNDER THE GO THAT FIRED THE START CUE:
            `cue.fire`'s cause, set around the fire so what the target makes
            carries that GO, and tested first - a cause a Doh took back fires
            nothing. */
        bool causeTakenBack (std::uint64_t cause) const { return takenBackSerials.count (cause) > 0; }
        void setFireCause (std::uint64_t cause) noexcept { currentGo = cause; }

        /*  A FIRE OF A CUE A DOH LEFT WITH A DEVICE'S OPERATOR (§24, HP): by
            name or by a trigger it is a deliberate send, and the cue is no
            longer held back; by a GO's start cue it is that GO reaching it. */
        /*  AND IT SPENDS A RESUME OF THE CUE (D2): fired by name, by a trigger
            or by a start cue, it starts from its top, the arm at the point
            revoked first. */
        void markFire (Engine& engine, std::int64_t tick, const std::string& cueId,
                       const std::string& from, std::uint64_t cause);

        /*  A JUMP ON THE LIST RETIMES THE HISTORY THE GO LIVED IN: the record
            is forgotten, and so is everything a Doh left on that list - its
            resume revoked. */
        void forgetGoOnJump (Engine& engine, std::int64_t tick, const std::string& listId);

        /*  Which GO a run belongs to: the GO whose effect made it (a footer an
            older act ran because of it) before the GO that made or adopted it. */
        std::uint64_t goOfRun (const std::string& runId) const;

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

        /** The cue's twenty-three EQ rows, read through the schema so a saved
            flat EQ - which the writer omits - reads as flat. */
        audio::EqSettings eqOf (const juce::ValueTree& cue,
                                const std::map<std::string, double>* moved = nullptr) const;

        /*  The cue's inserts against the show's set, in chain order: one
            FxSetting per entry, disabled and empty where the cue has no Fx
            for it (PR 9a.8). Through the schema for eqOf's reason. */
        std::vector<FxSetting> fxOf (const juce::ValueTree& cue,
                                     const std::map<std::string, double>* moved = nullptr) const;

        /*  Counts the pass a ranged run is on and places the boundary out of
            it, once, when it comes into the placement horizon.

            Tick thread, below the null-player gate, because everything it does
            is arithmetic on the sample counter. A replay reaches the same
            answers by re-injecting the `run.range` records this submits - and
            reaches no answer at all about which PASS, which is right: a pass is
            a readout and §3.15 says readouts do not replay. */
        void advanceRanges (Engine& engine);

        /*  Where the playhead is IN THE FILE, for every run that is sounding,
            so a client can draw one over a waveform (§3.30).

            A PASS OF ITS OWN, and not a line inside `advanceRanges`: that loop
            continues on `range < 0`, which is every kind but media and every
            media cue with NO ranges - the ordinary media cue, and the only run
            the coloured bar is ever drawn over. Paid there, the debt would have
            been paid for every case except the one that needed it.

            Tick thread, below the null-player gate, because it is arithmetic on
            the sample counter and there is no counter without a Player. A
            replay leaves `position` at nought exactly as it leaves
            `rangeIteration`, which is §3.15: a readout is not an event. */
        void updatePositions (std::int64_t tick);

        /*  THE PICTURES (Phase 8a, namespace draft 35.4): every video run's
            layer brought up a launch horizon ahead, the points of its fade-in
            placed with it, its fade-out placed when Esc asks for one, and its
            layer taken off when the run ends. Above the audio gate: a show
            with no player still shows pictures, on the tick's own clock. */
        void advanceVideo (Engine& engine, std::int64_t tick);

        /*  A video run taken to black over `ticks` and then ended: Esc's, and
            Doh!'s for a picture seen (VK). */
        void fadeOutVideo (const std::string& runId, std::int64_t tick, int ticks);

        /*  WHAT THE NEXT GO STARTS, READ AHEAD OF IT (VX; namespace draft §48):
            its stills and movies handed to the picture side, and every cue got
            ready - its sounds too - written as `ListState::ahead`. Made again
            only when the show changed (a standby, the focus, an edit) or the
            lengths a backwards movie starts from did; and while a file is
            missing, looked for again once a second, so one put back is found. */
        void prepareStandbyVideo();
        std::vector<video::Preload> preloadsSent;
        std::uint64_t revisionPrepared = 0;
        const std::map<std::string, double>* durationsPrepared = nullptr;
        const audio::EditRenders* rendersPrepared = nullptr;
        bool aheadMissing = false;

        /*  WHICH GROUPS ARE LIVE, as a number (namespace draft §49, ABF): an
            armed bank's pictures are read ahead, and the list is made again
            when a bank arms, closes or loses a strip - keyed on the banks and
            not on their members' runs, which end and are armed again a tick
            apart. */
        std::uint64_t banksPrepared = 0;
        std::uint64_t banksKey();
        std::int64_t aheadLookedTick = -1;

        /*  Where a video point lands: Go.dot's sample now, plus a launch
            horizon - the audio clock when there is one, the tick's otherwise
            - and -1 when there is no clock to place it on at all. */
        std::int64_t videoSampleAhead() const noexcept;
        int videoLeadTicks() const noexcept;
        std::int64_t videoSamplesFor (double seconds) const noexcept;

        /*  The inputs' peaks, taken once a tick into `inputMeters`. */
        void takeInputMeters();
        std::vector<double> inputMeters;

        /*  Recomputes every live run's effective level from its own and its
            ancestors', and hands the media ones to the audio side.

            ONE PLACE, AFTER THE FADES, and that is the point of it existing at
            all rather than each fade writing the voice itself. A fade knows what
            IT changed; only something that walks the tree knows what that means
            for a member three levels down whose own fade is not running. Written
            as a pass, a group trim reaches every descendant on the tick it
            moves, including the ones nothing else is touching. */
        void applyLevels();
        void applyRouting();

        /*  EVERY OUTPUT'S GAIN (namespace draft §38): its own trim plus the
            trim of its DCA and of every DCA that one sits inside, handed to the
            audio side each tick. A DCA counts at each place (WQ): a cue marked
            with it is trimmed by it in `applyLevels`, and the output it plays
            through again here. Pushed every tick rather than on a change, so
            an audio side brought up again - a rate change, a device back -
            has its outputs' gains on its first tick without being told. */
        void applyOutputLevels();

        /*  The DCAs from `first` up its nesting, outermost last, bounded by how
            many DCAs the show has: one walk for a cue's mark and an output's. */
        std::vector<std::string> dcaNestingFrom (std::string first) const;

        /*  WHAT EACH SOUNDING MEDIA CUE'S LEVEL LANE ASKS FOR THIS TICK
            (namespace draft §20.4), into `Run::laneDb` for `applyLevels` to add.
            Just before it, below nothing: with no Player there is no sample
            clock, no second of any file, and every lane's term stays nought.
            The points are re-read when the show's revision moves - `applyEq`'s
            gate - and read at the second the voice will be at one slew ahead. */
        void applyLanes();

        /*  EVERY SOUNDING MEDIA CUE'S SPEED (namespace draft §22.4): the cue's
            `rate` re-read when the show's revision moves - unless a speed fade
            holds the run - and any change placed on the voice a launch horizon
            ahead, held until then and ramped over a tick, on the run's own
            clock and the Player's alike, so the two never disagree. Between the
            launch and the range boundaries, which read that clock. `rateNow` is
            the readout. */
        void applyRates();

        /*  A SLICE'S IN OR OUT EDITED WHILE IT SOUNDS (namespace draft §33,
            the author's ruling of 2026-10-06): heard at once. On a revision
            change each sounding ranged run's slices are compared with what its
            slots play; a move of the one sounding is placed on its slot a
            launch horizon ahead - the file carries on where it is if it is
            inside the new loop, or jumps to the new in-point, faded, if the new
            out is behind it - and the run's clock moves with it. A slice not
            sounding takes its new points when it is entered. An armed cue not
            yet launched is armed again once its edits stop. Tick thread, after
            the speed and before the boundaries. */
        void applySlices (Engine& engine, std::int64_t tick);
        void moveSoundingSlice (Run& run, const RangeSpec& wanted, std::int64_t now);

        /*  BACKWARDS AND BOUNCING (namespace draft §41). Where the run's reader
            is - forwards always, at the speed's size - and where its file is, at
            a sample; a turn of its direction placed on its slot at a sample; a
            slice's start placed when it is entered backwards or bouncing; and
            the end of a cue with no ranges that has played backwards, which
            Go.dot places itself. */
        double readerSecondAt (const Run& run, double sample) const;
        double fileSecondAt (const Run& run, double sample) const;
        void turnRun (Run& run, double sample, int direction);
        void anchorSlice (Run& run, int slot, double sample, double readerAt, const Run::SlicePoints& slice);
        void endTurnedRuns();

        /*  THE PASS (§20.9), just after `applyLanes` and before the sum: the
            ride's value while nobody holds it, the hand's level as the voice's
            lane term from the first touch (latch, DH), a sample of it per tick
            where the voice is, and - when the pass ends - the lane it leaves,
            written in one engine `node.set` (DK). */
        void recordLane (Engine& engine);

        /*  A sounding cue's EQ, kept up with the document (Phase 9a): the
            routing pass's shape, gated on the same revision, pushing only
            the runs whose twenty-three rows differ from what the voice holds. */
        void applyEq();
        void applyFx();

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

        /*  WHAT A GO ON THIS CUE STARTS FIRST, as the cues themselves: the cue
            when it is not a group; for a group, the first enabled member of a
            sequence or every member of a timeline with no pre-wait, recursively;
            nothing for a sampler group, whose members a hand starts. Walked once
            here so the two that ask cannot disagree about a scene: `armablesFor`
            keeps its sounds, the read-ahead its pictures and movies (namespace
            draft §48, AAL). */
        std::vector<juce::ValueTree> launchedFirst (const juce::ValueTree& cue) const;

        /*  Every group a pointer at this cue stands in, outermost first, with
            the cue itself last when it is a group, or nothing when it is in
            none: the chain `prepareStandby` builds or descends through, by
            identifier. What a horizon prepares, and therefore what a moving
            pointer leaves behind - its first is the outermost group of the
            block the pointer is in. (It answered with that first alone, as
            `horizonRootFor`, until a block under a running act could be left
            behind too: 2026-10-01, namespace draft §23.9.) */
        std::vector<std::string> horizonGroupsFor (const juce::ValueTree& list,
                                                   const std::string& cueId) const;

        /*  WHETHER THE POINTER HAS LEFT THIS PREPARATION BEHIND, `horizon` being
            `horizonGroupsFor` of the cue it is on. A run made ready in case and
            not the pointer's own: at the top of a list, when it is not the
            block the pointer is in; under another run (2026-10-01, namespace
            draft §23.9), when it is a block nobody adopted whose scene is not
            one the pointer stands in, and its parent is neither being stopped
            nor a preparation left behind too - or an arm under a manual group
            playing its members, which takes only what a GO asks for (ID). What
            `armStandby` gives back. */
        bool leftBehind (const Run& run, const std::string& standby,
                         const std::vector<std::string>& horizon) const;

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

        /*  EVERY CUE THE BLOCK WOULD PREPARE IF IT COULD - the derived lines
            first, then the written header's, deduplicated. `preparableIn` is
            this filtered by `isPreparable`, and `settledWord` compares the two
            sizes to decide `partial`, so both have to be drawn from one list or
            the comparison is between different questions. It was: the word was
            compared with the WRITTEN header alone, and a scene whose header is
            entirely derived - which is what §13.7's whole shape encourages -
            read `partial` for ever with nothing wrong with it. */
        std::vector<std::string> blockCuesIn (const juce::ValueTree& group) const;

        /*  The cues under this node whose `preset` names the group, in document
            order - §13.7's derived header lines. A walk of the group's own
            subtree rather than of the show, because a `preset` names an
            ANCESTOR and anything that could name this group is under it. */
        void collectPresetsOf (const juce::ValueTree& node, const std::string& groupId,
                               std::vector<std::string>& out) const;

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
                               std::vector<std::string>& used, std::int64_t tick,
                               const std::set<std::string>& leaveOut);

        /*  Whether everything a `preparing` phase issued has arrived: a media
            arm armed, a network cue finished. */
        bool preparationSettled (const GroupJob& job) const;

        /** Which word from `preparedness` a settled preparation ended on. */
        const char* settledWord (const GroupJob& job, const juce::ValueTree& group) const;

        /*  The write half of a network cue, which a prepared one reaches only
            after the read has come back. Fills in what a `verified` wait needs
            and queues the datagram. */
        void writeOscNow (OscJob& job);

        /*  A prepared group run becoming a live one: the run turns `playing`,
            it is told where the pointer entered, and its job leaves the hold.

            ONE FUNCTION FOR THREE DOORS, because GO reaches a prepared group
            three ways - through `fireStandby`'s descent when the pointer is
            inside it, through `armInternal` when the pointer is on it, and
            through `fireKind` when its own parent's job launches it - and three
            copies of this would be three things to keep in step in the one
            place where being out of step means a scene created twice. (And a
            fourth since 2026-10-01, namespace draft §23.9: `fireStandby`'s
            member path, when the pointer is on the row of a scene made ready
            under an act that is running, or under the block of a group the same
            GO enters - adopted as the descent adopts a block under a running
            group, left for that group's job to launch.)

            `enters` IS THE WHOLE OF THE THIRD DOOR. Only the OUTERMOST group a
            press touches starts; the rest are told where the pointer entered
            and left standing, exactly as `fireStandby` has always left the
            groups it created. An inner group that started here would spawn its
            member on the next tick while its parent was still running the
            header that is supposed to come first - the scene beginning from the
            inside out. */
        void adoptPrepared (const std::string& runId, const std::string& entersAt,
                            bool enters, std::int64_t tick);

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

        /*  A MIC CUE CLAIMING ITS CHANNEL AND TAKING ITS TRACK (Phase 9b,
            namespace draft §18.5): the claim first, as a media cue's are; then,
            with an audio side, what would fail it in words, the wait when the
            channel is held - armed with no track - and the arm on the
            channel's own track. */
        void armMic (Engine& engine, const juce::ValueTree& cue, const std::string& runId);

        /*  A mic run whose channel has come free asks to be armed, once:
            `run.arm`, the sampler's door. The tick's, with an audio side. */
        void armWaitingMics (Engine& engine);

        /*  THE TAKES' HOOK (Phase 9c): the presses asked for placed at the
            launch latency, what the audio thread did by itself reported as
            `take.closed`, the playheads read, and each channel's `through` as
            the cue holding it says. Below the null-player gate. */
        void serviceTakes (Engine& engine);

        /*  A MIC CUE'S GO ON ITS SAMPLING CHANNEL (decision CF): what its
            `onGo` says, once the cue both has GO and holds the channel. */
        void takeOnGo (const std::string& runId, const std::string& channelId);

        /*  THE GO HALF OF IT, for either road a GO reaches a mic run by - fired
            cold (`fireKind`), or armed ahead by the standby and launched
            (`armInternal`, 2026-09-30): now when the run holds its channel,
            and when the claim lands otherwise. Nothing on a channel that does
            not sample, or with no take table. */
        void applyTakeOnGo (const juce::ValueTree& cue, const std::string& runId);

        /** A channel let go, the take held; and a waiting cue's GO acted on. */
        void takeReleased (const std::string& slotId, const std::string& toRun);

        /*  The half of an arm below the track: the routing, the offset, the
            ranges and the request itself, for a run that already holds its
            voice. `armMedia` reaches it after choosing a track; a seek reaches
            it for the track the run has. The level is handed in because the
            two disagree about it: an arm plays the cue's authored level, a
            seek keeps the one a fade had brought the run to. */
        void requestArmOn (Engine& engine, const juce::ValueTree& cue, Run& run,
                           double levelDb, int liveFirstInput = -1, int liveWidth = 1);

        /** A bundle-relative file name as the path the audio side opens. */
        std::string mediaPathOf (const std::string& named) const;

        /*  A state file's name resolved under the plugins folder - string
            work, no disk. One that would climb out of it resolves to a file
            that is not there, so the load fails in words. */
        std::string statePathOf (const std::string& named) const;

        /*  Builds the runs a plan names, outermost first, under the groups
            `runFor` already holds, and the jobs that carry them on. A jump
            hands in an empty map; a group seek hands in the scene's own run
            and the runs above it, so they stand and only what is inside is
            made again. */
        void seatPlan (Engine& engine, std::int64_t tick,
                       const std::vector<PlannedRun>& wanted,
                       std::map<std::string, std::string>& runFor,
                       const std::function<std::string()>& nextId,
                       const SeatResume* resume = nullptr);

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
        /*  `tick` is the command's own (2026-10-03, Doh! D3): what a stop's
            arrival is counted from, the same number live, where the hooks have
            just set `currentTick` to it, and in a replay, where they never do. */
        void fireFade (const juce::ValueTree& cue, const std::string& runId, std::int64_t tick);
        void fireStop (const juce::ValueTree& cue, const std::string& runId, std::int64_t tick);

        /** Which slice a transport cue's `range` names, as a playlist place, or -1. */
        int advanceTargetOf (const juce::ValueTree& cue, const Run& run) const;

        /*  A network cue firing: one write to a mounted node, queued for the
            end of this tick. No Engine here either, and for the same reason. */
        void fireOsc (const juce::ValueTree& cue, const std::string& runId, std::int64_t tick);
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

        /*  `points` is a drawn curve, or empty for the two words; a stop
            passes it empty, having no curve to draw (§14.6). `stopLagTicks`
            holds a stop of its own back that many ticks past the level's
            arrival: a fade that also moves the speed stops once the speed has
            been heard too (§22.6, ED). */
        void beginFade (std::int64_t tick,
                        const std::string& selfCueId,
                        const std::string& targetCueId,
                        const std::string& selfRunId, const std::string& kind,
                        double toDb, double seconds, FadeCurve, bool stopWhenDone,
                        std::vector<doc::FadePoint> points,
                        int stopLagTicks = 0);

        /*  A FADE ON A SPEED (namespace draft §22.6): the target's own speed,
            from wherever it stands to `toRate`, straight or S in the ratio. A
            media cue's only: aimed at a group, a mic cue or a cue not running
            it moves nothing (EC). Its takeover key is `rate:` and the run, so
            it takes over a speed fade and never a level fade. `alone` is a fade
            that moves the speed only: then it says what became of a target
            that is not there, and carries the fade's stop, which lands once the
            speed has reached the voice - a horizon and a tick after its last
            breakpoint (ED). With the level moving too, the level's job does
            both. Answers whether a job now moves the speed. */
        bool beginRateFade (std::int64_t tick,
                            const std::string& targetCueId, const std::string& selfRunId,
                            double toRate, double seconds, FadeCurve, bool stopWhenDone,
                            bool alone);

        /*  A FADE ON WHAT THE TARGET OWNS (namespace draft §26): one job per
            entry - a send, an EQ number, a plugin value - each from where the
            run's value is to where the fade says, in its own domain, keyed
            `move:`, the run and the entry. A media or a mic run only; anything
            else is §3.8's silent no-op, said once when `alone`. The first job
            carries the stop when asked. Answers whether any job began. */
        bool beginMoveFades (std::int64_t tick,
                             const std::string& targetCueId, const std::string& selfRunId,
                             const std::vector<FadeMove>& moves, double seconds, FadeCurve,
                             bool stopWhenDone, bool alone);

        /*  Where a run's entry stands now: what a fade moved it to, else what
            the lock rides, else the cue's own - a send it has not got at
            silence, a plugin value it has not set at the catalogue's default. */
        double movedValueNow (const Run& run, const std::string& entry) const;

        /** Every move job on a run let go - Esc's, for what it leaves where it is (PD). */
        void releaseMovesOf (const std::string& runId);

        /** A moved entry in words, for Doh!'s list: "send to Reverb", "EQ band 2 gain". */
        std::string moveWords (const std::string& entry) const;

        /*  A FADE AIMED AT A DCA (`fade/dca`, Phase 6): the DCA's trim moves
            from wherever it stands to `toDb`. No run to find and nothing to
            stop - a DCA has no sound of its own - and it takes over from a
            fade already moving the same DCA, from where that one had got to. */
        void beginDcaFade (std::int64_t tick,
                           const std::string& dcaId, const std::string& selfRunId,
                           double toDb, double seconds, FadeCurve,
                           std::vector<doc::FadePoint> points);

        /*  THE DCAS ABOVE A CUE, nearest first: its own mark, the DCA that one
            sits inside, and so on up - read off the document once per show
            revision into `dcaMarks` and summed per run per tick. Empty for a
            cue marked with nothing. */
        const std::vector<std::string>& dcaChainOf (const std::string& cueId);

        /*  A CUE'S DCA MARK AS THE RUNNER READS IT (namespace draft §50): the
            DCAs above it, nearest first, and what the mark carries - the
            picture's curve in % and the sound's offset in dB (ABT). Null for a
            cue marked with nothing, or with a DCA the show does not declare. */
        struct DcaMarkSpec
        {
            std::vector<std::string> chain;
            double curve = 0.0;
            double offsetDb = 0.0;
        };

        const DcaMarkSpec* dcaMarkOf (const std::string& cueId);

        /*  What a mark adds to the level of every run it reaches: its DCAs'
            trims, and its offset - which never lifts silence (ABV). */
        double dcaTermsOf (const std::string& cueId);

        void advanceFades (Engine& engine, std::int64_t tick);

        /*  THE OSC CUES' CURVES, ONE TICK (namespace draft 45, O.4): each
            message whose values a curve moves is written where they changed,
            and a cue at the end of its duration is handed to `advanceSends` to
            be done by its wait. A hook, before `advanceSends`. */
        void advanceCurves (Engine& engine, std::int64_t tick);

        /*  THE PROCESS CUES, ONE TICK (namespace draft §51): a run stopping is
            ended, and every running one's patch is handed what arrived - what
            devices reported since the last tick, the rows it hears - and what it
            sent is done: written to a device as a cue's write is, or submitted
            with the origin `process:<run>`. A hook, after the curves. */
        void advanceProcesses (Engine& engine, std::int64_t tick);
        void applyProcessSends (Engine& engine, const std::string& runId, const std::string& cueId,
                                const std::vector<process::Sent>& sent);

        /*  THE PASS, ONE TICK (O.9): each armed curve takes the device's newest
            report or the hand's ride, latched from the first, sampled on the
            cue's clock; at the end the curves are spliced, judged and written
            in one `node.setMany`. A hook, before `advanceCurves`. */
        void recordCurves (Engine& engine, std::int64_t tick);

        /*  An OSC cue's messages and the curves on them, read off the document:
            its own message first, then each further one, in order. */
        std::vector<CurveTarget> curveTargetsOf (const juce::ValueTree& cue) const;

        /*  The duration a cue's curves play: its row, or the longest curve's
            last point where that is nought. */
        double curveDurationOf (const juce::ValueTree& cue, const std::vector<CurveTarget>& targets) const;

        /*  One message's values written by a curve: into the tree through the
            device's door, onto the wire where its device is spoken to. False,
            and `why` set, when the door refused. */
        bool writeCurve (CurveJob& job, CurveTarget& target, const osc::Values& values,
                         std::int64_t tick, std::string& why);

        void advanceSends (Engine& engine);
        void advanceWaits (Engine& engine, std::int64_t tick);
        void armStandby (Engine& engine);
        void advanceGroups (Engine& engine);

        /*  `Run::seekable`, a readout, written every tick from `seekableNow`
            for every run not over (K9). A hook's: nothing is logged. */
        void mirrorSeekable();

        /*  `Run::roundFrom` and `Run::roundLength` (namespace draft §30.6, S8):
            for a scene a seek would move, where the round it is in began and
            how long it is, solved by `solveRound` once a round - again only
            when the round, the scene's start or the show changes. Nought
            everywhere else. A hook's, after `mirrorSeekable`: nothing is
            logged. */
        void mirrorRound();

        /*  A PREPARATION GIVEN BACK, as records: every value the block under
            `runId` pre-sent is written back with an ordinary `node.set`, and
            then `run.revoke` ends the block and everything in it. The pointer
            moving away does it (`armStandby`), and so does a group stopped
            while it was only being made ready (`advanceGroups`, 2026-09-30).

            HOOKS ONLY. It submits, and a handler that submitted would put the
            records in a replay twice - once from the log and once from itself.
            The handlers that revoke - `audio.clockMoved` and the settings
            operations `audio.apply`, `audio.setup` and `plugin.load` - call
            `revokePrepared` and put nothing back themselves; since K5
            (2026-10-02) they hand what they revoked to `putBackWhenWritable`,
            whose hook puts it back (`advancePutBacks`).

            AND THE BLOCK'S NETWORK JOBS ARE SETTLED HERE, so that a pre-send
            whose answer is read later in this same tick writes nothing. */
        void submitRevocation (Engine& engine, const std::string& runId);

        /*  ITS FIRST HALF ON ITS OWN: the values put back and the network jobs
            settled, and no `run.revoke` yet - for a scene Doh! gives back
            while something it set going still moves under it (namespace draft
            §24). Each restore is submitted once, so asking again is safe. */
        void submitRestores (Engine& engine, const std::string& runId);

        /*  ONE PASS OVER THE PUT-BACKS OWED (2026-10-02, K5's review, namespace
            draft §23.16): each pre-sent value put back while the desk still
            holds what the pre-send wrote, kept owed until a later pass sees it
            landed, and let go once it has, or once somebody else wrote the
            address. True when it submitted a `node.set`. Hook-side. */
        bool advancePutBacks (Engine& engine);

        /*  WHETHER A JOB OF THE RUNNER'S STILL DRIVES A RUN: a fade's, a
            network cue's, a group's own, or the end a memo is owed. What no job
            drives, nothing will end unless it is asked to stop. Hook state, read
            by a hook to shape what it submits. */
        bool drivenByAJob (const Run& run) const;

        /*  ONE MEMBER OF A STOPPING GROUP, ended the way the group is ending:
            `run.stop` when it was stopped, `run.kill` when it was killed, once
            a member rather than once a tick, and `run.done` for a post-wait a
            kill finds (namespace draft §23.2) - its voice killed beside it, the
            chain's tail with it (§23.6). Hook-side, as it submits. */
        void endMember (Engine& engine, const Run& member, bool graceful);

        /*  WHAT SOMEBODY ASKED FOR INSIDE A BLOCK THAT NEVER STARTED, ended the
            way the block is (`endMember`), and answering whether any of it is
            still unfinished - which holds the block's revocation back
            (namespace draft §23.3). */
        bool endAskedForIn (Engine& engine, const std::string& blockRun, bool graceful);

        /*  WHETHER A RUN IS UNDER A KILL: any run above it that skips its
            footer. A killed group reaches its members through its own job, a
            tick after the press, and a member is being killed in that tick all
            the same (namespace draft §23.2). */
        bool underAKill (const Run& run) const;

        /*  WHETHER A FADE-AND-STOP IS STILL FADING A GROUP DOWN (2026-10-02,
            K3, namespace draft §23.14): a job that ends in a stop holds it - a
            stop cue's `fade`, a fade cue that stops when done - its stop not
            yet due, and no abort on the group or above it. While it holds,
            the group's job plays the scene on under the fading level instead
            of stopping its members. Hook state, read by a hook. */
        bool fadingToItsStop (const Run& group) const;

        /*  WHETHER A RUN, OR ANY UNFINISHED RUN ABOVE IT, IS `stopping`
            (2026-10-02, K3's review, namespace draft §23.14, KU): a scene on
            its way out, or one inside such a scene. The standby's two walks
            neither descend into one nor adopt a block made under one - a fresh
            run is built instead, as for a scene Esc is fading down. */
        bool goingOut (const Run& run) const;

        /*  WHETHER A KILL HAS REACHED A RUN AND STILL STANDS: its own
            (`skipFooter`, which only `run.kill` and a double Esc write, with
            the stop it asked - a seek withdraws that) or one above it. Since
            H4 (namespace draft §23.10) what keeps a run from launching, firing
            or writing after the press: nothing a kill has reached starts
            anything. Handler state, so a handler may decide by it. */
        bool beingKilled (const Run& run) const;

        /*  WHETHER A RUN HANGS FROM A ROOT THE DOUBLE ESC SPARES: one only made
            ready, nobody having reached into it - the rule `stopEveryRoot`
            spares by (§23.3). What its pre-sends have queued goes out (§23.10). */
        bool sparedByThePress (const std::string& runId) const;

        /*  A SAMPLER GROUP'S MEMBERS PHASE, every tick: arm every member that
            has no run onto its strip, let a closing group finish, end the idle
            members on strips another group took, and hand a voice to a member
            waiting for one. Decides and submits, as every hook does. */
        void samplerTick (Engine& engine, GroupJob& job, const juce::ValueTree& group,
                          const Run& groupRun);

        /*  Fader-start and fader-stop on every sampler strip whose endpoint
            is a fader: the edges of §3.9a, read off each holder's `trim` and
            the touch table, and submitted as `strip.press` / `strip.release`. */
        void samplerEdges (Engine& engine);

        /*  A SOLO IN A BANK (2026-09-25): whether any clip of the sampler
            group run `groupRunId` is soloed and still held, and, every tick,
            the solos let go of whose clips have stopped - above the null
            player's gate, so a replay lets go the same. */
        bool bankSoloed (const std::string& groupRunId) const;
        void releaseSolos();

        /*  Takes a strip for a sampler member, or queues for it behind the
            run holding it - which is how a takeover waits for a playing clip
            to finish rather than cutting it off. */
        void claimStripFor (Run& run, const std::string& stripId);

        /*  Which strip member `cueId` of `group` lands on: its place among the
            group's media members, counted onto `samplerStrips()`. Empty past
            the last strip. */
        std::string stripForMember (const juce::ValueTree& group, const std::string& cueId);

        /*  A sampler group's arming, in the handler: `takeover=group` closes
            every other armed sampler group. Asked when the group fires and at
            every refresh. */
        void takeOverFrom (const std::string& groupRunId, const juce::ValueTree& group);

        /*  The short fade to silence a hold clip's release and a play-out
            clip's `stop` second press both make, then the stop. A fade job
            with no cue of its own behind it. */
        void beginReleaseFade (const std::string& runId, double seconds, std::int64_t tick);

        /*  A PICTURE'S RELEASE (namespace draft §49): taken to black over the
            same `releaseFade` and then ended, as Esc takes one - and a movie's
            locked sounds faded with it over the same seconds. A dB fade would
            leave its opacity alone and let the sweep cut it a tick later. */
        void releaseVideo (const std::string& runId, double seconds, std::int64_t tick);

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

        /*  A MOVIE'S OWN SOUNDS (namespace draft 37.5, WJ): the media cues
            locked to it, in document order - which its GO fires with it, on
            the same sample, and its standby arms with it. `followsAMovie`: a
            media cue whose `lockedTo` names a video cue that is there, which
            no GO and no group fires on its own. */
        std::vector<std::string> soundsLockedTo (const std::string& movieCue) const;
        bool followsAMovie (const juce::ValueTree& cue) const;

        /*  ONE WALK OF THE SHOW PER EDIT, not one per movie per tick: every
            movie's locked sounds, made again when the document's revision moves
            - an override switching a sound on or off moves it too - since a
            bank asks for its movies' every tick (namespace draft §49). */
        mutable std::map<std::string, std::vector<std::string>> lockedSounds;
        mutable std::uint64_t lockedSoundsAt = 0;

        /*  The sounds of a movie run fired with it: adopted when the standby
            armed them, else made under an identifier drawn from the movie
            run's and the cue's - so a replay, which re-supplies the movie's,
            makes the same - and launched in the same tick. */
        void fireLockedSounds (Engine& engine, std::int64_t tick, const std::string& movieCue,
                               const std::string& movieRun);

        /*  And ended with it: stopped when it was stopped, killed when it was. */
        void endLockedSounds (Engine& engine, const std::string& movieRun);

        /*  A MOVIE MEMBER'S SOUNDS, in the bank's tick (namespace draft §49, ABA,
            ABH): made under it as it is armed on its strip, its row reading
            pending while one waits for a voice, and its press let go - a
            `run.fire` - once each is ready to start on the picture's sample.
            Records, so a replay has the same spawns and the same fire. */
        void movieMemberTick (Engine& engine, const Run& movie, bool stripHeld);

        /*  Whether a press on this movie member waits for its sounds rather than
            bringing it up at once: it has sounds locked to it. The document's
            answer, so the press's handler decides the same on replay. */
        bool pressWaitsForSounds (const Run& movie, const juce::ValueTree& cue) const;

        /*  A movie member's sounds whose movie never came up and is gone -
            killed idle, closed, taken back - are ended: armed under a run that
            will never fire them, they would hold their voices for good. */
        void endOrphanedSounds (Engine& engine);

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
        video::Sink* videoSink = nullptr;
        TakeTable* takes = nullptr;
        int samplesPerTick = 0;
        std::string mediaFolder;
        std::string pluginsFolder;

        std::vector<FadeJob> running;

        /*  Runs with nothing left to do, ending on the next tick.

            A MEMO IS THE WHOLE POPULATION and it is here rather than nowhere
            because §3.6 needs every kind of cue to report done: a sequence group
            whose second member is a note to the operator has to know when to
            move to the third. It ends on the tick AFTER it fired, exactly as a
            network cue with `wait: none` does, because that is when a report is
            allowed to leave and not because anything was waited for. */
        std::vector<std::string> finishing;

        /*  THE TARGETS OF START CUES THAT FIRED THIS TICK, fired by name on
            the next `beforeTick` - a hook, so a replay, which runs no hooks,
            fires none of them itself and takes the `cue.fire` records the
            session logged. The one-tick lag is the cost of the record. */
        std::vector<std::pair<std::string, std::uint64_t>> startsToFire;

        /*  THE MOVES OF JUMP CUES THAT FIRED THIS TICK (namespace draft §27),
            submitted as `standby.jump` on the next `beforeTick`, the start
            cue's road and for its reason. Landing a tick later also lands it
            after the GO that fired the jump cue has advanced the pointer, so
            the jump's placement is the one that stands. */
        struct JumpToMake
        {
            std::string list, target;
            bool andGo = false;
            std::uint64_t cause = 0;
        };

        std::vector<JumpToMake> jumpsToMake;

        /*  See `setOverrideDocument`. Null in a runner nobody registered commands for,
            and then an enable or disable cue switches nothing. */
        doc::ShowDocument* overrideDocument = nullptr;

        /*  An enable or disable cue's switch of its target (§27, PK), noted on
            the GO it belongs to for Doh! (PS), stepping the standby off a cue
            switched off (PQ). */
        void switchOverride (const juce::ValueTree& cue, const std::string& runId);

        /*  One per group run in flight. A vector like every other job list
            here, and drained by the same `remove_if` on a retired flag. */
        ListState lists;
        const audio::MediaInfo* mediaInfo = nullptr;
        const std::map<std::string, double>* fixedDurations = nullptr;

        /*  Refreshed at the top of each tick from `mediaInfo`, and held for
            that tick so nothing swaps it mid-solve. */
        std::shared_ptr<const std::map<std::string, double>> durationsHeld;
        const std::map<std::string, double>* durations = nullptr;

        std::function<std::shared_ptr<const audio::EditRenders>()> renderSource;
        std::shared_ptr<const audio::EditRenders> rendersHeld;
        const audio::EditRenders* renders = nullptr;
        std::vector<GroupJob> scheduled;

        /*  The cue the standby was last seen on, so that arming it is asked for
            ONCE rather than on every tick it sits there.

            Engine state and not a model input: a replay never sets it, because
            a replay runs no hooks - and it does not need to, because the arm it
            would have asked for is a record in the log. */
        std::string armedStandby;

        /*  BLOCKS THE POINTER LEFT WITH SOMETHING PLAYING IN THEM - a member
            fired by name - which are given back once that has gone
            (`armStandby`, namespace draft §23.3). Hook state: the revocation
            it leads to is a record in the log. */
        std::vector<std::string> blocksToGiveBack;

        /*  RUNS A CLOCK MOVE OR A SETTINGS OPERATION (`audio.apply`,
            `audio.setup`, `plugin.load`) REVOKED, whose pre-sent values are
            still to be put back - or were submitted and not yet seen to land
            (`putBackWhenWritable`, `advancePutBacks`, namespace draft §23.16).
            Hook state, as the one above; a double Esc drops it (LP). */
        std::vector<std::string> owedPutBacks;

        /*  WHETHER `armStandby` HOLDS EVERY PREPARATION until the engine takes
            a write (K5's review, LM): set by a hand-over, kept through a double
            Esc so the standby is still made ready again (LC), and let go on the
            pass that finds nothing more owed. Hook state. */
        bool waitingForWrites = false;

        /*  THE TICK THE STANDBY MAY BE MADE READY AGAIN after those values went
            back (2026-10-02, K5's extension, §23.16, LE): five ticks, a tenth
            of a second, for the desk to apply the put-back before a fresh
            pre-send asks it what it holds. Hook state: the `run.prepare` it
            delays is a logged record either way. */
        static constexpr std::int64_t putBackSettleTicks = 5;
        std::int64_t prepareAfterPutBack = -1;

        /*  The tick being processed, so a stop fired inside a command
            handler can be scheduled against the same clock the tick hook
            reads. Set by beforeTick, which runs before the handlers do. */
        std::int64_t currentTick = 0;

        /*  The tick of the last GO that fired something, or none yet. */
        std::int64_t lastGoTick = -1;

        std::vector<OscJob> sending;

        /*  The OSC cues whose curves are playing (namespace draft 45). */
        std::vector<CurveJob> curving;

        /*  THE PROCESS CUES RUNNING (namespace draft §51): which run, which cue,
            and its patch as of which document revision. */
        struct ProcessJob
        {
            std::string self;
            std::string cue;
            std::string patch;
            std::uint64_t revision = 0;
            bool finished = false;
            bool failed = false;
        };
        std::vector<ProcessJob> processing;
        process::ProcessHost* processHost = nullptr;
        std::shared_ptr<const tree::TreeSnapshot> lastSnapshot;
        process::MidiInbox* processMidi = nullptr;
        std::optional<std::array<double, 6>> puckAxes;
        bool puckWanted = false;

        /*  What `process.send` handed each run, until the hook passes it on -
            handler state, capped per run so a replay, which runs no hook,
            holds a bounded amount. */
        std::map<std::string, std::vector<process::Input>> patchInputs;

        /*  The tick up to which heard values have been handed to the patches. */
        std::int64_t processHeardTick = -1;

        /*  The serial ports' lines, as `serial.heard` noted them, and where a
            patch's lines go out (PC.10). */
        serial::HeardLines serialHeard;
        serial::SerialTable* serialPorts = nullptr;

        /*  WHERE A MOVIE STARTS (§37, WL; §41): the second its playhead is put
            at by GO, how fast and which way it goes, the first range it plays,
            and where a movie with no ranges plays back to. Read by GO and by the
            read-ahead alike (namespace draft §48), so the frame read before GO is
            the frame GO shows. */
        struct MovieStart
        {
            double seconds = 0.0;
            double rate = 1.0;
            std::string rangeId;
            double pieceStart = 0.0;
        };

        MovieStart movieStartOf (const juce::ValueTree& cue) const;

        /*  ONE PER VIDEO RUN that is up or coming up (Phase 8a): its layer,
            the points placed for it, and - while Esc takes it down - the tick
            its fade-out ends, until which the job owns the run's ending, as a
            fade job owns its own. The sweep in `advanceWaits` reads that. */
        struct VideoJob
        {
            std::string self;
            video::LayerSpec spec;
            double opacity = 1.0;           // the cue's own, read at GO
            double fadeInSeconds = 0.0;
            bool placed = false;            // brought up on the picture side
            std::vector<video::Point> points;

            int fadeOutTicks = -1;          // asked by Esc, not yet placed
            std::int64_t endsAtTick = -1;   // the fade-out's last tick
            bool removed = false;

            /*  THE GEOMETRY'S POINTS, a vector per value (§36, VS) - the
                opacity's are `points` above - so a fade starts from where the
                picture is. */
            std::array<std::vector<video::Point>, video::propertyCount> moved;

            /*  A MOVIE'S PLAYHEAD (namespace draft 37, VZ, WA, WB): where in
                the file it is at `movieAt`, the sample of its last point; how
                fast it moves; how many passes it plays and how many it has.
                `movieFile` is the document's name for it, the key its length
                is found under. */
            bool movie = false;
            std::string movieFile;
            double movieLength = 0.0;       // as the show knew it at GO: the edit's, or the file's (nought until read)
            double moviePosition = 0.0;
            std::int64_t movieAt = -1;
            double rate = 1.0;
            bool movieEnded = false;

            /*  ITS RANGES (namespace draft 37.5, WL), as a sound's: the cue's,
                read again every tick so an edit is seen at once (TZ); the one
                playing, by identifier, and which pass of it. No range: the
                file once, from the start offset. */
            std::string cue;
            std::string rangeId;
            int pass = 0;

            /*  BACKWARDS AND BOUNCING (namespace draft §41): `bounce` is -1
                while a ping-pong range plays back from its out-point, and the
                file then goes the other way from the speed's; `pieceStart` is
                where a movie with no ranges plays back to - its start offset. */
            int bounce = 1;
            double pieceStart = 0.0;

            /*  WHAT THE DCAS ABOVE IT LEAVE OF ITS OPACITY (namespace draft
                37.5, WE), as last placed: all of it until one says less. */
            double dcaFactor = 1.0;

            /*  A SEEK'S LOCKED SOUNDS (namespace draft §47, AAC): the runs
                sought with it and not yet launched again, the picture held
                until they are - for at most `movieSoundWaitTicks`. */
            std::vector<std::string> soundsSought;
            int soundWaitTicks = 0;

            /*  The range index last reported with `run.range`, -1 for none. */
            int rangeReported = -1;

            /*  A PLAYING PICTURE FOLLOWS ITS EDITS (namespace draft §47, AAE):
                each moving value's row as last taken from the document -
                the opacity 0..1, the geometry in its own units - and whether
                an edit waits for a fade on it to let go. */
            std::array<double, video::propertyCount> rowSeen {};
            bool editWaiting = false;
        };

        std::vector<VideoJob> showing;

        /*  A FADE CUE MOVING A PICTURE (§36, VS): the values it moves and
            where to, from where they were when it began, over its ticks and
            along its curve - a point a tick, a horizon ahead. */
        struct VideoFade
        {
            std::string self;               // the fade cue's run
            std::string target;             // the video run it moves
            std::vector<std::pair<video::Property, double>> to;
            std::array<double, video::propertyCount> from {};

            /*  A MOVIE'S SPEED, moved on the Runner's side: the playhead is
                placed from it (WA). */
            bool movesRate = false;
            double rateTo = 1.0;
            double rateFrom = 1.0;
            std::int64_t startTick = 0;
            int ticks = 0;
            bool sCurve = false;
            bool stopWhenDone = false;
            bool begun = false;
            bool done = false;
        };

        std::vector<VideoFade> videoFades;

        /*  A video run's value of one property at `sample`, as the points the
            Runner placed say: the cue's own number before any. */
        double videoValueOf (const VideoJob& job, video::Property property, std::int64_t sample) const noexcept;

        /*  A point placed for a video run, on the job and on the picture side. */
        void placeVideoPoint (VideoJob& job, video::Property property, const video::Point& point);

        /*  THE DCAS ON A PICTURE (namespace draft 37.5, WE; §50, ABO, ABW): for
            each mark - on the run's cue and on each run's above it - its DCAs'
            trims summed in dB and turned into a factor along the fader's
            travel, bent by the mark's curve; the marks' factors multiplied, and
            a sampler member's hand with them. One with none marked. */
        double videoDcaFactorOf (const Run& run);

        /*  And followed: a point a horizon ahead on the tick the factor moves,
            after one holding where it was, so a fader at rest for a minute
            does not glide over that minute to where it went. */
        void followVideoDcas (VideoJob& job, const Run& run);

        /*  EVERY CANVAS'S LEVEL (namespace draft §38, WT): its own level times
            what its DCA, and every DCA that one sits inside, leaves of it along
            the fader's travel - the picture's law (37.5, WE). Handed to the
            video side on the tick one moves, every canvas at once. */
        void followCanvasLevels();

        /*  A fade cue aimed at a video cue: true when it was one, and handled. */
        bool fireVideoFade (const juce::ValueTree& fade, const std::string& runId, std::int64_t tick);

        void advanceVideoFades (Engine& engine, std::int64_t tick);

        /*  A movie's playhead a horizon ahead, its loops wrapped and its end
            reached (VZ, WB). */
        void advanceMovie (Engine& engine, VideoJob& job);

        /*  A PLAYING PICTURE TOLD WHAT ITS CUE SAYS NOW (namespace draft §47,
            AAE): the author, 2026-10-09, a playing cue's picture follows its
            edits at once, as a playing sound's EQ does. On a tick the show
            moved - or while an edit waits for a fade - the cue is read again:
            its look (blend, colour, fit, flips, grade and curves, mask) is
            restated on the layer; its opacity and geometry step to the row's
            new number a horizon ahead, or, where a fade or a ramp moves that
            value, when it lets go - so a fade never jumps. Which canvas, layer,
            source, file or insert stays the GO's. Under the lock no edit is
            taken, so nothing moves. */
        void applyPictureEdits (VideoJob& job, bool reread);
        std::uint64_t pictureRevision = 0;

        /*  A MOVIE'S PLAYHEAD AS ITS RUN READS IT (namespace draft §47, AAC):
            the file's second at the clock's now, the speed it goes at, the
            range it is in and which pass - what a sound's run reads, so the
            panel draws a movie's playhead where the picture is. */
        void publishMoviePlayhead (Engine& engine, VideoJob& job, Run& run);

        /*  Whether a run is a movie's: its playhead is its own (above), never
            the seconds since its GO. */
        bool isMovieRun (const std::string& runId) const noexcept;

        std::uint64_t videoOrder = 0;

        tree::MountTable* mounts = nullptr;
        tree::MountSender* sender_ = nullptr;
        midi::MidiSink* midiOut = nullptr;
        std::function<bool()> outage;
        DcaTable* dcas = nullptr;
        const LiveEdits* liveLayer = nullptr;
        const plugin::PluginTable* pluginTable = nullptr;
        const tree::TouchTable* touches = nullptr;

        /*  THE SAMPLER ROSTER, read once per show revision: every sampler
            strip in fill order, and which of them are pads. */
        std::vector<std::string> samplerRoster;
        std::set<std::string> gateStrips;
        std::uint64_t rosterRevision = 0;
        bool rosterRead = false;

        /*  WHAT THE FADER EDGES REMEMBER BETWEEN TICKS, per strip: which run
            was under it; whether a hand was on it a tick ago, and for how many
            ticks - the release edge is a hand that WAS there, and a start is a
            touch that has lasted; and whether this touch already started
            something, so one touch starts one clip. Hook-side memory, never a
            model input: the decisions it makes are records. */
        struct StripEdgeState
        {
            std::string holder;
            bool wasTouched = false;
            int touchedTicks = 0;
            bool fired = false;
            double lastTrim = -120.0;
        };

        std::map<std::string, StripEdgeState> stripEdges;

        /*  The DCA mark of every cue that has one - its chain and what it
            carries - keyed by cue, and the show revision it was read at.
            Rebuilt when the show changes - a mark, a nesting or a mapping
            edited - and never on a tick that changed nothing. */
        std::map<std::string, DcaMarkSpec> dcaMarks;
        std::uint64_t dcaChainsRevision = 0;
        std::uint64_t dcaChainsLayer = 0;     // the live layer's revision it was read at (§50, ABQ)
        bool dcaChainsRead = false;

        /*  EACH OUTPUT AS `applyOutputLevels` READS IT, at the revision it was
            read at: its channels, its own trim and its DCA nesting. */
        struct OutputGainSource
        {
            int firstChannel = 0;
            int width = 1;
            double trimDb = 0.0;
            std::vector<std::string> dcaChain;
        };

        std::vector<OutputGainSource> outputGainSources;
        std::uint64_t outputGainsRevision = 0;
        bool outputGainsRead = false;

        /*  EACH CANVAS AS `followCanvasLevels` READS IT, at the revision it was
            read at, and what was last handed to the video side. */
        struct CanvasLevelSource
        {
            std::string id;
            double level = 1.0;
            std::vector<std::string> dcaChain;
        };

        std::vector<CanvasLevelSource> canvasLevelSources;
        std::uint64_t canvasLevelsRevision = 0;
        bool canvasLevelsRead = false;
        std::vector<std::pair<std::string, double>> canvasLevelsSent;

        /*  Who asks a target what a value is. Null everywhere a replay or
            a tree dump runs, and a verified cue there finishes on its own
            records rather than on an answer nobody went and got. */
        tree::MountProbe* asker = nullptr;

        /*  THE OBSERVATION SWEEP: what the world was, at each step.

            §13.10. A step is a place the show was, and a place is only worth
            going back to if what was there is known - so at every applied
            trigger each target that can be asked is asked about every address
            this show writes to it, once. The answers are `mount.readback`
            records with the observation flag, so the log carries what the desk
            held at that step and a replay reads it back out.

            RATE-CAPPED PER MOUNT rather than per step, because steps are the
            operator's business and can come in threes: one sweep a second is
            what §13.10 promised and what M21 priced. A step during the cap is
            not queued for later - the next step gets a fresh sweep, and an
            observation of a moment that has passed is worth less than the cost
            of asking for it. */
        void observeAfterStep (Engine& engine, std::int64_t tick);

        /** The addresses this show writes, by mount, rebuilt when the show changes. */
        const std::map<std::string, std::vector<std::string>>& writtenAddresses() const;

        mutable std::map<std::string, std::vector<std::string>> addressesWritten;
        mutable std::uint64_t addressesFor = 0;

        std::uint64_t stepsSeen = 0, asked = 0;
        std::map<std::string, std::int64_t> observedAt;

        /*  The show revision `applyRouting` last pushed at. Nought is "never",
            so the first tick after a load pushes once and every tick after it
            does nothing until somebody edits. */
        std::uint64_t routingRevision = 0;

        /** The show revision `applyEq` last pushed at; `routingRevision`'s twin. */
        std::uint64_t eqRevision = 0;

        /** The show revision `applyLanes` last read the lanes at; the same twin. */
        std::uint64_t laneRevision = 0;

        /** The show revision `applyRates` last read the speeds at; the same twin. */
        std::uint64_t rateRevision = 0;

        /*  THE PASS'S OWN BOOKS (§20.9, §34): the table `lane.*` moves; for
            each lane of the flipped cue, the ride so far - a segment per
            stretch between a loop's wraps or a punch-in's touches - the lane as
            the show has it, which the ride is spliced into, the send it is on
            and the number it is an offset on; the cue and show revision those
            were read at; and the run whose pass has been written, so the tick
            between the write and its `lane.stop` landing does not write it
            twice. */
        struct LaneBook
        {
            std::vector<RideSegment> ride;
            std::vector<doc::LanePoint> lane;

            /*  The send the lane is on, empty for the level's and for a mix the
                cue does not send to - whose lane is silence (UQ). */
            std::string sendId;
            bool sends = false;

            /*  What the lane is an offset on: the cue's written level, or the
                send's; nought for a mix with no send, which is the level the
                send it is given is made at. */
            double written = 0.0;

            /*  Latched, last tick: a touch that has just begun starts a new
                segment, so a punch-in after a punch-out is not joined to it by
                a straight line over the stretch between (UP). */
            bool wasTouched = false;
        };

        LaneTable* lanes = nullptr;
        CurveTable* curveTable = nullptr;

        /*  The run of the curve pass whose end has been submitted, waiting for
            the `curve.stop` that says so - an end said once. */
        std::string curvePassEnded;
        std::map<std::string, LaneBook> laneBooks;
        std::string rideLaneCue;
        std::uint64_t rideLaneRevision = 0;
        std::string rideWritten;

        /*  THE MIXES A HAND HOLDS in the pass's run (§34): `applyLanes` leaves
            their offsets alone, so the hand's stays the one heard and the
            routing is not rebuilt every tick by a lane and a hand taking turns. */
        std::set<std::string> heldBuses;

        /*  And the live layer's, beside each: a turn under the lock moves the
            layer and not the show. */
        std::uint64_t routingLiveRevision = 0;
        std::uint64_t eqLiveRevision = 0;
        std::uint64_t fxRevision = 0;

        /*  And the plugin table's (2026-09-26): a plugin coming up says what
            it takes, which can change how wide a sounding cue is. */
        std::uint64_t routingPluginRevision = 0;
        std::uint64_t fxPluginRevision = 0;
        std::uint64_t fxLiveRevision = 0;

        /*  AND WHAT FADES MOVED (namespace draft §26): bumped on every tick a
            fade writes a run's send, EQ number or plugin value, so a voice
            follows its fade at the tick rate and nothing is read while no such
            fade is under way. One counter for the three, with one seen-value
            each. */
        std::uint64_t movedRevision = 0;
        std::uint64_t routingMovedRevision = 0;

        /*  AND WHAT THE SENDS' LANES ASK FOR (namespace draft §28): bumped
            when a sounding run's send lanes move by more than a hundredth of a
            decibel, so the matrix follows a lane at the tick rate and costs
            nothing where every lane is flat. */
        std::uint64_t sendLaneRevision = 0;
        std::uint64_t routingSendLaneRevision = 0;
        std::uint64_t eqMovedRevision = 0;
        std::uint64_t fxMovedRevision = 0;

        /*  THE MACHINE'S PLUGIN CATALOGUES, for where a fade on a parameter
            the cue never set begins: the parameter's default. Null when none
            is given; such a fade then starts where it ends. */
        const plugin::CatalogueStore* catalogues = nullptr;

        /*  THE PERSISTENT ASSERTION (§3.29, §13.11): after every applied
            trigger, what the section declares is checked against what is
            actually happening, and `run.assert` puts back what is not.

            AFTER A TRIGGER AND NOT EVERY TICK, which is the PRD's own reason:
            "a tick-rate check makes a stop impossible, a trigger-rate check is
            human-paced". An operator who kills a bed has until their next press
            to decide something else, and the kill is remembered anyway.

            AND AFTER THE SWEEP HAS ANSWERED, when it can: the same step asks
            each askable desk what it holds (§13.10), and an assertion that ran
            before the answers arrived would compare the plan against last
            second's world. It waits for the observations the step asked for,
            and gives up waiting after half a second - a desk that has gone
            quiet must not stop the section asserting. */
        void assertPersistent (Engine& engine, std::int64_t tick);

        std::uint64_t assertedFor = 0;
        std::int64_t assertDue = -1;

        /*  The tick of the last double Esc applied, or -1: `killedInDrain`. */
        std::int64_t killedAtTick = -1;

        /*  And of the last Esc, or -1: `escapedInDrain` (K8). */
        std::int64_t escapedAtTick = -1;

        /*  THE BEDS ESC PAUSED (K8, namespace draft §23.17), by cue: the run it
            took down and where it carries on from - `resumes` false for a mic
            run, and for a media run never heard, which starts from its top.
            Written by Esc's handler (`pausePersistent`); dropped by the fire
            that makes the cue's next run - the resume itself, or anybody's -
            by a double Esc (`dropOutputs`) and by a load-to-time, which
            re-solves the world. Handler state: a replay keeps the same, and
            never reads it, because `run.assert` carries the resume point. */
        struct PausedBed
        {
            std::string run;
            bool resumes = false;
            ResumePoint at;

            /*  The file it played (K8's review): a cue given another file while
                it was paused starts the new one from its top. */
            std::string media;
        };

        std::map<std::string, PausedBed> paused;

        /*  WHERE EACH PAUSED BED'S PLAYHEAD WAS AT THE PRESS (the author,
            2026-10-02, K8's review), by the run Esc took down: the readout the
            press's own tick made off the sample clock - before the drain that
            applied the press, and so before the panic fade had moved it on -
            read by `notePausedPlayheads` on the next tick, before the readout
            moves again. Exact through a speed fade, an edit of the speed, a
            speed held at nought, a stretched cue and the launch latency, none
            of which the document knows. HOOK STATE: no handler reads it; the
            second rides the logged `run.assert`, so a replay arms what the
            session armed. Pruned when the pause it belongs to has gone. */
        std::map<std::string, ResumePoint> playheads;

        /*  The hook that reads them. */
        void notePausedPlayheads();

        /*  THE DE-CLICK (K8's review; namespace draft §24, GQ): every run made
            to arrive over one, whose launch has now been placed, is given a
            straight fade from silence to its level over `deClickTicks` - a
            level job that reports nothing, stops nothing and submits nothing,
            so a replay, which runs no hook, is unchanged. Built once, for the
            paused bed here and for Doh!'s resume. */
        static constexpr int deClickTicks = 5;
        void deClickLaunched();

        /*  BEDS A PASS FOUND STILL FADING UNDER THE ESC THAT PAUSED THEM (K8):
            a fire of a cue whose run is on its way out is ignored (decision N),
            so the pass owes them, and `assertPersistent` puts each back on the
            tick its old run has ended. Hook state, emptied by Esc, a double
            Esc and a load-to-time. */
        std::set<std::string> owed;

        /*  The resume point `assertCue` hands the run it is about to make. */
        std::optional<ResumePoint> resumeNext;

        /*  The hook's half of putting a persistent cue back: `run.assert`,
            carrying where a paused bed carries on from - unless the log knows
            its file is all but over there, when it starts from the top. */
        void submitAssert (Engine& engine, const std::string& cueId);

        /*  Whether a run of the cue was killed since the last load-to-time:
            decision S's suspension, read off the run table. */
        bool killedSinceLift (const std::string& cueId) const;

        /** Whether the cue sits in a list's persistent section. */
        bool inPersistentSection (const std::string& cueId) const;

        /** A media cue's speed as its document says, nought to twenty. */
        double documentSpeedOf (const juce::ValueTree& cue) const;

        /*  Cues the operator killed, which stay killed for the session (decision
            S). Cleared by a load-to-time, which is a new answer to the same
            question and is the operator asking again. */
        std::set<std::string> suspended;

        /*  When the last load-to-time lifted the suspensions. A kill BEFORE it
            is a kill the operator has already asked to undo; the run table
            keeps every run for ever, so without this the same killed run would
            re-suspend the cue on the next step and the lift would do nothing. */
        std::int64_t liftedAt = -1;

        /*  Fades taken over by another fade since the last tick, whose runs
            have still to be ended. A queue rather than a submission at the
            takeover, because only the tick hook reports - see advanceFades. */
        std::vector<std::string> supersededRuns;

        //======================================================================
        /*  DOH!'S BOOKS (namespace draft §24). Handler state, every field -
            written by the `go`, `go.doh`, `cue.fire`, `trigger.fire`,
            `list.loadToTime` and Esc handlers - except where it says hook. */

        /*  A PREPARATION A GO ADOPTED, AS IT WAS BEFORE (2026-10-02, D2, namespace draft §24.12):
            taken in the GO's own handler at each door that adopts - a prepared
            block, an armed arm, a member the horizon armed under a running
            act - so a Doh of a GO nobody heard can hand it back exactly: the
            same runs, in the same states, under the same parents, their jobs
            as they were. `parentGroup` is the running act whose job takes it,
            or empty. */
        struct RunBefore
        {
            std::string id, state, prepare, parent, enterAt;
            bool launchRequested = false;
            std::int64_t launchRequestedAtTick = 0, dueTick = 0;
            std::vector<std::string> round;
            int iteration = 0;
            std::int32_t seed = 0;
            std::int64_t roundStartedAtTick = 0, firstRoundAtTick = 0;
        };

        struct Adoption
        {
            std::string root, parentGroup;
            std::vector<RunBefore> runs;
            std::vector<GroupJob> jobs;
        };

        /*  The snapshot, taken before the adoption changes anything - and only
            for the GO the record is open for, once per block. */
        void snapshotAdoption (const std::string& runId, const std::string& parentGroup);

        /*  THE LAST GO, as the Doh would take it back. */
        struct GoRecord
        {
            std::uint64_t serial = 0;           ///< nought: nothing to take back
            std::int64_t tick = -1;
            std::string list, cue;              ///< the list it moved, the standby it fired
            bool finishedBefore = false;
            std::int64_t lastGoTickBefore = -1;
            bool escapedAfter = false;          ///< Esc or double Esc since
            bool firedAfter = false;            ///< a fire, a trigger or a pad of its bank since
            std::vector<std::string> made;
            std::vector<std::string> touched;   ///< older manual groups it reached (§24, HE)
            std::optional<DohMark> markBefore;  ///< the list's mark as this GO found it

            /*  The entries of the mark this GO filed - marked cue, cues - so
                `endGo` can undo a filing no run of the marked cue took. */
            std::map<std::string, std::vector<std::string>> filed;

            /*  What it adopted, as it was (D2). */
            std::vector<Adoption> adoptions;

            /*  What its enable and disable cues switched, each cue's mark as
                this GO found it - `file`, `on` or `off` - the first switch of a
                cue only, so a Doh! puts back the overrides as they were before the GO
                (namespace draft §27, PS). */
            std::vector<std::pair<std::string, std::string>> overridesBefore;
        };

        /*  The cues a corrected GO sends nothing of, by its serial. */
        struct LeftSends
        {
            std::string list;
            std::set<std::string> cues;
        };

        /*  ONE RUN MADE, AND STAMPED: which GO it is, whether it is the
            horizon's, whether an older act made it because of the GO, and
            whether it sends nothing a device's operator was left with. Every
            road that makes a run in a handler comes through here. */
        void createRun (const std::string& id, const std::string& cueId, const std::string& kind,
                        const std::string& parentRun);

        /*  A RUN A GO ADOPTS, WITH EVERYTHING UNDER IT: a prepared block, an
            arm it launches, a member it asks for, a run taken under a tagged
            group. */
        void stampSubtree (const std::string& runId, std::uint64_t serial);

        /*  The cue's left entry, consumed by this run once. */
        void claimSendsLeft (Run& run);

        /*  A press that did something on a strip: a pad of the bank the last
            GO armed is that GO being played (§24, GI). */
        void notePlayed (const Run& member);

        /*  Whether a run was ever launched: launch evidence, read from fields
            handlers and records write - never from a hook's. */
        static bool hasLaunchEvidence (const Run& run) noexcept;

        /*  The newest unfinished run of a cue that is neither a preparation nor
            taken back: what decision N asks about, so a GO on a cue whose old
            voice a Doh is fading out starts it again. */
        const Run* liveUntakenRunOf (const std::string& cueId) const;

        /*  Whether anything under a GO's root was heard (§24's one predicate):
            a media or mic run of that GO with `run.started`, or a MIDI run of it
            launched to a port that plays sound. */
        bool heardUnder (const std::string& rootId, std::uint64_t serial) const;

        /*  Whether a group the GO reached is one an older act's reaction to the
            GO comes from (§24, HE). */
        bool isReached (const Run& group) const;
        bool isFooterCueOf (const Run& group, const std::string& cueId) const;

        /*  A run ended here and now, the way the jump's sweep ends one: no
            footer, no post-wait, its voice stopped, its slots and its jobs let
            go. */
        void endHere (const std::string& runId, std::int64_t tick);

        /*  What counts as having left for a device (§24, HQ). */
        bool countsAsSent (const Run& run) const;

        /*  The GO debounce, read for two Doh presses; and the window. */
        bool dohTooSoon (std::int64_t tick) const;
        bool dohTooLate (std::int64_t tick) const;

        /*  The list's row order, read from the document: whether a GO on
            `fired` is past `marked`, and whether a GO on `standby` reaches it. */
        bool isInside (const std::string& cueId, const std::string& ancestorId) const;
        bool reaches (const std::string& standby, const std::string& marked) const;
        bool isPast (const std::string& listId, const std::string& fired, const std::string& marked) const;

        std::uint64_t goCount = 0, currentGo = 0;
        GoRecord goRecord;
        std::set<std::uint64_t> takenBackSerials;
        std::int64_t lastDohTick = -1;
        std::string lastDohList;
        std::map<std::string, DohMark> marks;
        std::map<std::uint64_t, LeftSends> leftByGo;

        /*  THE DOH'S OWN PERSISTENT PASS (§24, HN): the step count its `d`
            opened, and the persistent OSC and MIDI cues of devices left to
            their operators, which that pass does not re-assert - nor anything
            at all after an Esc between the GO and the Doh, which a Doh never
            undoes (`persistentAllLeft`). Written by the handler, read by the
            hook. */
        std::uint64_t persistentLeftAt = 0;
        std::set<std::string> persistentLeft;
        bool persistentAllLeft = false;

        /*  HOOK MEMORY: a taken-back run whose voice the standby's preparation
            waits for before it arms the cue again. */
        std::string waitingForVoice;

        /*  THE MARKS, CHANGED THROUGH THESE AND NOTHING ELSE (GN, namespace draft §24.12): the resume
            dropped - its arm at the point revoked, but for the run that spent
            it - and the standby asked to make the cue ready again; a whole mark
            dropped, what was left with devices' operators with it; a mark set,
            the one it replaces dropped first. */
        void dropRoot (Engine& engine, std::int64_t tick, const std::string& listId,
                       const std::string& carriedOn = {});
        void dropMark (Engine& engine, std::int64_t tick, const std::string& listId);
        void setMark (Engine& engine, std::int64_t tick, const std::string& listId, DohMark mark);

        /*  The `list/resume` readout, from the list's mark. */
        void publishResume (const std::string& listId);

        /*  A resume arm of a root: unfinished, `resumes`, no launch evidence,
            under the root's act, or at the top for a root at the top. */
        bool isResumeArm (const Run& run, const DohRoot& root) const;

        /*  The root of a mark on any list that names this cue as a sound to
            carry on, for the arm that makes it ready at its point. */
        const DohRoot* soundRootOf (const std::string& cueId) const;

        /*  The resume fields a run made ready at a root's point is given
            before its arm: no pre-wait, the de-click, the level, the point. */
        void armAtRoot (Run& run, const DohRoot& root, const juce::ValueTree& cue) const;

        /*  WHETHER A PAUSED SOUND STILL HAS SOMETHING TO PLAY AT `at` seconds
            of its file (GM): a slice always does; else, by the length the
            log knows, until half a second of the clock from its end. */
        bool notOver (const juce::ValueTree& cue, int range, double at) const;

        /*  Where the paused run's file had got to, as the handler can count
            it: from its arm's origin at the cue's own speed since it was heard
            (K8's fallback, `armedOrigin`), or the slice it was in. */
        ResumePoint countedPoint (const Run& run, std::int64_t tick) const;

        /*  The root a heard GO leaves for the next GO on its cue, built from
            handler state at the press (namespace draft §24.12); nothing when it cannot be
            carried on - the next GO starts it from its top. */
        std::optional<DohRoot> rootFor (const std::string& rootId, std::uint64_t serial, std::int64_t tick,
                                        const std::vector<std::string>& leftCues, int fadeTicks) const;

        /*  THE UN-ADOPT (namespace draft §24.12): a preparation the GO adopted and nobody heard,
            handed back as it was. */
        void unadopt (Engine& engine, std::int64_t tick, const Adoption& adoption, std::uint64_t serial);

        /*  A run seated or brought back under a running act joins the act's
            job as a member it has launched (namespace draft §24.12). */
        void adoptIntoParentJob (const std::string& groupRun, const std::string& runId);

        const std::map<std::string, double>* handlerDurations() const noexcept
        {
            return haveLoggedDurations ? &loggedDurations : durations;
        }

        std::map<std::string, double> loggedDurations;
        bool haveLoggedDurations = false;

        /*  HANDLER-WRITTEN, HOOK-READ: the runs a Doh paused whose playhead the
            hook reports on the next tick (`go.dohPlayhead`); and that hook. */
        std::vector<std::string> playheadsOwed;
        void noteDohPlayheads (Engine& engine);

        /*  Set for the length of `prepareStandby`: what is made then is the
            horizon's, nobody's GO. */
        bool makingForHorizon = false;

        //======================================================================
        /*  WHAT A GO CHANGED ELSEWHERE, AND HOW DOH! PUTS IT BACK (2026-10-03,
            D3, PRD §3.32, namespace draft §24.13).

            Captured while the GO's record is open, at the moment each change is
            made, keyed on the GO a run answers to (`goOfRun`: what it caused
            before what made it, so an act's footer the GO set off is captured
            with the GO's own). Two kinds, and the replay rule keeps them apart:
            what changes a run, an identifier or the history is HANDLER-EXACT -
            the stops, the flags, the banks it closed, the takes - and what only
            shapes a job, a level or a write a hook makes is HOOK-CONSUMED - the
            desk's values, the levels. The desk is decided in a hook, on the tick
            after the press (`flushDohWrites`); the rest in `go.doh`'s handler. */

        /*  A DESK ADDRESS THE GO WROTE: what it held before the GO's first
            write, what the last one put there as the tree took it, the desk's
            first answer after that write (its echo, which a desk that quantises
            makes different), the cue that wrote it last and its device - and,
            once the Doh has read that cue's setting, whether it is left to its
            operator. Hook-consumed: read by the flush, never by a handler's
            decision. */
        struct DeskBefore
        {
            std::string address, device;
            std::optional<osc::Values> before;
            osc::Values lastWritten;
            std::optional<osc::Values> echo;
            std::string lastWriter;
            bool leftToOperator = false;
        };

        /*  A LEVEL, A SPEED OR A DCA TRIM A FADE OF THE GO MOVED on something
            the GO did not start: the job's key, the run it holds (none for a
            DCA), where it stood before the GO's first fade on it, where the
            GO's last fade on it was actually going (a mic's stop leaves its
            level where it is), the command tick, and the fade that was moving
            it before the GO, if one was. Hook-consumed. */
        struct LevelTouch
        {
            std::string key, heldRun, dca;
            bool movesRate = false;

            /*  Or one of the run's own numbers a fade moved (namespace draft
                §26, PE): its entry, and the domain it travels in. */
            std::string move;
            MoveDomain moveDomain = MoveDomain::linear;
            double from = 0.0, goTo = 0.0;
            std::int64_t atTick = 0;
            std::optional<FadeJob> superseded;
        };

        /*  A STOP THE GO ISSUED on a run not its own: when it lands, counted
            from the command's tick without the launch latency a replay does not
            have; whether a stop was asked of the target before it (by
            `stopAsked`, never `state`); whether it was in its pre-wait and when
            that would have run out; its level. Handler-exact - `jobStopsAt` and
            `levelBefore` excepted, which only shape a job. */
        struct GoStop
        {
            std::string target, source, targetKind;
            std::int64_t goTick = 0, landsAt = 0, jobStopsAt = 0;
            bool wasStopping = false, wasWaiting = false;
            std::int64_t dueTick = 0;
            double levelBefore = 0.0;

            /*  In its post-wait when the stop reached it, its sound over; and
                whether the stop is a fade - a scene's job waits for one before
                it touches a member (K3), so it can still be called off (D3's
                review). Handler state, both. */
            bool wasPostWait = false;
            bool fades = false;
        };

        /*  An advance or a boundary stop the GO asked of a run, as it was. */
        struct FlagBefore
        {
            std::string run;
            bool advanceRequested = false;
            int advanceTo = -1;
            std::string stopAfter;
        };

        /*  A PRESS THE GO MADE ON A SAMPLING CHANNEL'S TAKE: the account before
            and after it, which is all an inverse needs and all that says
            whether the take is still where the GO left it. */
        struct TakeBefore
        {
            std::string channel;
            TakeVerb verb = TakeVerb::record;
            std::string stateBefore, stateAfter;
            int layersBefore = 0, layersAfter = 0;
        };

        /*  THE RECORD'S D3 HALF, with the record's lifetime: opened by
            `beginGo`, spent by the Doh, forgotten by a jump on its list. */
        struct GoChanges
        {
            std::vector<DeskBefore> desk;
            bool deskOverflow = false;
            std::vector<LevelTouch> levels;
            std::vector<GoStop> stops;
            std::vector<FlagBefore> flags;
            std::vector<std::string> closedSamplers;
            std::vector<std::pair<std::string, std::string>> lostStrips;
            std::vector<TakeBefore> takes;

            /*  What it sent that nothing can read back - an OSC event, a write
                to an opaque device: (cue, device), once each. */
            std::vector<std::pair<std::string, std::string>> unputtable;
        };

        GoChanges goChanges;

        /*  Bounded, belt and braces: a scene that runs long after its GO keeps
            capturing until the next GO, and a Doh that late is refused. */
        static constexpr std::size_t changesKept = 256;

        /*  Whether a run answers to the GO the record is open for. */
        bool ofTheOpenGo (const std::string& runId) const;

        /*  THE CAPTURES, each where its change is made: a level, a speed or a
            trim a fade of the GO moved; a stop the GO issued, and the act its
            target sits in reached; a hard stop a transport cue fell back to; an
            advance or a boundary stop asked; a press on a take. */
        void noteLevelTouch (const std::string& key, const std::string& heldRun, const std::string& dca,
                             bool movesRate, double from, double goTo, std::int64_t tick,
                             const std::optional<FadeJob>& superseded);
        void noteGoStop (const GoStop& stop, const std::string& targetParent);
        void noteHardStop (const Run& target, const std::string& source, std::int64_t tick);
        void noteFlag (const Run& target, const std::string& source);
        void noteTake (const std::string& source, const std::string& channel, TakeVerb verb, const Take& before);

        /*  THE DESK'S HALF OF `writeOscNow`: before a write, the echo of the
            GO's last write to the address kept (the write is about to forget
            it) and, for a write of the GO's, what the address held; after it,
            what the GO wrote. */
        void deskBeforeWrite (const OscJob& job, const std::string& address,
                              std::optional<osc::Values>& held, bool& captured);
        void deskAfterWrite (const OscJob& job, const std::string& address,
                             const std::optional<osc::Values>& held,
                             const std::string& mountId, const osc::Values& values);

        /*  A SCENE THE GO STOPPED THAT HAD NOT ENDED AT THE DOH, put back once
            it has (§24.13): handler state - the hook only watches for the end,
            and `go.dohRelaunch` carries the decision. `left` is its OSC and
            MIDI cues left to their operators, read at the Doh. */
        struct PendingRelaunch
        {
            std::string list, groupRun;
            std::int64_t goTick = 0, dohTick = 0;
            double levelBefore = 0.0;
            std::set<std::string> left;
        };

        std::vector<PendingRelaunch> pendingRelaunches;

        /*  HOOK MEMORY: the scenes whose `go.dohRelaunch` has been submitted. */
        std::set<std::string> relaunchAsked;

        /*  WHAT A HANDLER LEAVES FOR THE FLUSH TO SEND (§24.1: a handler never
            submits): the Doh's desk entries, the values a jump or a relaunch
            wants on the desk, and the report's sentences. Stamped with the tick
            it was filled on - a fill finding an older stamp clears it first, so
            a replay, which never flushes, does not grow it. Hook-consumed. */
        struct DohStash
        {
            std::int64_t tick = -1;
            std::string list;
            std::vector<DeskBefore> desk;
            std::vector<std::pair<std::string, osc::Values>> values;
            std::vector<std::string> items;
            bool report = false;

            /*  THE ROLLBACKS A DOH DECIDED (2026-10-03, OV-OX): one per device,
                its kind ("osc" or "midi"), the device and the line - sent by
                the flush before the desk's put-back. */
            struct Rollback
            {
                std::string kind, device, text;
            };

            std::vector<Rollback> rollbacks;

            /*  Filled by a Doh's own press, not only by a relaunch: its report
                REPLACES the readout, an empty one clearing it; a relaunch's alone
                is APPENDED to what the readout holds (D4's review, OJ, OK). */
            bool fromDoh = false;
        };

        DohStash stash;
        DohStash& stashFor (std::int64_t tick);

        /*  HOOK MEMORY: what this tick's give-backs put back on the desk, by
            address - filled by `submitRestores`, whichever hook called it, read
            by the flush, which runs after them. */
        std::map<std::string, osc::Values> restoredThisTick;

        /*  THE HOOK THAT SENDS IT (§24.13), after `armStandby` in `beforeTick`. */
        void flushDohWrites (Engine& engine);

        /*  Set while a put-back makes runs: nobody's GO (GY's rule). */
        bool makingForPutBack = false;

        /*  WHAT ONE PUT-BACK HAS IN HAND: the press's tick, the GO's serial and
            list, the panic fade it fades over, the identifiers it may draw, the
            report's sentences so far, and the GO's runs. */
        struct PutBack
        {
            std::int64_t tick = 0;
            std::uint64_t serial = 0;
            std::string list;
            int fadeTicks = 0;
            double fadeSeconds = 0.0;
            std::function<std::string()> nextId;
            std::vector<std::string> items;
            std::set<std::string> goRuns;
        };

        void putBackLevels (PutBack& back, const GoChanges& changes);
        void putBackStops (Engine& engine, PutBack& back, const GoChanges& changes);
        void putBackFlagsBanksTakes (Engine& engine, PutBack& back, const GoChanges& changes);

        /*  A CUE THE GO STOPPED, MADE AGAIN WHERE IT WOULD BE NOW: a sound at
            the second it would have reached, fading in over the panic fade; a
            cue it stopped in its pre-wait with the rest of that wait; a timed
            scene at its second, its values re-sent where its cues take back.
            Answers whether anything was made. */
        bool relaunchStopped (Engine& engine, PutBack& back, const GoStop& stop,
                              const std::optional<FadeJob>& before);
        bool relaunchScene (Engine& engine, PutBack& back, const std::string& groupRun,
                            const std::set<std::string>& left, double levelBefore);

        /*  The newest live run of a cue other than `except`, as decision N
            would see it: what a relaunch would play beside. */
        const Run* liveOtherRunOf (const std::string& cueId, const std::string& except) const;

        /*  The OSC and MIDI cues under a scene whose setting is leave. */
        std::set<std::string> leftCuesUnder (const std::string& sceneCue) const;

        /*  A cue's name as the operator reads it, and a device's. */
        std::string cueLabel (const std::string& cueId) const;
        std::string deviceLabel (const std::string& kind, const std::string& deviceId) const;
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
