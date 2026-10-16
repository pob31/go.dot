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

#include <wfg/engine/audio/CardSound.h>

#include <wfg/engine/plugin/ProcessUtil.h>

#include <juce_core/juce_core.h>

#if defined (_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <sys/mman.h>
#endif

#include <algorithm>
#include <cstring>
#include <new>

namespace wfg::audio::cardsound
{
    void stamp (Region& r) noexcept
    {
        r.magic.store (magic, std::memory_order_relaxed);
        r.version.store (version, std::memory_order_relaxed);
        r.mapIndex.store (0, std::memory_order_relaxed);
        r.maps[0].count.store (0, std::memory_order_relaxed);
        r.maps[1].count.store (0, std::memory_order_relaxed);
        r.bytes.store (static_cast<std::uint32_t> (sizeof (Region)), std::memory_order_release);
    }

    bool looksValid (const Region& r) noexcept
    {
        return r.bytes.load (std::memory_order_acquire) == static_cast<std::uint32_t> (sizeof (Region))
            && r.magic.load (std::memory_order_relaxed) == magic && r.version.load (std::memory_order_relaxed) == version;
    }

    void publishLanes (Region& r, const std::vector<int>& channels) noexcept
    {
        const auto next = 1u - (r.mapIndex.load (std::memory_order_relaxed) & 1u);
        auto& map = r.maps[next];
        const auto count = std::min<std::size_t> (channels.size(), maxLanes);

        for (std::size_t lane = 0; lane < count; ++lane)
            map.channel[lane].store (channels[lane], std::memory_order_relaxed);

        map.count.store (static_cast<std::int32_t> (count), std::memory_order_relaxed);
        r.mapIndex.store (next, std::memory_order_release);
    }

    std::vector<int> lanesOf (const Region& r)
    {
        const auto& map = r.maps[r.mapIndex.load (std::memory_order_acquire) & 1u];
        const auto count = std::clamp (map.count.load (std::memory_order_relaxed), 0, maxLanes);
        std::vector<int> out;

        for (int lane = 0; lane < count; ++lane)
            out.push_back (map.channel[lane].load (std::memory_order_relaxed));

        return out;
    }

    void write (Region& r, const float* const* channels, int numChannels, int numSamples, std::int64_t firstSample,
                int sampleRate) noexcept WFG_AUDIO_THREAD
    {
        const auto& map = r.maps[r.mapIndex.load (std::memory_order_acquire) & 1u];
        const auto lanes = std::clamp (map.count.load (std::memory_order_relaxed), 0, maxLanes);

        if (lanes == 0 || numSamples <= 0)
            return;

        for (int offset = 0; offset < numSamples; offset += slotFrames)
        {
            const auto frames = std::min (slotFrames, numSamples - offset);
            const auto filled = r.filled.load (std::memory_order_relaxed);

            //  Full: dropped and counted, never waited for.
            if (filled - r.emptied.load (std::memory_order_acquire) >= static_cast<std::uint64_t> (slotCount))
            {
                r.dropped.fetch_add (frames, std::memory_order_relaxed);
                continue;
            }

            const auto index = static_cast<std::size_t> (filled % static_cast<std::uint64_t> (slotCount));
            auto& slot = r.slots[index];

            for (int lane = 0; lane < lanes; ++lane)
            {
                auto* to = r.samples[index][lane];
                const auto channel = map.channel[lane].load (std::memory_order_relaxed);

                if (channel >= 0 && channel < numChannels && channels[channel] != nullptr)
                    std::memcpy (to, channels[channel] + offset, static_cast<std::size_t> (frames) * sizeof (float));
                else
                    std::memset (to, 0, static_cast<std::size_t> (frames) * sizeof (float));
            }

            slot.sample.store (firstSample + offset, std::memory_order_relaxed);
            slot.frames.store (frames, std::memory_order_relaxed);
            slot.rate.store (sampleRate, std::memory_order_relaxed);
            slot.lanes.store (lanes, std::memory_order_relaxed);
            slot.droppedBefore.store (r.dropped.load (std::memory_order_relaxed), std::memory_order_relaxed);
            r.filled.store (filled + 1, std::memory_order_release);
        }
    }

