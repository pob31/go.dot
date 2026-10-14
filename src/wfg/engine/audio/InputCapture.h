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

#include <wfg/engine/rt/RtCheck.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
    WHAT AN INTERFACE'S INPUTS HEARD, written to a file with the sample each
    frame was heard on (namespace draft §58, RD.9b; `wfg serve --capture`).

    WHY IT EXISTS. M61 asks how far apart two machines start the same sound, and
    the only honest place to measure that is where the sound comes back: a
    click from each machine patched into one interface's inputs, heard on one
    clock. A render of the outputs (`--render`) says what the graph made; it
    cannot say when a Dante network, a driver's buffers and a converter let it
    out. So the inputs are written as they arrive, and every frame of the file
    is tied to the count the engine places its launches on - the AudioHost's
    own, the one `/godot/run/<id>/launchSample` reads - so an analysis can put
    a cue's launch and what the inputs heard on one axis without guessing.

    THE AUDIO THREAD COPIES AND NOTHING ELSE (PRD §4.2). `write` copies the
    block's chosen inputs into a ring of slots made before the interface
    starts, then publishes the slot with one release store; no allocation, no
    lock, no call that can block, and no notify - JUCE's ThreadedWriter, which
    `--render` uses, wakes its writer by signalling an event, and an event is a
    mutex. The writer here polls instead, every few milliseconds, on a thread
    of its own: the ring is seconds deep, and a writer a few milliseconds late
    costs nothing. A ring that is full when a block arrives drops that block
    and counts it - the audio thread never waits for a disk.

    THE FILE KEEPS THE AXIS. A dropped block is written as silence of its
    length by the writer, which knows where it fell from the sample of the next
    block that did arrive and the count of frames dropped before it, so frame n
    of the file is ALWAYS sample `firstSample + n` and an overflow costs a hole,
    never a shift. A clock that moves otherwise - back, or forward by more than
    was dropped: the interface opened again and its count started again - ends
    the capture there, and the sidecar says so: one file is one stretch of one
    clock.

    THE SIDECAR (`<wav>.json`): the rate, the block, the sample of the file's
    first frame on that count, the inputs in the file's channel order (1-based,
    as the interface numbers them), the frames written, the frames dropped,
    whether the file was closed cleanly and why it ended early if it did.
    Written when the first block lands and every second after, beside the WAV
    header, which is rewritten every second for the reason Render gives: a
    harness that stops `serve` with TerminateProcess runs no destructor, and a
    capture that needed one would be the run's only lost evidence.

    THREADS. `prepare` and `stop` are the message thread's, with no block
    running into the capture (before the device's callback is added, or with it
    removed). `write` is the audio thread's. `end`, `holdWriter` and the
    readouts are any thread's.
*/
namespace wfg::audio
{
    class InputCapture
    {
    public:
        struct Settings
        {
            /** The WAV; its sidecar is the same path with `.json` added. */
            std::string file;

            /*  The interface's inputs to keep, 1-based and inclusive, as the
                interface numbers them; a last of nought is the interface's
                last. */
            int firstInput = 1;
            int lastInput = 0;

            /*  How deep the ring between the audio thread and the writer is.
                Four seconds outlasts any disk a show machine has; a test makes
                it a block deep to see what an overflow does. */
            double ringSeconds = 4.0;
        };

        explicit InputCapture (Settings);
        ~InputCapture();

        InputCapture (const InputCapture&) = delete;
        InputCapture& operator= (const InputCapture&) = delete;

        /*  THE INTERFACE IS ABOUT TO START, with `deviceInputs` inputs at this
            rate and block: the first call makes the ring, opens the file and
            starts the writer. A later call - the same interface brought up
            again - leaves a capture of the same rate, block and inputs going
            (its clock decides whether it can carry on: the class comment), and
            ends one of another shape, saying why. False, with `lastError`, when
            the inputs asked for are not on the interface, the file cannot be
            written or the capture has already ended. Message thread, with no
            block running into the capture. */
        bool prepare (int deviceInputs, int sampleRate, int blockSize);

