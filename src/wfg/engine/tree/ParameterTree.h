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
    The `/godot` namespace, built from the parameter table and published once
    per tick.

    IT IS A PROJECTION. Nothing here owns a value. A node under `/godot/cue`
    reads an attribute of show.xml, a node under `/godot/engine` reads a counter
    the tick thread keeps, and a node under `/godot/cmd` describes a command the
    registry already holds. Writing to one is the `node.set` command, which goes
    through the document's single write path like every other mutation.

    OBJECTS ARE ADDRESSED BY IDENTITY, never by position: a cue lives at
    `/godot/cue/<id>` whatever list contains it and wherever it sits in the
    order. So a client's subscription survives a reorder, and the address an
    operator reads off the screen is the address a Choufleur pointer resolves
    (PRD §3.23). Order is a separate read-only node on the container.

    DERIVED VALUES ARE COMPUTED HERE, not stored. A cue's `kind`, `parent`,
    `index` and a container's `order` are `persist=none` in the table: the
    structure already says them, so storing them would be a second copy that
    eventually disagrees with the first. The document drops those rows
    entirely; this is what puts them back on the wire.

    WHAT IS REBUILT AND WHEN. The document side is the big one and changes only
    when someone edits the show, so it is built once, shared between snapshots
    by pointer, and thrown away when markStale() says the show moved. The
    engine's own counters are a dozen nodes that change every tick and are
    rebuilt every tick. Phase 1 rebuilds the document side on ANY applied
    mutation rather than working out which subtree was affected - the fixtures
    are small, and the property that has to be right now is that a published
    snapshot never changes afterwards, not that publishing is cheap. When there
    is a show big enough to measure, measure it.
*/

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/ListState.h>
#include <wfg/engine/cue/SlotAnalysis.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/*  Named and not included: the tree reads a MediaInfo only through a pointer
    here, and the .cpp is where it is dereferenced. */
namespace wfg::audio { class MediaInfo; }

namespace wfg::midi { class PortTable; }

namespace wfg::surface { class SurfaceTable; }

namespace wfg::tree
{
    /*  The engine's own numbers, handed to the tree each tick because the tree
        has no way to ask for them: Engine.h is deliberately vendor-free and
        knows nothing about a parameter tree, and the clock lives on a thread of
        its own. Whoever wires the three together fills this in. */
    struct EngineState
    {
        std::string product = "Go.dot";
        std::string version;

        std::int64_t tick = 0;
        int sampleRate = 0;
        int blockSize = 0;
        int samplesPerTick = 0;
        std::int64_t lateness = 0;
        std::int64_t latenessMax = 0;

        /** `dummy` in Phase 1, `hosted` or `device` from Phase 2. */
        std::string clock = "dummy";

        //======================================================================
        /*  What the audio actually is right now, as against what the document
            decided (PRD §4.10). `/godot/audio/tracks` and every bus node under
            `/godot/bus` are the document's; these three are the machine's, and
            they read
            their table defaults - no device, no outputs, stopped - until
            something opens one. Saying "stopped" with nothing open is the
            truthful answer rather than a placeholder. */
        std::string audioDevice;
        int audioOutputs = 0;
        std::string audioStatus = "stopped";
        std::string audioSettingsStatus = "ready", audioSettingsError;
        std::string audioAvailableBufferSizes;
        audio::OutputTestSettings audioTest;
        int audioSampleRate = 0, audioBufferSize = 0, hardwareInputs = 0, hardwareOutputs = 0;
        int audioSettingsRevision = 0;

        std::uint64_t errorCount = 0;
        std::string lastError;

        /*  The lipogram, as a number a client can read (PRD §4.2). Violations
            is Go.dot's own code allocating on the audio thread and must be
            zero; the foreign count is Tracktion's, published beside it rather
            than folded in, because our rule cannot honestly cover code we did
            not write. Both read zero in a build with the checks compiled out -
            which is why `wfg_tests` asks whether anybody was looking before it
            believes a zero. */
        /*  How many ticks pass between a GO being applied and the sound
            starting. Not a preference: the smallest delay that guarantees a
            launch is never placed in the past, which would make the cue late
            AND put a hole in it. */
        int launchLatencyTicks = 0;

