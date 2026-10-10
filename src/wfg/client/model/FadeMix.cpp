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

#include <wfg/client/model/FadeMix.h>

#include <wfg/client/model/Fx.h>
#include <wfg/client/model/Sends.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/cue/FadeMoves.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wfg::client::model
{
    namespace
    {
        /*  The lowest speed above nought on the throw: varispeed's lowest
            semitone step, 2^(-52/12), so the throw and the dial agree. */
        constexpr double lowestStep = 0.04958;
        constexpr double highestSpeed = 20.0;

        /*  A FIXED NUMBER OF DECIMALS WITH A POINT, under every locale: built
            from integers, since the C library's formatting writes a comma in
            fr_FR (README, the locale rule). */
        std::string fixed (double value, int decimals)
        {
            long long scale = 1;

            for (int d = 0; d < decimals; ++d)
                scale *= 10;

            const auto scaled = std::llround (std::abs (value) * static_cast<double> (scale));
            std::string out = scaled != 0 && value < 0.0 ? "-" : "";
            out += std::to_string (scaled / scale);

            if (decimals > 0)
            {
                auto fraction = std::to_string (scaled % scale);
                out += "." + std::string (static_cast<std::size_t> (decimals) - fraction.size(), '0') + fraction;
            }

            return out;
        }

        std::string labelOf (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            const auto base = "/godot/cue/" + cueId + "/";
            const auto number = text (snapshot, base + "number");
            const auto name = shownCueName (snapshot, cueId);

            if (! number.empty() && ! name.empty())
                return number + " " + name;

            if (! name.empty())
                return name;

            return number.empty() ? cueId : "cue " + number;
        }

        /** "Band 2 gain", "High-pass" - the EQ panel's own words for its rows. */
        std::string eqWords (const std::string& row)
        {
            if (row == "eqHpfFreq") return "High-pass";
            if (row == "eqLpfFreq") return "Low-pass";

            const auto suffix = row.size() > 4 ? row.substr (4) : std::string();
            const auto band = std::string ("Band ") + (row.size() > 3 ? row[3] : '?');

            if (suffix == "Gain") return band + " gain";
            if (suffix == "Q")    return band + " width";
            return band + " frequency";
        }

        std::string eqValueText (const std::string& row, double value)
        {
            if (row.find ("Gain") != std::string::npos)
                return fixed (value, 1) + " dB";

            if (row.find ("Freq") != std::string::npos)
                return value >= 1000.0 ? fixed (value / 1000.0, 2) + " kHz" : fixed (value, 0) + " Hz";

            return "Q " + fixed (value, 2);
        }
    }

    std::string fadeMoveAddress (const std::string& fadeId, const std::string& entry)
    {
        return "/godot/cue/" + fadeId + "/moves/" + entry;
    }

    double fractionForSpeed (double speed)
    {
        if (! (speed >= lowestStep))
            return 0.0;

        const auto low = std::log2 (lowestStep);
        const auto high = std::log2 (highestSpeed);
        return std::clamp ((std::log2 (std::min (speed, highestSpeed)) - low) / (high - low), 0.0, 1.0);
    }

    double speedForFraction (double fraction)
    {
        if (fraction <= 0.0)
            return 0.0;

        const auto low = std::log2 (lowestStep);
        const auto high = std::log2 (highestSpeed);
        const auto speed = std::exp2 (low + std::clamp (fraction, 0.0, 1.0) * (high - low));

        //  Four significant figures, as the dial writes a speed (§22.7).
        const auto scale = std::pow (10.0, 3 - static_cast<int> (std::floor (std::log10 (speed))));
        return std::round (speed * scale) / scale;
    }

    FadeMixReading readFadeMix (const tree::TreeSnapshot& snapshot, const std::string& fadeId)
    {
        FadeMixReading out;
        out.fadeId = fadeId;

        const auto fade = "/godot/cue/" + fadeId + "/";

        if (text (snapshot, fade + "kind") != "fade")
        {
            out.notice = "Only a fade cue has a fade's mixer.";
            return out;
        }

        const auto number = [&snapshot] (const std::string& address, double fallback)
        {
            return osc::parseDouble (text (snapshot, address)).value_or (fallback);
        };

        const auto levelOn = text (snapshot, fade + "levelOn") != "false";
        out.dcaId = text (snapshot, fade + "dca");
        out.targetId = text (snapshot, fade + "target");

        /*  A FADE ON A DCA moves its trim and nothing else: the DCA's own
            strip, which the motorised fader under it follows (§16.4). */
        if (! out.dcaId.empty())
        {
            const auto dca = "/godot/dca/" + out.dcaId + "/";
            const auto name = text (snapshot, dca + "name");

            out.present = true;
            out.targetName = name.empty() ? out.dcaId : name;
            out.targetKind = "dca";

            FadeStrip strip;
            strip.kind = "dca";
            strip.name = out.targetName;
            strip.under = "the DCA's trim";
            strip.address = fade + "level";
            strip.switchAddress = fade + "levelOn";
            strip.moved = levelOn;
            strip.value = levelOn ? number (fade + "level", -120.0) : number (dca + "trim", 0.0);
            out.strips.push_back (strip);
            return out;
        }

        if (out.targetId.empty() || text (snapshot, "/godot/cue/" + out.targetId + "/kind").empty())
        {
            out.notice = "This fade is aimed at nothing yet - pick its target, or a DCA, in the inspector.";
            return out;
        }

        const auto target = "/godot/cue/" + out.targetId + "/";
        out.present = true;
        out.targetKind = text (snapshot, target + "kind");
        out.targetName = labelOf (snapshot, out.targetId);
        out.hasEq = out.targetKind == "media" || out.targetKind == "mic";

        //  The level: the cue's, or a group's trim - a fader either way.
        {
            FadeStrip strip;
            strip.kind = "level";
            strip.name = "Level";
            strip.under = out.targetKind == "group" ? "the group's trim" : "the cue's level";
            strip.address = fade + "level";
            strip.switchAddress = fade + "levelOn";
            strip.moved = levelOn;
            strip.value = levelOn ? number (fade + "level", -120.0) : number (target + "level", 0.0);
            out.strips.push_back (strip);
        }

        //  The speed, which only a file has (§22.6, EC).
        if (out.targetKind == "media")
        {
            const auto rateOn = text (snapshot, fade + "rateOn") == "true";

            FadeStrip strip;
            strip.kind = "speed";
            strip.name = "Speed";
            strip.under = "one is the file's own";
            strip.address = fade + "rate";
            strip.switchAddress = fade + "rateOn";
            strip.moved = rateOn;
            strip.value = rateOn ? number (fade + "rate", 1.0) : number (target + "rate", 1.0);
            out.strips.push_back (strip);
        }

        if (! out.hasEq)
            return out;

        //  A send into every mix the show declares (PB).
        const auto sends = cue::parseMoveList (text (snapshot, fade + "sends"));

        for (const auto& send : readSends (snapshot, out.targetId))
        {
            FadeStrip strip;
            strip.kind = "send";
            strip.name = send.name;
            strip.under = send.widthWord + " " + send.channelWord;
            strip.busId = send.busId;
            strip.address = fadeMoveAddress (fadeId, "send/" + send.busId);
            strip.switchAddress = strip.address;

            if (const auto moved = sends.find (send.busId); moved != sends.end())
            {
                strip.moved = true;
                strip.value = moved->second;
            }
            else
            {
                strip.value = send.levelDb;
            }

            out.strips.push_back (strip);
        }

        //  The EQ numbers it moves, in the panel's order.
        const auto eq = cue::parseMoveList (text (snapshot, fade + "eq"));

        const auto addEq = [&] (const std::string& row)
        {
            if (const auto found = eq.find (row); found != eq.end())
                out.moves.push_back ({ "eq/" + row, "EQ " + eqWords (row), eqValueText (row, found->second),
                                       fadeMoveAddress (fadeId, "eq/" + row) });
        };

        addEq ("eqHpfFreq");
        addEq ("eqLpfFreq");

        for (const auto band : { '1', '2', '3', '4' })
            for (const auto* suffix : { "Freq", "Gain", "Q" })
                addEq (std::string ("eqB") + band + suffix);

        //  The inserts it can open, and the plugin values it moves, by name.
        const auto chain = readFx (snapshot, out.targetId);
        const auto fx = cue::parseMoveList (text (snapshot, fade + "fx"));

        for (const auto& strip : chain.strips)
        {
            if (strip.present())
                out.inserts.push_back ({ strip.pluginId, strip.name, strip.enabled });

            for (const auto& [key, value] : fx)
            {
                const auto slash = key.rfind ('/');

                if (slash == std::string::npos || key.substr (0, slash) != strip.pluginId)
                    continue;

                const auto index = std::atoi (key.c_str() + slash + 1);
                std::string name = "parameter " + std::to_string (index + 1);

                for (const auto& param : strip.params)
                    if (param.index == index && ! param.name.empty())
                        name = param.name;

                out.moves.push_back ({ "fx/" + key, strip.name + " - " + name,
                                       fixed (value * 100.0, 0) + " %", fadeMoveAddress (fadeId, "fx/" + key) });
            }
        }

        /*  A plugin value whose entry has left the set is still in the list,
            and still a row to untick. */
        for (const auto& [key, value] : fx)
        {
            const auto entry = "fx/" + key;

            if (std::none_of (out.moves.begin(), out.moves.end(),
                              [&entry] (const FadeMoveRow& row) { return row.entry == entry; }))
                out.moves.push_back ({ entry, key + " (not in the set)", fixed (value * 100.0, 0) + " %",
                                       fadeMoveAddress (fadeId, entry) });
        }

        return out;
    }
}
