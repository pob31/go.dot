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
    FUZZ TARGET: reading a QLab workspace and walking it into a plan
    (QlabReader.h, QlabWalk.h): bytes in, `readWorkspaceBytes`, and what reads
    is walked as the importer walks it - every list, then with every cue
    given a channel count and a picture size and every font found, so the
    branches the importer reaches only when a file was found on disk are
    reached too. The plist underneath is fuzz_bplist's; this goes through it
    with the structure QLab's reader expects, which is where its own checks
    start to matter.
*/

#include "FuzzTarget.h"

#include "../QlabFixture.h"

#include <wfg/engine/import/QlabReader.h>
#include <wfg/engine/import/QlabWalk.h>

#include <functional>
#include <string>

namespace qlab = wfg::import::qlab;

namespace
{
    void eachCue (const qlab::Cue& cue, const std::function<void (const qlab::Cue&)>& fn)
    {
        fn (cue);
        for (const auto& child : cue.children)
            eachCue (child, fn);
    }
}

extern "C" int LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size)
{
    const auto read = qlab::readWorkspaceBytes (data, size);

    if (! read.workspace)
    {
        if (read.error.empty())
            __builtin_trap();   // refused "in words", or not refused
        return 0;
    }

    const auto& workspace = *read.workspace;

    (void) qlab::walk (workspace);

    qlab::WalkOptions options;
    for (std::size_t i = 0; i < workspace.lists.size(); ++i)
    {
        options.lists.insert (static_cast<int> (i));
        eachCue (workspace.lists[i], [&options] (const qlab::Cue& cue)
        {
            options.channels[cue.id] = 2;
            options.pictureSizes[cue.id] = { 1920, 1080 };
        });
    }
    options.fontOf = [] (const std::string& postScript) -> std::optional<qlab::FontFace>
    {
        qlab::FontFace face;
        face.family = postScript;
        return face;
    };
    /*  ONLY AN ITEM'S LAST RANGE RUNS TO THE END (namespace draft §99): the
        slices before it end where the next begins. */
    for (const auto& list : qlab::walk (workspace, options).lists)
        for (const auto& item : list.items)
            for (std::size_t at = 0; at + 1 < item.ranges.size(); ++at)
                if (item.ranges[at].toEnd)
                    __builtin_trap();

    //  Hostile sizes and channel counts: a file QLab found that is nothing like the cue.
    for (auto& [id, count] : options.channels) count = 0;
    for (auto& [id, wh] : options.pictureSizes) wh = { 0, -1 };
    (void) qlab::walk (workspace, options);

    return 0;
}

namespace wfg::fuzz
{
    namespace fixture = wfg::test::qlab;
    using fixture::Value;

    std::vector<Bytes> seeds()
    {
        std::vector<Bytes> out;

        for (const int major : { 5, 4 })
        {
            fixture::Workspace w (major);
            const auto s21 = major >= 5 ? std::string ("PATCH-S21") : std::string ("3");
            const auto wfs = major >= 5 ? std::string ("PATCH-WFS") : std::string ("2");

            const auto unmute = w.network ("UnMute", "/channel/118/mute \\F", s21);
            const auto place = w.network ("Set input 21", "/wfs/input/21/positionXYZ -3.0 1.0 3.0", wfs);
            const auto doors = w.group ("Doors open", major >= 5 ? 6 : 1, { unmute, place });
            const auto sound = w.audio ("", "audio/a.WAV", { { 0, 0, -200.0 }, { 0, 1, 0.0 }, { 1, 0, -6.0 }, { 2, 2, -3.0 } },
                                        { { "rate", Value::number (0.55) }, { "startTime", Value::number (22.0) },
                                          { "endTime", Value::number (595.25) }, { "playCount", Value::integer (2) },
                                          { "infiniteLoop", Value::boolean (true) } });
            const auto fadeIn = w.fade ("", sound, 1.0, true, { { 0, 0, -30.0 } });
            const auto machinery = w.group ("Machinery", 3, { sound, fadeIn });
            const auto fader = w.networkFade ("Fader", "/channel/114/fader #v#", s21, -18.0, 0.0, 5.0);
            const auto script = w.cue ("ScriptCue", "a script", { { "source", Value::string ("tell application \"QLab\"") } });
            const auto memo = w.notes (w.memo ("TUNE"), "When house lights go down");
            const auto top = w.group ("1 - Doors", 1, { doors, machinery, fader, script, memo });
            out.push_back (w.bytes ({ w.group ("Main", 0, { top }) }));
        }

        return out;
    }
}
