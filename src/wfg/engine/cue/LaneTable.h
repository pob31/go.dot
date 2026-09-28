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
    A LANE BEING RECORDED FROM A FADER (namespace draft §20.9): which media
    cue's level lane, which strip was taken for it, whether a pass is running,
    and what the fader rides.

    TONIGHT'S, NEVER THE SHOW'S (PRD §4.10): a pick and a pass are what the
    hands are doing; the lane a pass ends in is the decision, and that is
    written to the document as one `node.set` when it ends. So this is a table
    beside the takes' and the DCAs', moved by named commands (`lane.*`, logged
    and replayed) and by the Runner's hook, and published under
    `/godot/surface/lane…` - never stored.

    ONE LANE AT A TIME, which is what makes the ride ONE node
    (`/godot/surface/laneRide`, decision DJ) rather than a row on every cue:
    the strip taken for it is pointed at that node, and the bridge and the
    virtual panel ride it the way they ride any strip's target.

    Tick thread only, like every table the Runner and the tree share.
*/

#include <string>

namespace wfg::cue
{
    class LaneTable
    {
    public:
        /** The media cue whose lane is armed or has a fader, or empty. */
        const std::string& cue() const noexcept     { return lane; }

        /** The strip taken for it, or empty while the lane waits for a touch. */
        const std::string& strip() const noexcept   { return fader; }

        /** Armed, with no fader yet: the next fader touched is taken (DF). */
        bool waiting() const noexcept               { return ! lane.empty() && fader.empty(); }

        /** A fader is taken: the lane can be recorded. */
        bool taken() const noexcept                 { return ! lane.empty() && ! fader.empty(); }

        //==============================================================================
        /*  Arms a cue's lane. A fader taken for another cue is let go, so the
            next touch takes one for this; the same cue again keeps its fader. */
        void arm (const std::string& cueId)
        {
            if (cueId != lane)
                fader.clear();

            lane = cueId;
            clearPass();
        }

        void take (const std::string& stripId)      { fader = stripId; }

        /** Lets the fader go and forgets the lane, which is also what `lane.arm ""` does. */
        void free()
        {
            lane.clear();
            fader.clear();
            clearPass();
        }

        //==============================================================================
        /*  THE PASS (decision DH, latch): started by `lane.record`, asked to end
            by `lane.stop`, ended by the Runner once it has written the lane - or
            dropped, for a run killed under it. `run` is the run the pass plays;
            `touched` latches at the first touch of the ride in the pass. */
        bool recording = false;
        bool stopping = false;
        std::string run;
        bool touched = false;

        /*  THE HAND'S LEVEL, as the last `node.set` on the ride left it - held
            after the hand lets go, which is the latch. Written by the live
            door; read by the Runner only once `touched`. */
        double handDb = 0.0;

        /*  Whether a hand has written the ride since the pass began. A touch
            with no move yet has written nothing, and the latch then starts
            from where the fader was - the ride's own value - not from a level
            left over from another pass. */
        bool handSeen = false;

        /*  WHAT THE RIDE NODE READS: the lane where the file is (where it
            starts, outside a pass) until the first touch, the hand's from then
            on. Set by the Runner's hook each tick, published by the tree - the
            value a motor or the panel follows while nobody holds it. */
        double rideDb = 0.0;

        void startPass (const std::string& runId)
        {
            clearPass();
            recording = true;
            run = runId;
        }

        void clearPass()
        {
            recording = false;
            stopping = false;
            run.clear();
            touched = false;
            handSeen = false;
        }

    private:
        std::string lane;
        std::string fader;
    };
}