        std::uint64_t rtViolations = 0;
        std::uint64_t rtForeignAllocations = 0;

        //======================================================================
        // Which bundle is open. Runtime, not document: PRD §4.10.
        std::string documentPath;
        std::string documentName;
        bool documentDirty = false;

        /*  THE UNDO HISTORY, READ IN THE AFTER-TICK like every other readout
            here, and never subscribed to.

            juce::UndoManager is a ChangeBroadcaster whose every mutation sends a
            change message, inert only while nobody has added a listener. A
            desktop client attaching one to grey out its Undo menu item would put
            a post to the message manager on the tick thread once per applied
            edit - so these four are read, not pushed, and namespace draft §14.9
            makes that a rule rather than an accident. */
        bool documentCanUndo = false;
        bool documentCanRedo = false;
        std::string documentUndoName;
        std::string documentRedoName;

        /*  THE WHOLE STACK, as names (2026-09-19): what Undo would unmake
            newest first, and what Redo would put back nearest first, each
            space-separated - a transaction is named after its command, and
            no command has a space in it. Read from the manager in the
            after-tick like the four above, for the same reason. */
        std::string documentUndoHistory;
        std::string documentRedoHistory;

        /*  THE LAST THING `document.copy` COPIED, as the fragment text a paste
            takes. A client puts it on the operating system's clipboard when it
            changes, which is how two windows that are two processes hand cues
            to each other; the page pastes it straight back. Empty until a
            copy. */
        std::string documentClipboard;

        /*  HOW MANY TIMES THE SHOW HALF HAS CHANGED since the bundle was
            opened - `ShowDocument::showRevision()`, copied here each tick so
            the tree publishes it beside the dot that is derived from the same
            counter. A GO, a standby move, a focus change and every run leave
            it alone; an edit, an undo, a revert and a recovery move it. Its
            first reader is the desktop client's cue list, which rebuilds its
            rows when this moves and not otherwise: the document half of a
            snapshot is rebuilt whenever ANY command applies (`markStale`), so
            a chain of runs would otherwise have a 500-row model rebuilt
            several times a second for a show nobody edited. Never zero once
            published - the counter starts at 1 - so a client can keep 0 for
            "no picture built yet". */
        std::uint64_t documentRevision = 0;

        /*  WHETHER A PREVIOUS SESSION LEFT WORK BEHIND: a `recovery/show.xml`,
            or failing that a `recovery.previous.N/`, found when this bundle was
            opened, and neither adopted nor discarded since (namespace draft
            §14.10).

            It is a record carried on the session and never a look at the disk,
            and the reason is this session's own autosave: `recovery/` appears
            two seconds after the first edit of any show, so a node that asked
            the filesystem would offer every unsaved show back to the operator
            who was in the middle of writing it.

            Read here from `doc::DocumentSession` for the same reason the dot
            beside it is read here - before the publish, or a client draws the
            offer one tick after the engine printed it. */
        bool documentRecovery = false;

        /*  WHAT THE WRITER THREAD LAST FAILED TO DO, in the writer's own
            sentence, and empty when nothing is outstanding (PR 5.5, second
            half). Not `lastError`, which quotes a rejected record: a write
            that fails on the writer belongs to a command that was applied.
            `doc::DocumentSession::writeError` says when it goes out. */
        std::string documentWriteError;

        /*  HOW MANY DATAGRAMS THE SENDER GATE HAS DROPPED since the show
            opened, and nought when strict senders is off - which is not the
            same as nought when it is on, and is why this is published rather
            than inferred. A surface that has gone quiet is the case it is for:
            a number that climbs as somebody presses a button says the message
            is arriving and being refused. Counted on the socket thread and
            read here once a tick, like everything else in this struct. */
        std::uint64_t refusedDatagrams = 0;
    };

