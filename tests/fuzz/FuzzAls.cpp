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

    AND SINCE THE AUXES (namespace draft §93): a third-party plugin's state is
    read out of hex the set holds, so it can never be longer than the text it
    came from; and the walk's returns become auxes with sends of their own, so
    every send the walk makes - a sound's, an aux's - is aimed at a mix the walk
    made, at a level that is a number.

    Seeds are the hand-written fixtures in tests/fixtures/als, found through
    WFG_FUZZ_FIXTURES, which CMake sets - and the session set again with a VST3,
    a VST2 and an AU on its return, as AlsReaderTests writes them.
*/

#include "FuzzTarget.h"

#include <wfg/engine/import/AlsReader.h>
#include <wfg/engine/import/AlsWalk.h>

#include <algorithm>
#include <cmath>
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

    //  A plugin's state is hex in the text: never more bytes than the text has.
    const auto plugins = [size] (const als::Track& track)
    {
        for (const auto& device : track.devices)
            if (device.plugin && device.plugin->state.size() + device.plugin->controllerState.size() > size)
                __builtin_trap();
    };

    for (const auto& t : set.tracks)  plugins (t);
    for (const auto& t : set.returns) plugins (t);
    if (set.master) plugins (*set.master);

    /*  EVERY SEND AIMED AT A MIX THE WALK MADE, at a level that is a number -
        a sound's, and an aux's own into the other returns (§93). */
    const auto whole = als::walk (set);

    const auto aimed = [&whole] (const als::SendOut& send)
    {
        const auto known = std::any_of (whole.mixes.begin(), whole.mixes.end(),
                                        [&send] (const als::Mix& mix) { return mix.key == send.mix; });

        if (! known || ! std::isfinite (send.levelDb))
            __builtin_trap();

        for (const auto& point : send.lane)
            if (! std::isfinite (point.db))
                __builtin_trap();
    };

    for (const auto& sound : whole.sounds)
        for (const auto& send : sound.sends)
            aimed (send);

    for (const auto& mix : whole.mixes)
    {
        for (const auto& send : mix.sends)
            aimed (send);

        for (const auto* device : mix.plugins)
            if (device == nullptr || ! device->plugin)
                __builtin_trap();
    }

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

        /*  THE SESSION SET WITH THIRD-PARTY PLUGINS before its Reverb (§93), so
            the mutations reach the plugin readers and an aux carrying them. */
        if (! out.empty())
        {
            std::string text (out.front().begin(), out.front().end());
            const auto at = text.find ("<Reverb Id=\"1\">");

            if (at != std::string::npos)
            {
                text.insert (at,
                    "<PluginDevice Id=\"7\"><On><Manual Value=\"true\" /></On><PluginDesc><Vst3PluginInfo Id=\"0\">"
                    "<Preset><Vst3Preset Id=\"1\"><ProcessorState>0A0B0C\n0D</ProcessorState>"
                    "<ControllerState>FF01</ControllerState></Vst3Preset></Preset><Name Value=\"Pro-R\" />"
                    "<Uid><Fields.0 Value=\"1447773007\" /><Fields.1 Value=\"-1\" /><Fields.2 Value=\"0\" />"
                    "<Fields.3 Value=\"16\" /></Uid></Vst3PluginInfo></PluginDesc></PluginDevice>"
                    "<PluginDevice Id=\"8\"><PluginDesc><VstPluginInfo Id=\"0\"><Path Value=\"C:/VST/Room.dll\" />"
                    "<PlugName Value=\"Room\" /><UniqueId Value=\"1919251788\" /><Preset><VstPreset Id=\"1\">"
                    "<Buffer>CAFE</Buffer></VstPreset></Preset></VstPluginInfo></PluginDesc></PluginDevice>"
                    "<AuPluginDevice Id=\"9\"><PluginDesc><AuPluginInfo Id=\"0\"><ComponentType Value=\"1635083896\" />"
                    "<ComponentSubType Value=\"1953653099\" /><ComponentManufacturer Value=\"1634758764\" />"
                    "<Name Value=\"AUMatrixReverb\" /><Preset><AuPreset Id=\"1\"><Buffer>3C3F</Buffer></AuPreset>"
                    "</Preset></AuPluginInfo></PluginDesc></AuPluginDevice>");
                out.emplace_back (text.begin(), text.end());
            }
        }
#endif
        return out;
    }
}
