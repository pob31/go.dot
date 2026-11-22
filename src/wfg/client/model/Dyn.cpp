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
#include <wfg/engine/cue/FadeMoves.h>
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
        out.live = wordsStartingWith (text (snapshot, rowAt ("live")), "dyn");

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

    namespace
    {
        //  Each number's place in the one value, by its row.
        float* fieldOf (audio::DynSettings& s, const std::string& row)
        {
            if (row == "dynThreshold")     return &s.thresholdDb;
            if (row == "dynRatio")         return &s.ratio;
            if (row == "dynAttack")        return &s.attackMs;
            if (row == "dynRelease")       return &s.releaseMs;
            if (row == "dynKnee")          return &s.kneeDb;
            if (row == "dynMakeup")        return &s.makeupDb;
            if (row == "dynGateThreshold") return &s.gateThresholdDb;
            if (row == "dynGateRange")     return &s.gateRangeDb;
            if (row == "dynGateAttack")    return &s.gateAttackMs;
            if (row == "dynGateHold")      return &s.gateHoldMs;
            if (row == "dynGateRelease")   return &s.gateReleaseMs;
            if (row == "dynKeyLowCut")     return &s.keyLowCutHz;
            if (row == "dynKeyHighCut")    return &s.keyHighCutHz;
            return nullptr;
        }
    }

    float dynNumberOf (const audio::DynSettings& settings, const std::string& row)
    {
        auto copy = settings;
        const auto* field = fieldOf (copy, row);
        return field != nullptr ? *field : 0.0f;
    }

    bool setDynNumber (audio::DynSettings& settings, const std::string& row, float value)
    {
        auto* field = fieldOf (settings, row);

        if (field == nullptr)
            return false;

        *field = value;
        return true;
    }

    DynReading readFadeDyn (const tree::TreeSnapshot& snapshot, const std::string& fadeId)
    {
        /*  THE TARGET'S STAGE WITH THE FADE'S NUMBERS OVER IT (§141, BIF), as
            the EQ panel draws a fade's: what it arrives at. Or the aux it moves
            (§93), which has a stage of its own. */
        const auto fade = "/godot/cue/" + fadeId + "/";
        const auto auxId = text (snapshot, fade + "aux");
        const auto targetId = ! auxId.empty() ? auxId : text (snapshot, fade + "target");

        if (! text (snapshot, fade + "dca").empty())
        {
            DynReading out;
            out.notice = "This fade moves a DCA, which has no dynamics.";
            return out;
        }

        if (targetId.empty())
        {
            DynReading out;
            out.notice = "This fade is aimed at nothing yet - pick its target in the inspector.";
            return out;
        }

        auto out = readDyn (snapshot, targetId);

        if (! out.present)
        {
            out.notice = "This fade's target has no dynamics: only an audio cue, a mic cue or an aux has them.";
            return out;
        }

        out.fadeId = fadeId;
        out.live.clear();

        for (const auto& [row, value] : cue::parseMoveList (text (snapshot, fade + "dyn")))
        {
            if (! cue::isMovableDynRow (row))
                continue;

            out.fadeMoves[row] = value;
            setDynNumber (out.settings, row, static_cast<float> (value));
        }

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