    class ParameterTree
    {
    public:
        /*  None of the four references may outlive the tree.

            The mounts are the part of the namespace Go.dot did not write: they
            arrive at their own prefixes rather than under /godot, because
            /godot/mount holds the DECLARATION and /wfs holds the target.

            The runs are the only one read on the RUNTIME side. Mounts and the
            document are rebuilt when the show changes; a run changes several
            times a second while nothing about the show does, so publishing it
            from the cached half would have frozen every run at whatever it read
            the last time somebody edited a cue. */
        ParameterTree (const doc::ShowDocument& documentToProject,
                       const CommandRegistry& commandsToDescribe,
                       const MountTable& mountsToPublish,
                       const cue::RunTable& runsToPublish);

        /*  Tick thread only. Rebuilds the document side if it has been marked
            stale, then publishes an immutable snapshot and returns it. */
        std::shared_ptr<const TreeSnapshot> publish (std::int64_t tick, const EngineState& state);

        /*  The sender whose counts `mount/<id>/sent` publishes, if there is
            one. Absent - `wfg tree`, a replay, any test - every mount reads
            zero, which is the truth: nothing was sent because there was
            nowhere to send it. */
        void setSender (const MountSender* senderToRead) noexcept { sender = senderToRead; }

        /*  What each declared MIDI port turned out to be plugged into, and
            what this machine has to plug one into. The same shape as the
            sender above and absent for the same kind of reason: a replay or a
            tree dump has opened no device, so every port reads unbound, which
            is the truth rather than a placeholder. */
        void setMidiPorts (const midi::PortTable* portsToRead) noexcept { ports = portsToRead; }

        /*  What each declared control surface turned out to be - talked to or
            not, and why - for `/godot/surface/<id>/connected`, `problem` and
            `serial`. The port table's shape and its reason: a replay or a tree
            dump drives no surface, so every hardware surface reads not
            connected, which is the truth. A virtual one reads connected
            whatever this says, being the client's own panel. */
        void setSurfaces (const surface::SurfaceTable* surfacesToRead) noexcept
        {
            surfaces = surfacesToRead;
        }

        /*  How long each media file is, read once when the show was opened, for
            `/godot/cue/<id>/duration`. Keyed by the `file` the document names.

            The same shape as the sender above, and absent for the same kind of
            reason: a caller that never read any media - a replay, a test, a
            tree dump of a bundle with no media folder - leaves every duration
            at nought, which is what nought means anyway (`audio/MediaInfo.h`).

            Held by pointer and not owned; the map must outlive the tree. Marks
            the document half stale, because that is the half a cue lives in. */
        /*  THE LENGTHS ARE ASKED FOR EACH PUBLISH and not held by address
            (2026-09-22). They used to be frozen at open, so an address was
            safe and a file imported since read a length of nought for the rest
            of the session; now one is LEARNED when the analyser reads it, the
            map is swapped, and a holder of the old address would go on
            publishing the old answer. `publish` takes the pointer for the
            length of a pass and lets it go.

            Kept as a setter so a test can hand in a map of its own without a
            `MediaInfo` at all. */
        void setMediaDurations (const std::map<std::string, double>* durationsToPublish) noexcept
        {
            fixedDurations = durationsToPublish;
            stale = true;
        }

