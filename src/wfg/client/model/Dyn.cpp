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

#include <wfg/client/model/Dyn.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <string>
#include <vector>

namespace wfg::client::model
{
    DynReading readDyn (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        DynReading out;

        if (cueId.empty())
        {
            out.notice = "Nothing is picked.";
            return out;
        }

        out.base = soundBase (snapshot, cueId);
        const auto rowAt = [&out] (const std::string& row) { return out.base + row; };

        /*  THE ROWS EXIST ON A SOUND ONLY, and `dynOn` is asked first, as the
            EQ asks `eqOn`: no such node, no stage. */
        const auto on = flag (snapshot, rowAt ("dynOn"));

        if (on == Flag::unsaid)
        {
            out.notice = "Only an audio cue, a mic cue or an aux has dynamics.";
            return out;
        }

        out.present = true;

        const auto number = [&] (const char* row, float fallback)
        {
            const auto value = osc::parseDouble (text (snapshot, rowAt (row)));
            return value.has_value() ? static_cast<float> (*value) : fallback;
        };

        auto& s = out.settings;
        s.on = on == Flag::yes;
        s.rms = text (snapshot, rowAt ("dynDetector")) == "rms";
        s.compOn = flag (snapshot, rowAt ("dynCompOn")) != Flag::no;
        s.thresholdDb = number ("dynThreshold", s.thresholdDb);
        s.ratio = number ("dynRatio", s.ratio);
        s.attackMs = number ("dynAttack", s.attackMs);
        s.releaseMs = number ("dynRelease", s.releaseMs);
        s.kneeDb = number ("dynKnee", s.kneeDb);
        s.makeupDb = number ("dynMakeup", s.makeupDb);
        s.gateOn = flag (snapshot, rowAt ("dynGateOn")) == Flag::yes;
        s.gateThresholdDb = number ("dynGateThreshold", s.gateThresholdDb);
        s.gateRangeDb = number ("dynGateRange", s.gateRangeDb);
        s.gateAttackMs = number ("dynGateAttack", s.gateAttackMs);
        s.gateHoldMs = number ("dynGateHold", s.gateHoldMs);
        s.gateReleaseMs = number ("dynGateRelease", s.gateReleaseMs);
        s.keyLowCutHz = number ("dynKeyLowCut", s.keyLowCutHz);
        s.keyHighCutHz = number ("dynKeyHighCut", s.keyHighCutHz);

        /*  WHAT IT MAY LISTEN TO (§141): with no key, the cue itself before its
            level (BHU) - which is why the empty word reads so here, where an
            insert's reads "no key". */
        out.key = text (snapshot, rowAt ("dynKey"));
        out.keyChoices.push_back ({ "", "this cue, before its level" });
        out.keyChoices.push_back ({ "self", "this cue, after its level" });

        for (auto& choice : keySources (snapshot, cueId))
            out.keyChoices.push_back (std::move (choice));

        return out;
    }

    std::string dynKeyWords (const DynReading& reading, const std::string& key)
    {
        return keyWords (reading.keyChoices, key);
    }

    std::string dynRowAt (const DynReading& reading, const std::string& row)
    {
        return reading.base + row;
    }
}