    bool peek (const Region& r, Block& block) noexcept
    {
        const auto emptied = r.emptied.load (std::memory_order_relaxed);

        if (r.filled.load (std::memory_order_acquire) == emptied)
            return false;

        const auto index = static_cast<std::size_t> (emptied % static_cast<std::uint64_t> (slotCount));
        const auto& slot = r.slots[index];
        block.sample = slot.sample.load (std::memory_order_relaxed);
        block.frames = std::clamp (slot.frames.load (std::memory_order_relaxed), 0, slotFrames);
        block.rate = slot.rate.load (std::memory_order_relaxed);
        block.lanes = std::clamp (slot.lanes.load (std::memory_order_relaxed), 0, maxLanes);
        block.droppedBefore = slot.droppedBefore.load (std::memory_order_relaxed);

        for (int lane = 0; lane < maxLanes; ++lane)
            block.lane[static_cast<std::size_t> (lane)] = lane < block.lanes ? r.samples[index][lane] : nullptr;

        return true;
    }

    void release (Region& r) noexcept
    {
        r.emptied.fetch_add (1, std::memory_order_release);
    }

    std::int64_t droppedFrames (const Region& r) noexcept
    {
        return r.dropped.load (std::memory_order_relaxed);
    }

    //==========================================================================
    struct File::Impl
    {
        juce::File file;
        std::unique_ptr<juce::MemoryMappedFile> mapping;
        Region* region = nullptr;
        std::string path;
        std::string problem;
        bool locked = false;
    };

    File::File (const std::string& folder) : impl (std::make_unique<Impl>())
    {
        static std::atomic<int> made { 0 };

        const juce::File where { juce::String::fromUTF8 (folder.c_str()) };
        where.createDirectory();

        impl->file = where.getChildFile ("card-sound-" + juce::String (plugin::process::currentId()) + "-"
                                         + juce::String (++made) + ".shm");
        impl->path = impl->file.getFullPathName().toStdString();

        const auto bytes = regionBytes();

        {
            juce::FileOutputStream out (impl->file);

            if (! out.openedOk())
            {
                impl->problem = "could not create " + impl->path;
                return;
            }

            out.setPosition (0);
            out.truncate();
            const std::vector<char> zeros (bytes, 0);
            out.write (zeros.data(), zeros.size());
        }

        impl->mapping = std::make_unique<juce::MemoryMappedFile> (impl->file, juce::MemoryMappedFile::readWrite, false);

        if (impl->mapping->getData() == nullptr || impl->mapping->getSize() < bytes)
        {
            impl->problem = "could not map " + impl->path;
            impl->mapping.reset();
            impl->file.deleteFile();
            return;
        }

        auto* data = impl->mapping->getData();

        //  Every page touched before any block writes to it, and kept in memory where the system allows.
        std::memset (data, 0, bytes);

       #if defined (_WIN32)
        impl->locked = VirtualLock (data, bytes) != 0;
       #else
        impl->locked = mlock (data, bytes) == 0;
       #endif

        impl->region = new (data) Region {};
        stamp (*impl->region);
    }

    File::~File()
    {
        if (impl->mapping != nullptr && impl->locked)
        {
           #if defined (_WIN32)
            VirtualUnlock (impl->mapping->getData(), regionBytes());
           #else
            munlock (impl->mapping->getData(), regionBytes());
           #endif
        }

        impl->region = nullptr;
        impl->mapping.reset();

        if (impl->file != juce::File())
            impl->file.deleteFile();
    }

    Region* File::region() const noexcept             { return impl->region; }
    const std::string& File::path() const noexcept    { return impl->path; }
    const std::string& File::problem() const noexcept { return impl->problem; }
    bool File::locked() const noexcept                { return impl->locked; }
}