        /*  WHAT THE ANALYSER HAS LEARNT ABOUT EACH FILE, for
            `/godot/cue/<id>/hash` and `/godot/run/<id>/timbre` (namespace draft
            §14.5): the late half of `audio::MediaInfo`, the hash and the
            pyramid, read through ONE `snapshot()` per publish - a pointer copy
            under that object's own short lock, however many runs are playing.

            BESIDE `setMediaDurations` AND NOT INSTEAD OF IT: the slot analysis
            reads the durations by ADDRESS, so they stay a pointer of their own
            into the frozen half (`audio/MediaInfo.h`). Absent - a tree built
            without this, as a test builds one - every hash and every timbre
            reads empty, the answer §3.30 gives for a clip whose colours have
            not arrived. (`wfg replay` builds no tree at all: its runner is
            handed the lengths its log recorded, and nothing it does is
            published.)

            MARKS NOTHING STALE, and that is the difference from the durations
            above. Both nodes this feeds are published by the runtime half,
            which is built on every publish, because a hash and a pyramid
            arrive from the analyser's thread while nothing about the show
            moves - a cached half would have frozen them empty.

            Held by pointer and not owned; the MediaInfo must outlive the
            tree's last publish. */
        void setMediaInfo (const audio::MediaInfo* mediaToRead) noexcept { mediaInfo = mediaToRead; }

        /*  WHERE EACH LIST IS BEING POINTED, for `list/aim`, `list/solve` and
            `list/statePosition`.

            Held by pointer and not owned, like the sender and the durations,
            and absent for the same kind of reason: `wfg tree` and a replay
            point at nothing, so every list reads an empty aim - which is the
            truth about a session in which nobody has asked what the show would
            be, not a placeholder standing in for one. */
        void setListState (const cue::ListState* stateToPublish) noexcept
        {
            lists = stateToPublish;
        }

        /** The show changed; rebuild before the next publish. */
        /*  The SHOW moved. The mounted half is not touched by this: it has a
            cache of its own, invalidated by the mount table's own revision
            counter, because M9 measured it at twenty-nine times the cost of
            everything else and a cue rename must not pay it. */
        void markStale() noexcept { stale = true; }

        /** How many times the mounted half has been rebuilt. See the member. */
        std::size_t mountRebuilds() const noexcept { return mountRebuildCount; }

        /*  How many times the edit-time liveness analysis has been rebuilt.
            The same shape and the same argument as the count above: what the
            cache guarantees is countable and exact - a hundred publishes with
            nothing edited rebuild it no times at all - and a wall clock on a
            shared CI runner is a flaky test. M18 asserts it. */
        std::size_t analysisRebuilds() const noexcept { return analysis.rebuilds(); }


        /*  Any thread. The most recently published snapshot, or an empty one
            before the first publish - never nullptr, so a caller never has to
            check. */
        std::shared_ptr<const TreeSnapshot> snapshot() const;

    private:
        void rebuildDocumentPart();
        void rebuildMountPart();

        const doc::ShowDocument& document;
        const CommandRegistry& commands;
        const MountTable& mounts;
        const MountSender* sender = nullptr;
        const midi::PortTable* ports = nullptr;
        const surface::SurfaceTable* surfaces = nullptr;
        /*  What a test handed in, when one did. Otherwise the lengths come
            from `mediaInfo` at the top of every publish. */
        const std::map<std::string, double>* fixedDurations = nullptr;

        /*  Held for the length of one publish so the map cannot be swapped out
            from under the walk, and released with the next one. */
        std::shared_ptr<const std::map<std::string, double>> durationsHeld;
        const std::map<std::string, double>* durations = nullptr;
        const audio::MediaInfo* mediaInfo = nullptr;

        /*  Every declared slot, in document order, as the document half last
            saw them. The runtime half publishes `holder` and `pending` against
            this rather than walking the show again: those two change with every
            run and the document half is a cache. */
        std::vector<std::string> declaredSlots;

        /*  Every cue the show holds, in document order, as the document half
            last saw them - the same shape and the same reason as the slot
            roster above.

            `/godot/cue/<id>/prepare` cannot come from the cached half: how far
            ahead a cue has been got ready changes as the pointer moves and as
            a horizon works, while nothing about the show does, so published
            from there it would freeze at whatever it was when somebody last
            edited a cue. And it cannot be published for SOME cues only, or a
            client polling a cue would watch its node list change shape. So
            every cue gets one, out of the half that is rebuilt every tick. */
        std::vector<std::string> declaredCues;

