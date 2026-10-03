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

#include <wfg/engine/cue/FadeMoves.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace wfg::cue
{
    std::map<std::string, double> parseMoveList (const std::string& text)
    {
        std::map<std::string, double> out;
        std::istringstream in (text);
        std::string pair;

        while (in >> pair)
        {
            const auto colon = pair.rfind (':');

            if (colon == std::string::npos || colon == 0)
                continue;

            const auto value = osc::parseDouble (pair.substr (colon + 1));

            if (value.has_value() && std::isfinite (*value))
                out[pair.substr (0, colon)] = *value;
        }

        return out;
    }

    std::string formatMoveList (const std::map<std::string, double>& values)
    {
        std::string out;

        for (const auto& [key, value] : values)
        {
            if (! out.empty())
                out += ' ';

            out += key + ":" + osc::formatDouble (value);
        }

        return out;
    }

    namespace
    {
        /** `eqB<n><suffix>` with n 1..4, answering the suffix. */
        bool bandRow (const std::string& row, std::string& suffix)
        {
            if (row.size() < 5 || row.rfind ("eqB", 0) != 0 || row[3] < '1' || row[3] > '4')
                return false;

            suffix = row.substr (4);
            return true;
        }
    }

    bool isMovableEqRow (const std::string& row)
    {
        if (row == "eqHpfFreq" || row == "eqLpfFreq")
            return true;

        std::string suffix;
        return bandRow (row, suffix) && (suffix == "Freq" || suffix == "Gain" || suffix == "Q");
    }

    void eqRowRange (const std::string& row, double& lowest, double& highest)
    {
        //  The table's ranges (godot-parameters.csv, sound/eq*).
        std::string suffix;

        if (row == "eqHpfFreq")                          { lowest = 20.0;   highest = 2000.0; }
        else if (row == "eqLpfFreq")                     { lowest = 1000.0; highest = 20000.0; }
        else if (bandRow (row, suffix) && suffix == "Gain") { lowest = -24.0; highest = 24.0; }
        else if (bandRow (row, suffix) && suffix == "Q")    { lowest = 0.1;   highest = 10.0; }
        else                                             { lowest = 20.0;   highest = 20000.0; }
    }

    MoveDomain eqRowDomain (const std::string& row)
    {
        std::string suffix;
        return bandRow (row, suffix) && suffix == "Gain" ? MoveDomain::decibels : MoveDomain::logarithm;
    }

    std::vector<FadeMove> readFadeMoves (const std::string& sends, const std::string& eq,
                                         const std::string& fx)
    {
        std::vector<FadeMove> out;

        for (const auto& [bus, db] : parseMoveList (sends))
            out.push_back ({ "send/" + bus, std::clamp (db, -120.0, 12.0), MoveDomain::decibels });

        for (const auto& [row, value] : parseMoveList (eq))
        {
            if (! isMovableEqRow (row))
                continue;

            double lowest = 0.0, highest = 0.0;
            eqRowRange (row, lowest, highest);
            out.push_back ({ "eq/" + row, std::clamp (value, lowest, highest), eqRowDomain (row) });
        }

        for (const auto& [key, value] : parseMoveList (fx))
        {
            std::string kind, name;
            int index = -1;

            if (splitMoveEntry ("fx/" + key, kind, name, index))
                out.push_back ({ "fx/" + key, std::clamp (value, 0.0, 1.0), MoveDomain::linear });
        }

        return out;
    }

    bool splitMoveEntry (const std::string& entry, std::string& kind, std::string& name, int& index)
    {
        const auto slash = entry.find ('/');

        if (slash == std::string::npos || slash + 1 >= entry.size())
            return false;

        kind = entry.substr (0, slash);
        auto rest = entry.substr (slash + 1);
        index = -1;

        if (kind == "send" || kind == "eq")
        {
            if (rest.find ('/') != std::string::npos || rest.find (':') != std::string::npos)
                return false;

            name = rest;
            return kind == "send" || isMovableEqRow (rest);
        }

        if (kind != "fx")
            return false;

        const auto second = rest.find ('/');

        if (second == std::string::npos || second == 0 || second + 1 >= rest.size())
            return false;

        const auto digits = rest.substr (second + 1);

        if (digits.size() > 6 || ! std::all_of (digits.begin(), digits.end(),
                                                [] (char c) { return c >= '0' && c <= '9'; }))
            return false;

        if (digits.size() > 1 && digits.front() == '0')
            return false;

        name = rest.substr (0, second);
        index = std::atoi (digits.c_str());
        return name.find (':') == std::string::npos;
    }

    const char* moveListAttribute (const std::string& kind)
    {
        if (kind == "send") return "sends";
        if (kind == "eq")   return "eq";
        if (kind == "fx")   return "fx";
        return "";
    }

    double moveValueAt (double from, double to, double progress, bool sCurve, MoveDomain domain)
    {
        const auto t = std::clamp (progress, 0.0, 1.0);
        const auto shaped = sCurve ? t * t * (3.0 - 2.0 * t) : t;

        if (t >= 1.0)
            return to;

        /*  A frequency or a Q in its logarithm, so a sweep spends as long on
            each octave. Both are kept above nought by their rows' ranges; a
            stray nought falls back to straight rather than to a logarithm of
            nothing. */
        if (domain == MoveDomain::logarithm && from > 0.0 && to > 0.0)
            return std::exp (std::log (from) + (std::log (to) - std::log (from)) * shaped);

        return from + (to - from) * shaped;
    }
}
