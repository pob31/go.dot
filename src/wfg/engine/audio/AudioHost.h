/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <wfg/engine/audio/CueMatrix.h>
#include <wfg/engine/clock/SampleClock.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

/*  Tracktion Engine, stood up with no audio hardware, and the sample counter it
    advances.

    THIS HEADER NAMES NO TRACKTION TYPE, and the reason is not tidiness. Two
    traps live in those headers and both are recorded in Console.cpp:
    tracktion_engine.h bare-`#undef`s `__TEXT`, and tracktion_engine_playback.cpp
    `#undef`s and redefines `VERSION` mid-translation-unit. A pimpl keeps both
    inside one .cpp instead of leaking into every test that wants to pump a
    block. It also keeps the compile cost where it is paid once.

    WHY A HOST AT ALL, RATHER THAN CALLING TRACKTION FROM THE ENGINE. Because
    PRD §3.25 inverts the usual arrangement: Go.dot owns time and Tracktion is a
    player it commands. The tick thread owns the model and must never touch a
    Tracktion ValueTree - every one of those writes asserts the message thread -
    so the two live behind a seam from the first line rather than being
    separated later, when there would be call sites to find.

    WHAT THIS CLASS DOES NOT DO YET. It opens no device: the hosted interface is
    driven by whoever calls processBlock(), which in tests is the test and in
    PR 2.7 will be a real audio callback. It generates no Edit. Both arrive in
    their own commits; this one answers the first question, which is whether the
    engine comes up at all in our build without touching the user's machine.
*/
namespace wfg::audio
{
    /** How the hosted audio interface is opened. No defaults: a rate Go.dot
        chose for itself is a rate nobody chose, which is the same reason
        `wfg serve` refuses to guess one. */
    struct HostSettings
    {
        int sampleRate = 0;
        int blockSize = 0;

        /** Hardware output channels the graph is built to fill. */
        int outputChannels = 0;
    };

    /** The shape of the show's audio, read out of the document's <Audio>. */
    struct EditSpec
    {
        /** The polyphony ceiling: N tracks is N simultaneous cues (PRD §3.25). */
        int tracks = 0;

        /** Channels per track, and so the width of a cue's source. */
        int channelsPerTrack = 2;
    };

    /*  Somewhere for a block to go once the graph has produced it.

        The audio thread calls this, once per block, after the graph has run and
        before the sample counter moves. Two uses: a test asserting that a cue
        reached the outputs it named, and PR 2.8's WAV render, which is how CI
        hears anything at all. In a show there is no sink and the blocks go
        straight to the device. */
    struct BlockSink
    {
        virtual ~BlockSink() = default;

        /** The audio thread. Allocating here would break §4.2 for everyone. */
        virtual void blockProduced (const float* const* channels,
                                    int numChannels, int numSamples) noexcept = 0;
    };

    //==============================================================================
    class AudioHost
    {
    public:
        /*  `storageFolder` is where Tracktion writes its own preferences and
            cache. It is a parameter and not a default because a test that wrote
            into the real application-data directory would leave a trace of
            itself on the machine that ran it, and because two tests running at
            once would then share one. */
        explicit AudioHost (std::string storageFolder);
        ~AudioHost();

        AudioHost (const AudioHost&) = delete;
        AudioHost& operator= (const AudioHost&) = delete;

        /** Brings the engine up and opens the hosted interface. False if the
            settings are unusable or the engine could not be created; `lastError`
            says which. Message thread. */
        bool start (const HostSettings& settings);

        /** Message thread. Safe to call without a matching start. */
        void stop();

        bool isRunning() const noexcept;

        /** Why the last start failed, empty if it did not. */
        const std::string& lastError() const noexcept;

        /*  One block through the graph, advancing the sample counter by exactly
            the block size.

            THE AUDIO THREAD CALLS THIS, from PR 2.7. Today it is called by a
            test, which is what makes a test deterministic: the graph advances
            when the test says so and not when a device interrupt says so.
            Either way it is the only thing that moves the clock. */
        void processBlock();

        /** Blocks processed since the last start. Any thread. */
        std::int64_t blocksProcessed() const noexcept;

        /*  Where each block goes after the graph has produced it. Null for none,
            which is what a show uses. Set it with the audio stopped: it is read
            on the audio thread without synchronisation, because a pointer that
            changed mid-block is a question nobody should have to ask. */
        void setBlockSink (BlockSink* sink) noexcept;

        /*  The sample counter, for TickThread. It only ever moves forward: the
            type is an AudioClockSource and not a ManualClock precisely so that
            nothing on this path can rewind it. */
        const SampleClock& clock() const noexcept;