        /*  Every MEDIA cue, in document order, with the `file` it named when
            the document half last walked it: the roster `/godot/cue/<id>/hash`
            is published against, the `prepare` shape exactly (§14.5).

            The file comes WITH the identifier because the hash is looked up by
            file - the path the document writes, which is the key MediaInfo
            files its records under - and the runtime half has no document to
            read it from. It cannot go stale: an edit to a cue's `file` is an
            edit to the show, and rebuilds this with everything else.

            And only media cues, which is the one way this differs from the
            roster above. A memo has no file to hash, so it has no hash node at
            all, which is `duration`'s rule - and a media cue has one from the
            moment it exists, empty until the analyser answers, so no client
            watches a cue's node list change shape. */
        std::vector<std::pair<std::string, std::string>> declaredMedia;

        /** Every cue list, in document order. See `declaredCues`. */
        std::vector<std::string> declaredLists;

        /*  Every DCA, in document order: the roster `/godot/dca/<id>/trim` is
            published against from the runtime half, because a trim is what a
            fader is doing tonight and the document half is a cache. */
        std::vector<std::string> declaredDcas;
        const cue::RunTable& runs;

        /*  Which cues can be holding one slot at once, and every dangling
            reference the show has - both functions of the document at one
            revision, so both out of one cache asked by that revision. */
        cue::SlotAnalysis analysis;

        const cue::ListState* lists = nullptr;

        /*  THE LAST SOLVE, AND WHAT IT WAS A SOLVE OF.

            A solve is a walk of a list and it is published at five hertz, so it
            is computed when the question changes rather than fifty times a
            second for an answer nobody moved. The question is the aim and the
            document: the same aim over an edited show is a different answer,
            which is why the revision is half the key. */
        mutable std::map<std::string, std::string> solves;
        mutable std::map<std::string, std::string> solvedFor;

        std::shared_ptr<const std::vector<Node>> documentPart;

        /*  SOMEBODY ELSE'S NAMESPACE, CACHED APART FROM THE SHOW'S.

            M9's answer. With WFS-DIY's own capture mounted - 2487 nodes of a
            megabyte of JSON - one applied mutation cost 3.24 ms in a Release
            build, of which 3.13 ms was re-materialising and re-sorting the
            mounted half. That is 16% of a twenty-millisecond tick spent
            rebuilding a namespace that had not changed, every time a cue was
            renamed; and Phase 3 adds mutation RATE, because a trigger can fire
            forty times a minute.

            So it has a cache of its own, and the thing that invalidates it is
            the mount table's own revision counter rather than a flag anybody
            has to remember to set. */
        std::shared_ptr<const std::vector<Node>> mountPart;
        std::uint64_t mountRevision = 0;

        /*  How many times the mounted half has actually been rebuilt.

            PUBLISHED SO THE SPLIT CAN BE ASSERTED RATHER THAN TIMED. M9's
            numbers are a wall clock, and a wall-clock threshold on a shared CI
            runner is a flaky test that teaches people to re-run the suite. What
            the split guarantees is countable and exact: a hundred edits to the
            SHOW rebuild the mounted half no times at all. Put back the way it
            was, that count is a hundred. */
        std::size_t mountRebuildCount = 0;
        bool stale = true;

        /*  A plain mutex rather than the RtSnapshot spin lock, and the
            difference is which threads are involved. Neither side here is the
            audio thread: the tick thread publishes and the server threads read,
            and both are allowed to block for the few nanoseconds a pointer swap
            takes. When Phase 2 puts a READER on the audio callback, that reader
            gets the spin-lock treatment - the lipogram (PRD §4.2) is about that
            thread, not about this one. */
        mutable std::mutex publishMutex;
        std::shared_ptr<const TreeSnapshot> published;
    };
}
