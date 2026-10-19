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

#include <wfg/engine/audio/AuxBus.h>

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace wfg::audio
{
    void AuxBus::prepare (int writerCount, int columnCount, int maxBlock)
    {
        writers = std::max (0, writerCount);
        columns = std::max (0, columnCount);
        maxSamples = std::max (0, maxBlock);

        samples.assign (static_cast<std::size_t> (writers) * static_cast<std::size_t> (columns)
                          * static_cast<std::size_t> (maxSamples), 0.0f);

        silence = std::make_unique<std::atomic<bool>[]> (static_cast<std::size_t> (std::max (1, writers)));

        for (int writer = 0; writer < writers; ++writer)
            silence[static_cast<std::size_t> (writer)].store (true, std::memory_order_relaxed);
    }

    float* AuxBus::row (int writer, int column) noexcept
    {
        if (writer < 0 || writer >= writers || column < 0 || column >= columns)
            return nullptr;

        return samples.data() + (static_cast<std::size_t> (writer) * static_cast<std::size_t> (columns)
                                   + static_cast<std::size_t> (column)) * static_cast<std::size_t> (maxSamples);
    }

    const float* AuxBus::row (int writer, int column) const noexcept
    {
        return const_cast<AuxBus*> (this)->row (writer, column);
    }

    void AuxBus::setSilent (int writer, bool silent) noexcept
    {
        if (writer >= 0 && writer < writers)
            silence[static_cast<std::size_t> (writer)].store (silent, std::memory_order_release);
    }

    bool AuxBus::isSilent (int writer) const noexcept
    {
        if (writer < 0 || writer >= writers)
            return true;

        return silence[static_cast<std::size_t> (writer)].load (std::memory_order_acquire);
    }

    void AuxBus::addColumn (int column, float* destination, int start, int numSamples) const noexcept
    {
        if (destination == nullptr || column < 0 || column >= columns || start < 0)
            return;

        const auto frames = std::min (numSamples, maxSamples - start);

        if (frames <= 0)
            return;

        for (int writer = 0; writer < writers; ++writer)
            if (! isSilent (writer))
                if (const auto* from = row (writer, column))
                    juce::FloatVectorOperations::add (destination, from + start, frames);
    }
}
