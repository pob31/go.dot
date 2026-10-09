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
    A PICTURE SIDE OF THE TESTS' OWN that writes down what it is told (Phase 8a,
    namespace draft 35.4): the layers shown and restated, every point of every
    moving value by layer, what is read ahead (§48) and in what order beside the
    layers, what is removed and when, the double Esc's clears, the canvases'
    levels. VideoTests' first, shared since §49 with the sampler's.
*/

#include <wfg/engine/video/VideoSink.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wfg::testing
{
    struct FakeVideoSink final : video::Sink
    {
        void show (const video::LayerSpec& spec) override  { shown.push_back (spec); events.push_back ("show " + spec.file); }
        void restate (const video::LayerSpec& spec) override  { restated.push_back (spec); }

        /*  THE OPACITY'S POINTS by layer, and every other value's by layer and
            property: what a fade of the geometry is checked against. */
        void move (const std::string& id, video::Property property, const video::Point& point) override
        {
            if (property == video::Property::opacity)
                points[id].push_back (point);
            else
                geometry[id][property].push_back (point);
        }

        /*  WHAT IS READ AHEAD (namespace draft §48): the last list, every list
            sent, and - in `events`, with the layers shown - the order the two
            arrive in, which is what keeps a picture GO has just shown held. */
        void prepare (const std::vector<video::Preload>& items) override
        {
            preloads = items;
            preloadsSent.push_back (items);
            prepared.clear();

            for (const auto& item : items)
                prepared.push_back (item.path);

            events.push_back ("prepare");
        }

        void remove (const std::string& id, std::int64_t sample) override
        {
            removed.push_back ({ id, sample });
        }

        void clear() override  { ++clears; }

        void canvasLevels (const std::vector<std::pair<std::string, double>>& levels) override
        {
            canvasLevelsSent.push_back (levels);
        }

        /*  The layer shown for a run, or null. */
        const video::LayerSpec* shownFor (const std::string& runId) const
        {
            for (const auto& spec : shown)
                if (spec.id == runId)
                    return &spec;

            return nullptr;
        }

        std::vector<std::vector<std::pair<std::string, double>>> canvasLevelsSent;
        std::vector<video::LayerSpec> shown;
        std::vector<video::LayerSpec> restated;
        std::map<std::string, std::vector<video::Point>> points;
        std::map<std::string, std::map<video::Property, std::vector<video::Point>>> geometry;
        std::vector<std::string> prepared;
        std::vector<video::Preload> preloads;
        std::vector<std::vector<video::Preload>> preloadsSent;
        std::vector<std::string> events;
        std::vector<std::pair<std::string, std::int64_t>> removed;
        int clears = 0;
    };
}
