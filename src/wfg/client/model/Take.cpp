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

#include <wfg/client/model/Take.h>

#include <wfg/client/model/Rack.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace wfg::client::model
{
    namespace
    {
        /*  A number off the tree's own text, never a locale question - and
            never a throw on a malformed reading. `reading` rather than `text`
            for OutputList's reason: GCC's -Wshadow. */
        double numberOf (const std::string& reading, double fallback)
        {
            return osc::parseDouble (reading).value_or (fallback);
        }

        std::string layersWords (int count)
        {
            return std::to_string (count) + (count == 1 ? " layer" : " layers");
        }
    }

    TakeReading readTake (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        TakeReading reading;
        reading.cueId = cueId;

        const auto base = "/godot/cue/" + cueId + "/";

        if (text (snapshot, base + "kind") != "mic")
        {
            reading.notice = "Only a mic cue on a sampling channel has a take.";
            return reading;
        }

        reading.channelId = text (snapshot, base + "channel");

        if (reading.channelId.empty())
        {
            reading.notice = "This mic cue plays through no rack channel, so there is no take to show.";
            return reading;
        }

        const auto slot = "/godot/slot/" + reading.channelId + "/";
        reading.channelName = text (snapshot, slot + "name");

        if (reading.channelName.empty())
            reading.channelName = reading.channelId;

        reading.capacity = numberOf (text (snapshot, slot + "takeSeconds"), 0.0);

        if (reading.capacity <= 0.0)
        {
            reading.notice = reading.channelName + " records nothing: give it a longest take in the"
                             " Rack tab, then Load now.";
            return reading;
        }

        reading.present = true;

        if (const auto word = text (snapshot, slot + "take"); ! word.empty())
            reading.state = word;

        reading.length = numberOf (text (snapshot, slot + "takeLength"), 0.0);
        reading.layers = static_cast<int> (numberOf (text (snapshot, slot + "takeLayers"), 0.0));
        reading.maxLayers = static_cast<int> (numberOf (text (snapshot, slot + "layers"), 4.0));
        reading.loopIn = numberOf (text (snapshot, slot + "loopIn"), 0.0);
        reading.loopOut = numberOf (text (snapshot, slot + "loopOut"), 0.0);
        reading.playhead = numberOf (text (snapshot, slot + "playhead"), 0.0);
        reading.problem = text (snapshot, slot + "takeProblem");

        /*  WHO HOLDS THE CHANNEL, and whether what holds it is a mic cue
            sounding - the engine's own test for Rec, Loop and a layer. */
        reading.holderRun = text (snapshot, slot + "holder");

        if (! reading.holderRun.empty())
        {
            const auto run = "/godot/run/" + reading.holderRun + "/";
            const auto holderCue = text (snapshot, run + "cue");

            reading.holderName = text (snapshot, "/godot/cue/" + holderCue + "/name");
            reading.channelSounds = text (snapshot, run + "kind") == "mic"
                                      && text (snapshot, run + "state") == "playing";
        }

        if (const auto word = text (snapshot, base + "onGo"); ! word.empty())
            reading.onGo = word;

        reading.through = isYes (flag (snapshot, base + "through"));
        reading.keeping = isYes (flag (snapshot, slot + "keeping"));
        reading.kept = text (snapshot, slot + "kept");
        reading.locked = isYes (flag (snapshot, "/godot/document/locked"));
        return reading;
    }

    std::string takePanelWords (const TakeReading& take)
    {
        if (! take.present)
            return take.notice;

        std::string said;

        if (take.state == "empty")
            said = "Empty: Rec records up to " + secondsWords (take.capacity) + ".";
        else if (take.state == "recording")
            said = "Recording, up to " + secondsWords (take.capacity) + ": Rec again loops it.";
        else if (take.state == "looping")
            said = "Looping " + tenthsOfSeconds (take.length) + ", "
                   + std::to_string (take.layers) + " of " + layersWords (take.maxLayers) + " on it.";
        else if (take.state == "overdubbing")
            said = "Laying a layer on " + tenthsOfSeconds (take.length) + ": Rec again closes it.";
        else if (take.state == "held")
            said = "Held: " + tenthsOfSeconds (take.length) + ", silent until Rec or Loop.";
        else
            said = take.state;

        /*  EVERY LAYER IN USE - §19.3's sentence, which the engine's refusal
            of the next Rec does not say anywhere a hand would see. */
        if (take.layersFull() && (take.state == "looping" || take.state == "held"))
            said += " " + take.channelName + " holds its " + layersWords (take.maxLayers) + ": Undo one or Clear.";

        /*  AND WHAT KEEP DID: writing now, or the file it wrote last. */
        if (take.keeping)
            said += " Keeping it as a file...";
        else if (! take.kept.empty())
            said += " Kept as " + take.kept + ".";

        if (! take.problem.empty())
            said += " " + take.problem + (take.problem.back() == '.' ? "" : ".");

        return said;
    }

    std::string takePressWhy (const TakeReading& take)
    {
        if (! take.present || take.channelSounds)
            return {};

        if (take.holderRun.empty())
            return "Nothing sounds on " + take.channelName + ": GO a mic cue on it to record.";

        return (take.holderName.empty() ? std::string ("The cue holding it") : take.holderName)
               + " holds " + take.channelName + " and is not sounding yet.";
    }

    std::string loopPointAddress (const std::string& channelId, bool inPoint)
    {
        return "/godot/slot/" + channelId + (inPoint ? "/loopIn" : "/loopOut");
    }

    std::string tenthsOfSeconds (double seconds)
    {
        const auto tenths = std::llround (std::max (0.0, seconds) * 10.0);
        return std::to_string (tenths / 10) + "." + std::to_string (tenths % 10) + " s";
    }
}