        /** The settings the interface was opened with. Zeroed when stopped. */
        const HostSettings& settings() const noexcept;

        /*  How many wave output devices the engine sees, and how wide the first
            one is. Go.dot describes exactly one, spanning the whole rig, so
            these answer 1 and the hardware channel count - and a test that
            reads something else has found the device layout being carved into
            stereo pairs behind our back. */
        int waveOutputDeviceCount() const noexcept;
        int waveOutputDeviceWidth() const noexcept;

        /*  Builds the Edit the show plays through: a fixed set of tracks, each
            with one launcher slot, Go.dot's own output plugin, and a route to
            the one wide device. Message thread, once, at load - PRD §3.25 fixes
            the graph here and nothing after this changes its shape.

            False if the engine is not running or the spec is unusable;
            `lastError` says which. */
        bool buildEdit (const EditSpec& spec);

        /** How many tracks the generated Edit has. Zero before one is built. */
        int trackCount() const noexcept;

        /** How many channels each of those tracks carries - a cue's input width. */
        int editChannelsPerTrack() const noexcept;

        /*  Builds the playback graph a second time, offline, and asks whether
            every node in it has a unique identifier.

            WHY THIS EXISTS. Tracktion derives a node's id by hash-combining the
            ids of the items it is built from, and that combine barely mixes its
            value argument - this project reported it upstream after observing
            duplicate ids at 24 of 63 track counts, where two nodes on unrelated
            tracks then adopt one another's state across a graph rebuild. It is
            a debug assertion in Tracktion and silent in release.

            So Go.dot checks its own generated Edit rather than trusting either
            the hash or the report. Message thread, at load, on a graph that is
            thrown away. */
        struct NodeIdReport
        {
            int nodes = 0;

            /*  Nodes carrying id 0 - built from no EditItem, so with no identity
                to collide with. Tracktion's own assertion ignores them, and so
                does `duplicates`. Counted separately because a check that
                ignores most of the graph is not the check it appears to be. */
            int zeroIds = 0;

            /** Non-zero ids that appear more than once. Any is a defect. */
            int duplicates = 0;

            bool ok() const noexcept { return duplicates == 0; }
        };

        /** Builds the graph offline and counts what is in it. */
        NodeIdReport inspectNodeIds() const;

        /*  Points a track's resident clip at a media file. This is the seed of
            what PR 2.3 will call arming a cue: the clip stays, its source
            changes. Message thread, and it costs one graph rebuild - `source`
            is on Tracktion's restart list, which the plan budgets for.

            False if the index or the file is no good. */
        bool setTrackSource (int trackIndex, const std::string& mediaFile);

        /*  Waits until the track's source is mapped into the audio file cache,
            pumping blocks while it waits. False if it never became ready.

            A wave clip is silent until the cache holds a mapped Reader for its
            file, and the cache only maps a file while something holds one - so
            this is not a sleep, it is a sleep with the graph running. Firing a
            cue before this returns plays silence for as long as the disk takes,
            with the run reporting itself as playing throughout. PR 2.3's arm
            calls it from standby.

            Message thread: it sleeps, so it is never on the GO path. */
        bool waitForTrackSourceReady (int trackIndex, int timeoutMilliseconds);

        /*  Whether the cache holds a mapped reader for the track's source - the
            same question waitForTrackSourceReady waits on, ASKED rather than
            waited on.

            It exists because the waiting version pumps blocks itself, which is
            right when nothing else is pumping and a data race when something
            is. A show has a pump thread, so the arm asks once a tick and gets
            on with the tick. Any thread. */
        bool isTrackSourceReady (int trackIndex) const;

        /*  Points a track's output stage at a set of destinations: absolute
            hardware channels and their gains, plus the cue's level in dB.

            Clears the whole matrix first, because a voice is reused - whatever
            the previous cue on it was routed to would otherwise still be there,
            and a cue would play out of a speaker belonging to the one before
            it. Snapped rather than slewed: an arm happens while the voice is
            silent, and sliding up from the last cue's coefficients would be a
            fade nobody asked for at the wrong moment.

            The triples are {input, output, gain}. A plain array rather than a
            named struct because this header names no type from the cue layer
            and the cue layer names none from here. */
        void setTrackRouting (int trackIndex, double levelDb,
                              const std::vector<std::array<double, 3>>& coefficients);

        /*  Starts a track's clip as soon as the next block, unquantised.

            A DIAGNOSTIC, not the GO path. It reaches the clip - which costs two
            heap allocations - and it places no instant, so where the sound
            starts depends on which block happened to be next. M1 uses it
            because M1 asks where a cue goes and not when.

            Message thread. GO uses launchTrackAt. */
        bool launchTrack (int trackIndex);

