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
    THE PICTURE SIDE AS A REGION (Phase 8a, namespace draft 35.4): the Sink the
    Runner talks to, written straight into the memory the renderer reads. Tick
    thread, all of it, and never waiting - a slot's what-it-is under its
    sequence, a point appended and published by its count, a removal stored.

    ONE SLOT PER LAYER, found by the run's identifier. The engine keeps which
    identifier holds which slot on its own side, so finding one is a short
    loop over strings the engine owns and never a read of shared memory. A
    slot whose removal is placed is let go only once the clock has passed it
    by a margin (`release`), so the renderer never finds a slot reused under
    a layer it is still fading out.

    NAMES NO JUCE TYPE.
*/

#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/VideoSink.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wfg::video
{
    class RegionSink final : public Sink
    {
    public:
        explicit RegionSink (region::Region& regionToWrite) noexcept : r (regionToWrite)
        {
            removing.fill (region::notRemoved);
        }

        void show (const LayerSpec& spec) override
        {
            auto at = slotOf (spec.id);

            if (at < 0)
                at = freeSlot();

            /*  SIXTY-FOUR PICTURES UP AT ONCE IS THE LIMIT, and the
                sixty-fifth shows nothing rather than taking another's place. */
            if (at < 0)
                return;

            auto& slot = r.layers[static_cast<std::size_t> (at)];

            region::beginWrite (slot.seq);
            slot.used.store (1, std::memory_order_relaxed);
            region::writeText (slot.id, spec.id);
            region::writeText (slot.canvas, spec.canvas);
            slot.layer.store (spec.layer, std::memory_order_relaxed);
            slot.order.store (spec.order, std::memory_order_relaxed);
            slot.source.store (static_cast<std::uint32_t> (region::sourceFrom (spec.source)), std::memory_order_relaxed);
            slot.paint.store (spec.paint, std::memory_order_relaxed);
            slot.blend.store (static_cast<std::uint32_t> (region::blendFrom (spec.blend)), std::memory_order_relaxed);
            region::writeText (slot.file, spec.file);
            slot.fit.store (static_cast<std::uint32_t> (region::fitFrom (spec.fit)), std::memory_order_relaxed);
            slot.scale.store (spec.scale, std::memory_order_relaxed);
            slot.offsetX.store (spec.offsetX, std::memory_order_relaxed);
            slot.offsetY.store (spec.offsetY, std::memory_order_relaxed);
            slot.rotation.store (spec.rotation, std::memory_order_relaxed);
            slot.flipH.store (spec.flipH ? 1u : 0u, std::memory_order_relaxed);
            slot.flipV.store (spec.flipV ? 1u : 0u, std::memory_order_relaxed);
            slot.contrast.store (spec.grade.contrast, std::memory_order_relaxed);
            slot.saturation.store (spec.grade.saturation, std::memory_order_relaxed);
            slot.gamma.store (spec.grade.gamma, std::memory_order_relaxed);
            slot.hue.store (spec.grade.hue, std::memory_order_relaxed);
            slot.hasCurves.store (spec.grade.hasCurves ? 1u : 0u, std::memory_order_relaxed);

            for (std::size_t channel = 0; channel < 3; ++channel)
                std::memcpy (slot.tables[channel], spec.grade.tables[channel].data(), 256);

            slot.shapeCount.store (spec.shape.count, std::memory_order_relaxed);
            slot.shapeFeather.store (spec.shape.feather, std::memory_order_relaxed);
            slot.shapeInvert.store (spec.shape.invert ? 1u : 0u, std::memory_order_relaxed);
            std::memcpy (slot.shapeX, spec.shape.x, sizeof (slot.shapeX));
            std::memcpy (slot.shapeY, spec.shape.y, sizeof (slot.shapeY));
            for (auto& ring : slot.rings)
                ring.written.store (0, std::memory_order_relaxed);
            slot.removeAt.store (region::notRemoved, std::memory_order_relaxed);
            region::endWrite (slot.seq);

            ids[static_cast<std::size_t> (at)] = spec.id;
            removing[static_cast<std::size_t> (at)] = region::notRemoved;
        }

        void move (const std::string& id, Property property, const Point& point) override
        {
            const auto at = slotOf (id);

            if (at < 0)
                return;

            auto& ring = r.layers[static_cast<std::size_t> (at)].rings[static_cast<std::size_t> (property)];
            const auto written = ring.written.load (std::memory_order_relaxed);
            auto& into = ring.points[written & (region::pointsPerLayer - 1)];

            into.sample.store (point.sample, std::memory_order_relaxed);
            into.value.store (point.value, std::memory_order_relaxed);
            ring.written.store (written + 1, std::memory_order_release);
        }

        void remove (const std::string& id, std::int64_t sample) override
        {
            const auto at = slotOf (id);

            if (at < 0)
                return;

            if (sample < 0)
            {
                letGo (at);
                return;
            }

            r.layers[static_cast<std::size_t> (at)].removeAt.store (sample, std::memory_order_release);
            removing[static_cast<std::size_t> (at)] = sample;
        }

        void clear() override
        {
            for (int at = 0; at < region::maxLayers; ++at)
                if (! ids[static_cast<std::size_t> (at)].empty())
                    letGo (at);
        }

        void prepare (const std::vector<std::string>& paths) override
        {
            region::writePrepared (r, paths);
        }

        /*  EVERY SLOT WHOSE REMOVAL THE CLOCK HAS PASSED, by `margin`, freed.
            Tick thread, once a tick, with the clock the points are placed on. */
        void release (std::int64_t sampleNow, std::int64_t margin) noexcept
        {
            for (int at = 0; at < region::maxLayers; ++at)
            {
                const auto due = removing[static_cast<std::size_t> (at)];

                if (due != region::notRemoved && sampleNow > due + margin)
                    letGo (at);
            }
        }

        /** How many layers are held, freed or not yet: a test's question. */
        int held() const noexcept
        {
            int count = 0;

            for (const auto& id : ids)
                if (! id.empty())
                    ++count;

            return count;
        }

    private:
        int slotOf (const std::string& id) const noexcept
        {
            for (int at = 0; at < region::maxLayers; ++at)
                if (ids[static_cast<std::size_t> (at)] == id)
                    return at;

            return -1;
        }

        int freeSlot() const noexcept
        {
            for (int at = 0; at < region::maxLayers; ++at)
                if (ids[static_cast<std::size_t> (at)].empty())
                    return at;

            return -1;
        }

        void letGo (int at) noexcept
        {
            auto& slot = r.layers[static_cast<std::size_t> (at)];

            region::beginWrite (slot.seq);
            slot.used.store (0, std::memory_order_relaxed);
            slot.removeAt.store (region::notRemoved, std::memory_order_relaxed);
            region::endWrite (slot.seq);

            ids[static_cast<std::size_t> (at)].clear();
            removing[static_cast<std::size_t> (at)] = region::notRemoved;
        }

        region::Region& r;
        std::array<std::string, region::maxLayers> ids;
        std::array<std::int64_t, region::maxLayers> removing {};
    };
}