        /*  ONE BLOCK OF THE INTERFACE'S INPUTS, `channels[0 .. numChannels)` as
            the callback was handed them, whose first frame is `firstSample` on
            the count launches are placed on. The audio thread's: copies,
            counts, and returns. A missing channel pointer is copied as silence. */
        void write (const float* const* channels, int numChannels, int numSamples,
                    std::int64_t firstSample) noexcept WFG_AUDIO_THREAD;

        /*  No block is taken from now on; the writer finishes with what the
            ring holds, and the sidecar says `why`. The first reason stands.
            Any thread but the audio thread. */
        void end (const std::string& why);

        /*  Drains the ring, closes the WAV, writes the sidecar a last time and
            joins the writer. Idempotent. Message thread, after the interface
            has stopped calling `write`. */
        void stop();

        /*  A TEST'S HAND ON THE WRITER: held, it empties nothing, so a ring made
            small overflows when the test says rather than when the machine is
            busy. */
        void holdWriter (bool held) noexcept;

        //======================================================================
        bool isPrepared() const noexcept { return prepared.load (std::memory_order_acquire); }

        /** Whether the first block has reached the file, and its sample. */
        bool hasStarted() const noexcept { return started.load (std::memory_order_acquire); }
        std::int64_t firstSample() const noexcept { return first.load (std::memory_order_acquire); }

        /** Frames in the file, the silence written for drops included. */
        std::int64_t framesWritten() const noexcept { return framesInFile.load (std::memory_order_acquire); }

        /** Frames the audio thread could not put in the ring. */
        std::int64_t framesDropped() const noexcept { return dropped.load (std::memory_order_acquire); }

        /** Why `prepare` failed, or why the capture ended early; empty if neither. */
        std::string lastError() const;

        /** The 1-based inputs in the file's channel order, once prepared. */
        std::vector<int> inputs() const;

        const Settings& settings() const noexcept { return wanted; }

        /** The sidecar's path: the WAV's with `.json` added. */
        std::string sidecarPath() const;

    private:
        struct Slot
        {
            std::int64_t sample = 0;
            int frames = 0;

            /*  The audio thread's drop count when this slot was filled, so the
                writer knows how much of the gap before it was dropped rather
                than a clock that moved. */
            std::int64_t droppedBefore = 0;
        };

        struct File;

        void run();
        void drain();
        void writeSidecar (bool closed);
        void finishWith (const std::string& why);
        bool refuse (const std::string& why);

        Settings wanted;

        //  The shape the first `prepare` made: fixed for the capture's life.
        int channelCount = 0, firstChannel = 0, rate = 0, block = 0, deviceWidth = 0;
        int slotFrames = 0;
        std::size_t slotCount = 0;

        std::vector<Slot> slots;
        std::vector<float> samples;          // slotCount x channelCount x slotFrames, planar a slot
        std::vector<float> silence;          // slotFrames of nought, for a drop's hole
        std::vector<const float*> pointers;  // the writer's, one a channel

        std::atomic<std::uint64_t> filled { 0 };    // slots the audio thread has filled
        std::atomic<std::uint64_t> emptied { 0 };   // slots the writer has emptied
        std::atomic<std::int64_t> dropped { 0 };
        std::atomic<bool> prepared { false }, ended { false }, held { false };

        //  The writer's own; published for the readouts.
        std::atomic<bool> started { false };
        std::atomic<std::int64_t> first { 0 }, framesInFile { 0 };
        std::int64_t expected = 0, droppedAccounted = 0;
        bool axisBroken = false;

        std::unique_ptr<File> file;
        std::thread writer;
        mutable std::mutex mutex;            // the writer's wake-up and the reasons; never the audio thread's
        std::condition_variable wake;
        bool stopping = false;
        std::string reason;
        std::string error;
    };
}
