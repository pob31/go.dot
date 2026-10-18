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

/*
    FUZZ TARGET: reading an Ableton Live set and walking it into the show it
    plays (AlsReader.h, AlsWalk.h): the set's XML in, `readSetXml`, and what
    reads is walked as the importer walks it - every scene, then each scene on
    its own, with the warp helpers asked of every clip. (The gzip layer is
    JUCE's and is not what is fuzzed: the XML under it is.)

    Seeds are the hand-written fixtures in tests/fixtures/als, found through
    WFG_FUZZ_FIXTURES, which CMake sets.
*/

#include "FuzzTarget.h"

#include <wfg/engine/import/AlsReader.h>
#include <wfg/engine/import/AlsWalk.h>

#include <fstream>
#include <iterator>
#include <string>

namespace als = wfg::import::als;

extern "C" int LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size)
{
    const auto read = als::readSetXml (std::string (reinterpret_cast<const char*> (data), size));

    if (! read.set)
    {
        if (read.error.empty())
            __builtin_trap();   // "why there is no set at all", or there is one
        return 0;
    }

    const auto& set = *read.set;

    (void) als::walk (set);

    for (std::size_t i = 0; i < set.scenes.size(); ++i)
    {
        const auto scene = static_cast<int> (i);
        (void) als::sceneDoesSomething (set, scene);

        als::WalkOptions only;
        only.scenes.insert (scene);
        (void) als::walk (set, only);
    }

    als::WalkOptions outOfRange;
    outOfRange.scenes = { -1, static_cast<int> (set.scenes.size()), 1 << 30 };
    (void) als::walk (set, outOfRange);
    (void) als::sceneDoesSomething (set, -1);
    (void) als::sceneDoesSomething (set, 1 << 30);

    const auto clips = [&set] (const als::Track& track, auto&& fn) { for (const auto& slot : track.slots) if (slot.clip) fn (*slot.clip); };
    const auto each = [&] (auto&& fn)
    {
        for (const auto& t : set.tracks)  clips (t, fn);
        for (const auto& t : set.returns) clips (t, fn);
        if (set.master) clips (*set.master, fn);
    };

    each ([&set] (const als::Clip& clip)
    {
        for (const double beats : { -1.0, 0.0, 0.5, 1e9 })
            (void) als::sampleSecondsAt (clip, beats, set.tempo);
        (void) als::sampleSecondsAt (clip, 1.0, 0.0);
        (void) als::beatsPerSecond (clip);
        (void) als::warpIsStraight (clip);
    });

    return 0;
}

namespace wfg::fuzz
{
    std::vector<Bytes> seeds()
    {
        std::vector<Bytes> out;
#ifdef WFG_FUZZ_FIXTURES
        for (const auto* name : { "session.als.xml", "live10.als.xml" })
        {
            std::ifstream in (std::string (WFG_FUZZ_FIXTURES) + "/als/" + name, std::ios::binary);
            Bytes bytes ((std::istreambuf_iterator<char> (in)), {});
            if (! bytes.empty())
                out.push_back (std::move (bytes));
        }
#endif
        return out;
    }
}
