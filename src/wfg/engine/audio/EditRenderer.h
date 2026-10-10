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

#include <wfg/engine/audio/EditRenderTable.h>
#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/document/MediaEdit.h>
#include <wfg/engine/document/ShowDocument.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*  THE RENDERS OF A SOUND'S EDIT (namespace draft §55, ADM, ADN).

    While a sound cue's edit is open the cue plays a render of its sections,
    made here, on a thread of its own, after every change: the file read block
    by block, each section's material at its trim, an equal-power crossfade
    centred on every join that is not still one in the file, written as a
    32-bit float WAV at the file's rate and channels under `media/.edits/`.

    A NEW NAME FOR EVERY DISTINCT EDIT: the file a cue plays is mapped by the
    audio side and cannot be written over on Windows, and both the analyser
    and the player key a file by its path and never notice changed bytes. So
    a render is named by a key - the file's name, size and time and the edit's
    text, hashed - and an edit put back as it was finds its render still there.
    The renders are a cache: never analysed, never copied by Save as, swept at
    open of the keys no edit has, and a render a newer one has replaced is let
    go of once no run names it.

    FREEZE (ADN) copies the render into media/ as `<stem> (edit).wav` - the
    name free, " 2" and so on when taken, as a conversion's is - on this
    thread too, and then says so; serve turns that into `media.frozen`, the
    document's swap, one undoable record, so the bounce and the swap are two
    halves a replay keeps apart as a conversion's are.

    Modelled on `MediaAnalyser`: a std::thread, a lock, a queue, a stop raised
    under the lock, and a snapshot of what it knows swapped whole so the tick
    thread holds one per tick as it holds the lengths.
*/
namespace wfg::audio
{
    /** The whole of an edit, as a job: the cue, the file it was made from and its sections. */
    struct RenderJob
    {
        std::string cue;
        std::string sourceName;             // relative to the media folder, as the row holds it
        std::vector<doc::Section> sections;
    };

    struct RenderResult
    {
        bool ok = false;
        double seconds = 0.0;
        std::string problem;
    };

    /*  THE RENDER ITSELF, synchronous, from a file to a file; the tests call
        it straight. `stop` raised ends it between two blocks and leaves no
        file behind; `progress` is told the percent after every block. Reads
        past either end of the file are silence. */
    RenderResult renderEdit (const std::string& sourcePath, const std::vector<doc::Section>& sections,
                             const std::string& targetPath, const std::atomic<bool>* stop = nullptr,
                             const std::function<void (int)>& progress = {});

    /** The key a render is named by: sixteen hex characters of a hash over the file's name, size, time and the edit's text. */
    std::string renderKeyOf (const std::string& sourceName, std::int64_t sizeBytes, std::int64_t modifiedMs,
                             const std::string& editText);

    /** The bounce a freeze writes: "<stem> (edit).wav", and " 2".." 999" when that name is taken. */
    std::string freeBounceName (const std::string& folder, const std::string& sourceName);

    //==============================================================================
    class EditRenderer
    {
    public:
        /*  Publishes the renders' lengths into `mediaToPublishInto`, which must
            outlive it; serve declares this after its MediaInfo and its
            analyser, so it is destroyed - and stopped - first. */
        EditRenderer (MediaInfo& mediaToPublishInto, std::string mediaFolder);
        ~EditRenderer();

        EditRenderer (const EditRenderer&) = delete;
        EditRenderer& operator= (const EditRenderer&) = delete;

        bool start();
        void stop();
        bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }

        /*  ANY THREAD. The edit as it now is, rendered unless a render of
            exactly this edit is there or in hand; a job of the same cue still
            waiting is replaced. Nothing to offer for a cue whose sections are
            gone - `forget` it. */
        void offer (const RenderJob& job);
        void forget (const std::string& cue);

        /*  ANY THREAD, once at open, after the open edits were offered: the
            renders under `.edits/` whose key none of these jobs has are
            removed, and a part file older than an hour. Run after the renders
            queued before it. */
        void sweep (std::vector<RenderJob> openEdits);

        /*  THE RENDERS A NEWER ONE HAS REPLACED, by their relative names,
            handed over once; the caller removes those no run names
            (`discard`) and hands the rest back (`stale`). */
        std::vector<std::string> takeStale();
        void stale (const std::string& relativeFile);
        void discard (const std::string& relativeFile);

        /** What it knows, swapped whole on every change; `changes` moves when it does. */
        std::shared_ptr<const EditRenders> snapshot() const;
        std::uint32_t changes() const noexcept { return moved.load (std::memory_order_acquire); }

        /** The readout's text: one line per cue - cue, state, percent, problem - a tab between each. */
        static std::string readoutText (const EditRenders& renders);

        //==============================================================================
        struct FreezeJob
        {
            std::string cue;
            std::string source;       // the file the edit was made from
            std::string renderFile;   // the render, relative to the media folder
        };

        /** Told on the renderer's thread when a bounce has landed, or could not. */
        using FreezeDone = std::function<void (const FreezeJob& job, const std::string& bounce, const std::string& problem)>;

        void setOnFrozen (FreezeDone done);

        /** ANY THREAD: the render copied into media/ beside its source, then `onFrozen`. */
        void freeze (const FreezeJob& job);

    private:
        void run();
        void render (const RenderJob& job);
        void runSweep (const std::vector<RenderJob>& openEdits);
        void runFreeze (const FreezeJob& job);
        void publishEntry (const EditRender& entry);
        std::string keyFor (const RenderJob& job) const;

        MediaInfo* media = nullptr;
        const std::string folder;

        mutable std::mutex guard;
        std::condition_variable wake;
        std::deque<RenderJob> queued;
        std::deque<FreezeJob> freezes;
        std::vector<RenderJob> sweepWanted;
        bool sweepAsked = false;
        std::shared_ptr<const EditRenders> known { std::make_shared<EditRenders>() };
        std::vector<std::string> replaced;
        FreezeDone onFrozen;
        std::atomic<std::uint32_t> moved { 0 };

        std::atomic<bool> running { false };
        std::atomic<bool> stopping { false };
        std::thread thread;
    };

    /*  `media.freeze <cue>` (namespace draft §55.3): the bounce asked for.
        Refused until the render of the edit as it now is exists (`busy`),
        under the lock, on a cue that is no sound, on one with no open edit.
        Taken and ignored where nothing renders - a replay, a rig - so a log
        that holds it replays; `media.frozen`, the record the renderer submits
        when the bounce has landed, is the document's and replays as the swap. */
    void registerEditRenderCommands (CommandRegistry&, EditRenderer*, const doc::ShowDocument&);
}
