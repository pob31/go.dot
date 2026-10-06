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
    THE FADERS FLIPPED TO ONE CUE (namespace draft §34, which replaced §20.9's
    one fader taken by touch): which media cue every fader surface shows, which
    of its lanes are armed by their strip's REC, whether a pass is running, and
    what each fader rides.

    A LANE IS NAMED BY ITS KEY: `level` for the cue's own level lane, or the
    identifier of a mix for the cue's send into it - whether or not the cue
    sends there yet (UQ). Identifiers are Crockford base32 and never spell
    `level`. Strip k of a flipped surface is lane k (UN): `flippedLanes` in
    LaneCommands says which they are.

    TONIGHT'S, NEVER THE SHOW'S (PRD §4.10): a flip, the REC choices and a pass
    are what the hands are doing; the lanes a pass ends in are the decision,
    and those are written to the document as one `lane.write` when it ends. So
    this is a table beside the takes' and the DCAs', moved by named commands
    (`lane.*`, logged and replayed) and by the Runner's hook, and published
    under `/godot/surface/lane…` and `/godot/bus/<mix>/laneRide` - never stored.

    Tick thread only, like every table the Runner and the tree share.
*/

#include <map>
#include <set>
#include <string>

namespace wfg::cue
{
    /** The key of a cue's own level lane; every other key is a mix's identifier. */
    inline constexpr const char* levelLaneKey = "level";

    class LaneTable
    {
    public:
        /** The media cue the faders are flipped to, or empty. */
        const std::string& cue() const noexcept     { return lane; }

        /** The faders are flipped to a cue: its lanes can be armed and recorded. */
        bool flipped() const noexcept               { return ! lane.empty(); }

        //==============================================================================
        /*  Flips the faders to a cue. Another cue starts with nothing armed;
            the same cue again keeps its REC choices. */
        void flip (const std::string& cueId)
        {
            if (cueId != lane)
            {
                armed.clear();
                rides.clear();
            }

            lane = cueId;
            clearPass();
        }

        /** Flips the faders back and forgets the cue, which is also what `lane.arm ""` does. */
        void free()
        {
            lane.clear();
            armed.clear();
            rides.clear();
            clearPass();
        }

        bool isArmed (const std::string& key) const { return armed.count (key) != 0; }

        /*  A STRIP'S REC (UI), before a pass or during one (UL). Disarmed, its
            fader is let go of: a touch later in the pass starts afresh, from
            where the fader is (UP). */
        void setArmed (const std::string& key, bool on)
        {
            if (on)
            {
                armed.insert (key);
                return;
            }

            armed.erase (key);

            if (const auto found = rides.find (key); found != rides.end())
            {
                found->second.touched = false;
                found->second.handSeen = false;
            }
        }

        const std::set<std::string>& armedKeys() const noexcept { return armed; }

        /*  THE PASS IS OVER, HOWEVER IT ENDED - and the faders STAY FLIPPED,
            with their REC choices (2026-10-06, UM, the author's decision): one
            pass after another rides one lane after another. That replaces, for
            the flip, QX's "the end gives the fader back". `said` is what the
            pass ended in, kept for the window to say (`lastPass`). */
        void endPass (const std::string& said)
        {
            lastPass = said;
            clearPass();
        }

        /*  WHAT THE LAST PASS ENDED IN, published as `/godot/surface/lanePass`
            so nothing ends in silence (namespace draft §30.4, §34): the tick it
            ended on, its cue, and `kept` with the points it wrote, the seconds
            they span and the lanes, `untouched` for a pass nobody rode,
            `locked` for one the lock kept from being written, or `dropped` for
            one a kill took. Empty until a pass has ended; never cleared by
            `free`, since the flip ending is exactly when it is read. */
        std::string lastPass;

        //==============================================================================
        /*  ONE LANE'S FADER. `rideDb` is what the ride node reads - the number
            as it is heard (UK): the written level or send plus its lane, where
            the file is (where it starts, outside a pass), until the lane is
            armed and touched in a pass, and the hand's from then on. Set by
            the Runner's hook each tick, published by the tree - the value a
            motor or the panel follows while nobody holds it. */
        struct Ride
        {
            double rideDb = 0.0;

            /*  THE HAND'S LEVEL, as the last `node.set` on the ride left it -
                held after the hand lets go, which is the latch. Written by the
                live door; read by the Runner only once `touched`. */
            double handDb = 0.0;

            /*  Whether a hand has written the ride since the pass began, or
                since the lane was armed again. A touch with no move yet has
                written nothing, and the latch then starts from where the fader
                was - the ride's own value. */
            bool handSeen = false;

            /** Latched at the lane's first touch while it is armed in a pass (DH). */
            bool touched = false;
        };

        /** By key, every lane of the flipped cue the Runner has read. */
        std::map<std::string, Ride> rides;

        Ride& rideOf (const std::string& key) { return rides[key]; }

        //==============================================================================
        /*  THE PASS (decision DH, latch): started by `lane.record`, asked to end
            by `lane.stop`, ended by the Runner once it has written the lanes -
            or dropped, for a run killed under it. `run` is the run the pass
            plays. */
        bool recording = false;
        bool stopping = false;
        std::string run;

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

            for (auto& [key, ride] : rides)
            {
                ride.touched = false;
                ride.handSeen = false;
            }
        }

    private:
        std::string lane;
        std::set<std::string> armed;
    };
}
