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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/*
    THE SOUND A DECKLINK CARD EMBEDS, TAKEN OFF THE AUDIO THREAD (namespace
    draft §71, APH, APJ).

    A card output may carry buses of the show in its cable beside the picture.
    The card lives in the renderer, a process of its own; the sound is made on
    the audio thread here. Between them is this: a ring of blocks in a file
    both map, the engine writing and the renderer reading.

    WHAT IS TAKEN. Each block, after every output's trim and DCA and after
    LTC (AudioHost::processBlock, where the interface's own samples are), the
    logical output channels the lane map names are copied, one a lane: the
    union of every card output's channels, each output picking its own lanes
    from the map. A channel the interface was not opened with is a lane of
    silence. Nothing when the map is empty, which is every show without a card
    carrying sound.

    THE AUDIO THREAD COPIES AND NOTHING ELSE (PRD §4.2): fixed slots, made with
    the file; `memcpy` and relaxed or release stores; no allocation, lock,
    system call or notify. A block longer than a slot takes several. A full
    ring drops what does not fit and counts it - the audio thread never waits
    for a renderer that is slow or not running. Each slot carries the sample
    its first frame was made for on the count launches are placed on, so the
    reader puts it where it belongs and fills a drop with silence of its length.

    THE LANE MAP is the tick thread's: two maps, the current one named by an
    index, the other written and then made current. A block reads whichever is
    current as it begins.

    THE FILE (`File`) is made by the engine before any audio driver and let go
    after the last, so no block ever writes into a region gone; its pages are
    touched at once and locked into memory where the system allows, so a write
    on the audio thread is never a page brought in from disk.
*/
namespace wfg::audio::cardsound
{
    /** "GdCS", read as a little-endian word. */
    constexpr std::uint32_t magic = 0x53436447u;

    /** Bumped whenever the structure below changes shape. */
    constexpr std::uint32_t version = 1;

    /** Lanes the ring carries at most: as many channels as a card embeds. */
    constexpr int maxLanes = 32;

    /** Frames a slot holds, and slots in the ring - a third of a second at 48 kHz. */
    constexpr int slotFrames = 256;
    constexpr int slotCount = 64;

    struct Slot
    {
        std::atomic<std::int64_t> sample;          ///< the first frame's, on Go.dot's count
        std::atomic<std::int32_t> frames;
        std::atomic<std::int32_t> rate;            ///< the interface's, as the block was made
        std::atomic<std::int32_t> lanes;
        std::atomic<std::int64_t> droppedBefore;   ///< frames dropped when this slot was filled
    };

    struct LaneMap
    {
        std::atomic<std::int32_t> count;
        std::atomic<std::int32_t> channel[maxLanes];   ///< the logical output channel each lane carries
    };

    struct Region
    {
        std::atomic<std::uint32_t> magic;
        std::atomic<std::uint32_t> version;
        std::atomic<std::uint32_t> bytes;

        /*  The tick thread's: which of the two maps is current. */
        std::atomic<std::uint32_t> mapIndex;
        LaneMap maps[2];

        /*  The audio thread's count of slots filled, and the reader's of
            slots emptied, a cache line apart - spelled out as gaps, which a
            compiler does not warn about as it does `alignas` padding. */
        char beforeFilled[64];
        std::atomic<std::uint64_t> filled;
        char betweenCounts[64];
        std::atomic<std::uint64_t> emptied;
        char afterEmptied[64];
        std::atomic<std::int64_t> dropped;

        Slot slots[slotCount];
        float samples[slotCount][maxLanes][slotFrames];
    };

    static_assert (std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::int32_t>::is_always_lock_free
                     && std::atomic<std::int64_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free,
                   "two processes share these: a locking atomic would lock nothing across them");

    constexpr std::size_t regionBytes() noexcept
    {
        constexpr std::size_t page = 4096;
        return (sizeof (Region) + page - 1) / page * page;
    }

    /*  A freshly zeroed region stamped as this build's, the size last with
        release; and whether a mapped one is. */
    void stamp (Region& r) noexcept;
    bool looksValid (const Region& r) noexcept;

    //==========================================================================
    /*  THE TICK THREAD'S: the logical output channels to carry, one a lane, at
        most maxLanes; empty carries nothing. */
    void publishLanes (Region& r, const std::vector<int>& channels) noexcept;

    /*  The current map, as a reader sees it. */
    std::vector<int> lanesOf (const Region& r);

    //==========================================================================
    /*  THE AUDIO THREAD'S: one block of the logical outputs, `channels[0 ..
        numChannels)`, whose first frame is `firstSample` on Go.dot's count,
        at `sampleRate`. Copies what the map names into the ring, and returns. */
    void write (Region& r, const float* const* channels, int numChannels, int numSamples, std::int64_t firstSample,
                int sampleRate) noexcept WFG_AUDIO_THREAD;

    //==========================================================================
    /*  THE READER'S (the renderer): the oldest slot not yet taken, its lanes
        read where they lie, until `release`. False when there is none. */
    struct Block
    {
        std::int64_t sample = 0;
        int frames = 0;
        int rate = 0;
        int lanes = 0;
        std::int64_t droppedBefore = 0;
        std::array<const float*, maxLanes> lane {};
    };

    bool peek (const Region& r, Block& block) noexcept;
    void release (Region& r) noexcept;

    /*  Frames the audio thread dropped since the region was made. */
    std::int64_t droppedFrames (const Region& r) noexcept;

    //==========================================================================
    /*  THE FILE THE TWO PROCESSES MAP, made in `folder`, zeroed, stamped and
        locked into memory where the system allows; deleted when it goes. */
    class File
    {
    public:
        explicit File (const std::string& folder);
        ~File();

        File (const File&) = delete;
        File& operator= (const File&) = delete;

        /*  The region, or null when the file could not be made - and why. */
        Region* region() const noexcept;
        const std::string& path() const noexcept;
        const std::string& problem() const noexcept;

        /*  Whether its pages are locked into memory. */
        bool locked() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