        //======================================================================
        /*  THE GO PATH. Everything below is callable from the tick thread while
            a show is running: it allocates nothing, takes no lock the audio
            thread can block on, and touches no Tracktion ValueTree.

            What makes that possible is that buildEdit resolved the launch
            handles once, on the message thread, and cached them. Reaching a
            clip costs two heap allocations every time - getAudioTracks does an
            unconditional ensureStorageAllocated, and getClipSlots returns an
            array by value - which is fine in a diagnostic and not fine at
            50 Hz on the thread that owns the model. */

        /*  Turns one of Go.dot's own future sample positions into the beat to
            launch at.

            NOT A CONVERSION GO.DOT DERIVED. The offset between Go.dot's sample
            counter and Tracktion's beat axis is MEASURED once per block, in the
            callback, where the two numbers describe the same instant - see the
            note on the anchor in AudioHost.cpp. Any thread; meaningless before
            the first block has gone through, and anchoredAtSample says so. */
        double beatsAtSample (std::int64_t sample) const noexcept;

        /** The sample the anchor was last taken at. Zero before the first block. */
        std::int64_t anchoredAtSample() const noexcept;

        /*  Tracktion's own sample counter minus Go.dot's - one block in a
            healthy run. Published rather than asserted because a CHANGE in it
            means Tracktion skipped blocks (a suspended device, a CPU-overload
            mute, a resync), and that is the number to look at when a show has
            drifted and nobody knows why. */
        std::int64_t referenceSkewSamples() const noexcept;

        /*  Queues a track's clip to start at a beat. Tick thread.

            A BEAT THAT HAS ALREADY PASSED DOES NOT SIMPLY START LATE. Tracktion
            plays one block from the head of the file and only then jumps
            forward by the lateness, so the cue is late AND has a hole in it -
            verified in tracktion_LaunchHandle.cpp, where the block being
            rendered takes the block's own start while the state stored for the
            blocks after it is back-dated. That is why the launch instant is
            placed well ahead rather than as soon as possible, and why lateness
            is counted rather than tolerated.

            False when there is no such track or no graph. */
        bool launchTrackAt (int trackIndex, double monotonicBeat) noexcept;

        /*  Stops a track's clip at a beat, or at the next block when none is
            given. Tick thread.

            BEST EFFORT WHEN THE CLIP HAS NOT STARTED YET, and a caller has to
            know it: cancelling a queued launch reads Tracktion's queue through
            a try-lock that answers "nothing queued" when the audio thread
            happens to hold it, which is indistinguishable from there really
            being nothing. So a cancel can silently not happen. Go.dot keeps its
            own record of what it launched and confirms on the next tick rather
            than trusting the answer. */
        bool stopTrackAt (int trackIndex, double monotonicBeat) noexcept;
        bool stopTrack (int trackIndex) noexcept;

        /*  What a track's launch handle says right now, as a plain value.

            Tick thread, and safe: an atomic acquire load plus a seqlock read,
            both wait-free absent a concurrent write. It names no Tracktion type,
            so the vendor-free surface stays vendor-free. */
        struct TrackPlayState
        {
            bool valid = false;      ///< false when there is no such track
            bool playing = false;

            /** Beats played since it started. Zero when it is not playing. */
            double playedBeats = 0.0;
        };

        TrackPlayState trackPlayState (int trackIndex) const noexcept;

        /*  Whether a track's clip is playing, and how long its source is. PR 2.3
            needs the first to notice a cue has finished; both are here now
            because a silent output has several possible causes and guessing
            between them is not a diagnosis. */
        bool isTrackPlaying (int trackIndex) const;
        double trackSourceLengthSeconds (int trackIndex) const;

        /*  How many tracks hold a resident clip in their slot. Should equal the
            track count: a slot with no clip means that track's launcher node -
            and with it the track's whole output stage - is absent from the
            playback graph, silently. */
        int residentClipCount() const;

        /*  A track's output stage - its level and routing coefficients. This is
            what a media cue writes when it is armed, and what a fade writes at
            50 Hz. Null for an index no track answers to. */
        CueMatrix* trackMatrix (int trackIndex) noexcept;

        /*  The loudest sample the track's output plugin saw arriving and
            leaving, since the last reset.

            These exist because a silent output has several possible causes -
            the clip never started, the clip stopped early, the matrix is wrong,
            the graph never reaches the device - and a test that can only see the
            far end cannot tell them apart. They are how M1 established that a
            cue was routed correctly and still went quiet halfway through.
            Written on the audio thread as relaxed stores; read from anywhere. */
        float trackInputPeak (int trackIndex) const;
        float trackOutputPeak (int trackIndex) const;
        void resetTrackPeaks (int trackIndex);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
