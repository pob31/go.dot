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
    AN OSC CUE'S CURVES, RECORDED (namespace draft 45, O.9): which cue is armed
    for recording, which of its curves are, whether a pass is running, and what
    each armed curve is riding.

    A table of its own beside the faders' (`LaneTable`, §34) - ZD, mine: the
    faders' is about one media cue's level and mixes, and `lane-record.wfglog`
    stays as it is. The arithmetic after the samples - a wrap's new segment,
    thinning, the splice into the old curve - is the lanes' (`LaneRecording`).

    TONIGHT'S, NEVER THE SHOW'S (PRD §4.10): an arming and a pass are what the
    hands are doing; the curves a pass ends in are the decision, and those are
    written as one `node.setMany` when it ends - one step of undo (ZL). Moved by
    named commands (`curve.*`, logged and replayed) and by the Runner's hook,
    published under `/godot/curves/` and each curve's `ride`.

    Tick thread only, like every table the Runner and the tree share.
*/

#include <wfg/engine/cue/LaneRecording.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wfg::cue
{
    class CurveTable
    {
    public:
        /** The OSC cue armed for recording, or empty. */
        const std::string& cue() const noexcept { return armedCue; }
        bool armed() const noexcept             { return ! armedCue.empty(); }

        /*  Arms a cue. Another cue starts with no curve armed; the same cue again
            keeps its choices. */
        void arm (const std::string& cueId)
        {
            if (cueId != armedCue)
            {
                curves.clear();
                rides.clear();
            }

            armedCue = cueId;
            clearPass();
        }

        void free()
        {
            armedCue.clear();
            curves.clear();
            rides.clear();
            clearPass();
        }

        bool isArmed (const std::string& curveId) const { return curves.count (curveId) != 0; }

        /*  A CURVE'S REC, before a pass or during one: disarmed mid-pass, what it
            rode so far is kept, and a later arming starts afresh. */
        void setArmed (const std::string& curveId, bool on)
        {
            if (on)
            {
                curves.insert (curveId);
                return;
            }

            curves.erase (curveId);
            rides.erase (curveId);
        }

        const std::set<std::string>& armedCurves() const noexcept { return curves; }

        //==============================================================================
        /*  WHAT A CURVE RIDES: the value it is at - the device's report once one
            arrives in a pass, its own curve's otherwise - whether a report has
            latched it (ZL), what it has ridden so far, and the observation it
            last sampled, by the tick the device's account was taken on. */
        struct Ride
        {
            double value = 0.0;
            bool latched = false;

            /*  WHO MOVED IT LAST (ZJ): `heard` - the device, whose value Go.dot
                does not send while it records - or `hand`, a SpaceMouse (O.11),
                whose value it sends so the device follows. */
            std::string source;

            std::vector<RideSegment> segments;
            std::int64_t sampledTick = -1;
            std::int64_t lastTick = -1;
        };

        std::map<std::string, Ride> rides;
        Ride& rideOf (const std::string& curveId) { return rides[curveId]; }

        //==============================================================================
        /*  THE PASS (ZL): started by `curve.record` on the run it fired, asked to
            end by `curve.stop`, ended by the Runner once it has written the
            curves - or dropped, for a run killed under it. */
        bool recording = false;
        bool stopping = false;
        std::string run;

        /*  The tick the pass began on: a report taken before it is not one the
            pass heard, however late it is read. */
        std::int64_t startTick = -1;

        void startPass (const std::string& runId)
        {
            recording = true;
            stopping = false;
            run = runId;

            for (auto& [id, ride] : rides)
            {
                ride.latched = false;
                ride.segments.clear();
                ride.sampledTick = -1;
                ride.lastTick = -1;
            }
        }

        void endPass (const std::string& said)
        {
            lastPass = said;
            clearPass();
        }

        /*  WHAT THE LAST PASS ENDED IN, published as `/godot/curves/pass`: the
            tick, the cue, and `kept` with the points written, `untouched` for a
            pass nothing moved, `locked`, or `dropped`. */
        std::string lastPass;

    private:
        void clearPass()
        {
            recording = false;
            stopping = false;
            run.clear();
            startTick = -1;

            for (auto& [id, ride] : rides)
            {
                ride.latched = false;
                ride.segments.clear();
            }
        }

        std::string armedCue;
        std::set<std::string> curves;
    };
}
