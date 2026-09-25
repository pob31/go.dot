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

#include <wfg/client/model/Eq.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/audio/EqMath.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace wfg::client::model
{
    std::string eqAddress (const std::string& cueId, const std::string& row)
    {
        return "/godot/cue/" + cueId + "/" + row;
    }

    std::string eqBandRow (int band, const char* suffix)
    {
        return "eqB" + std::to_string (band + 1) + suffix;
    }

    const char* eqShapeWord (audio::EqSettings::Shape shape)
    {
        switch (shape)
        {
            case audio::EqSettings::Shape::lowShelf:  return "lowShelf";
            case audio::EqSettings::Shape::highShelf: return "highShelf";
            case audio::EqSettings::Shape::peak:      break;
        }

        return "peak";
    }

    audio::EqSettings::Shape eqShapeFor (const std::string& word)
    {
        if (word == "lowShelf")  return audio::EqSettings::Shape::lowShelf;
        if (word == "highShelf") return audio::EqSettings::Shape::highShelf;

        return audio::EqSettings::Shape::peak;
    }

    EqReading readEq (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        EqReading out;

        if (cueId.empty())
            return out;

        /*  THE ROWS EXIST ONLY ON A MEDIA CUE, and `eqOn` is the one asked
            first: a cue with no such node has no EQ, whatever its kind word
            says, and the rest are read against the defaults the table gives
            them - which is what a flat EQ is, and what a saved one that was
            never touched reads as. */
        const auto on = flag (snapshot, eqAddress (cueId, "eqOn"));

        if (on == Flag::unsaid)
        {
            out.notice = "Only a media cue has an EQ.";
            return out;
        }

        out.present = true;

        const auto number = [&] (const std::string& row, float fallback)
        {
            const auto reading = text (snapshot, eqAddress (cueId, row));
            const auto value = osc::parseDouble (reading);
            return value.has_value() ? static_cast<float> (*value) : fallback;
        };

        const auto isYes = [&] (const std::string& row)
        {
            return flag (snapshot, eqAddress (cueId, row)) == Flag::yes;
        };

        auto& s = out.settings;
        s.on = on == Flag::yes;
        s.hpf = isYes ("eqHpf");
        s.lpf = isYes ("eqLpf");
        s.hpfFreq = number ("eqHpfFreq", s.hpfFreq);
        s.lpfFreq = number ("eqLpfFreq", s.lpfFreq);

        for (int band = 0; band < audio::EqSettings::numBands; ++band)
        {
            auto& b = s.band[band];
            b.freq = number (eqBandRow (band, "Freq"), b.freq);
            b.gain = number (eqBandRow (band, "Gain"), b.gain);
            b.q = number (eqBandRow (band, "Q"), b.q);

            /*  A BAND'S SWITCH is on unless the tree says off: a switch it
                has not published is the default, which is in. */
            b.on = flag (snapshot, eqAddress (cueId, eqBandRow (band, "On"))) != Flag::no;
        }

        s.band[0].shape = eqShapeFor (text (snapshot, eqAddress (cueId, "eqB1Shape")));
        s.band[3].shape = eqShapeFor (text (snapshot, eqAddress (cueId, "eqB4Shape")));

        out.live = text (snapshot, eqAddress (cueId, "live"));

        return out;
    }

    int eqHandleForAddress (const std::string& cueId, const std::string& address)
    {
        const auto base = eqAddress (cueId, "eq");

        if (cueId.empty() || address.rfind (base, 0) != 0)
            return -1;

        const auto row = address.substr (base.size() - 2);

        if (row.rfind ("eqHpf", 0) == 0)
            return 4;

        if (row.rfind ("eqLpf", 0) == 0)
            return 5;

        //  eqB<n>...: the band's own number, from one.
        if (row.size() > 3 && row.rfind ("eqB", 0) == 0 && row[3] >= '1' && row[3] <= '4')
            return row[3] - '1';

        return -1;
    }

    std::vector<EqPoint> eqCurve (const audio::EqSettings& settings, double sampleRate, int points)
    {
        std::vector<EqPoint> out;

        if (points < 2)
            return out;

        out.reserve (static_cast<std::size_t> (points));

        /*  EVENLY IN OCTAVES, because that is how a frequency axis is read:
            twenty to twenty thousand is ten octaves, and a point every tenth
            of one is what makes a narrow notch show at all. */
        const auto low = std::log2 (20.0);
        const auto high = std::log2 (20000.0);

        for (int i = 0; i < points; ++i)
        {
            const auto fraction = static_cast<double> (i) / static_cast<double> (points - 1);
            const auto frequency = std::exp2 (low + (high - low) * fraction);

            out.push_back ({ frequency,
                             audio::eqmath::responseDb (settings, frequency, sampleRate) });
        }

        return out;
    }

    double pinchedQ (double fromQ, double fromDistance, double distance)
    {
        if (! (fromDistance > 0.0) || ! (distance > 0.0))
            return std::clamp (fromQ, eqQLowest, eqQHighest);

        return std::clamp (fromQ * fromDistance / distance, eqQLowest, eqQHighest);
    }

    double turnedQ (double q, double wheel, bool pinch, bool fine)
    {
        const auto step = fine ? 1.01 : 1.1;
        const auto tenths = (pinch ? -wheel : wheel) * 10.0;

        return std::clamp (q * std::pow (step, tenths), eqQLowest, eqQHighest);
    }

    double magnifiedQ (double q, double scale)
    {
        if (! (scale > 0.0))
            return std::clamp (q, eqQLowest, eqQHighest);

        return std::clamp (q / scale, eqQLowest, eqQHighest);
    }
}
