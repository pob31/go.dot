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

#include <wfg/engine/surface/DcaKnob.h>

#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>

namespace wfg::surface
{
    namespace
    {
        const std::string& textOf (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            static const std::string none;
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->values.size() != 1 || ! node->values.front().isString())
                return none;

            return node->values.front().getString();
        }

        double numberOf (const tree::TreeSnapshot& snapshot, const std::string& address, double otherwise)
        {
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->values.size() != 1 || ! node->values.front().isNumber()
                  || node->values.front().isNonFinite())
                return otherwise;

            return node->values.front().asDouble();
        }

        std::vector<std::string> wordsOf (std::string_view line)
        {
            std::vector<std::string> out;
            std::size_t at = 0;

            while (at < line.size())
            {
                const auto end = std::min (line.find (' ', at), line.size());

                if (end > at)
                    out.emplace_back (line.substr (at, end - at));

                at = end + 1;
            }

            return out;
        }

        bool moves (const DcaMark& mark, KnobMode mode) noexcept
        {
            return mode == KnobMode::curve ? mark.picture : mark.sound;
        }

        double valueOf (const DcaMark& mark, KnobMode mode) noexcept
        {
            return mode == KnobMode::curve ? mark.curve : mark.offsetDb;
        }

        //  Nearer the run that started last: later first, then nearer, then later in the order.
        bool shownBefore (const DcaMark& a, const DcaMark& b) noexcept
        {
            if (a.started > b.started || a.started < b.started)
                return a.started > b.started;

            if (a.depth != b.depth)
                return a.depth < b.depth;

            return a.order > b.order;
        }

        //  A number as a screen says it: whole, or to a tenth under ten, signed, no locale.
        std::string signedNumber (double value, bool tenths)
        {
            const auto magnitude = std::abs (value);
            const auto wantTenths = tenths && magnitude < 10.0;
            const auto scaled = static_cast<long long> (std::llround (magnitude * (wantTenths ? 10.0 : 1.0)));

            if (scaled == 0)
                return "0";

            auto digits = wantTenths ? std::to_string (scaled / 10) + "." + std::to_string (scaled % 10)
                                     : std::to_string (scaled);

            return (value < 0.0 ? "-" : "+") + digits;
        }
    }

    std::map<std::string, std::vector<DcaMark>> dcaMarksPlaying (const tree::TreeSnapshot& snapshot)
    {
        std::map<std::string, std::vector<DcaMark>> out;
        const auto declared = wordsOf (textOf (snapshot, "/godot/dca/order"));

        if (declared.empty())
            return out;

        //  A DCA and every one it sits inside, as far as there are DCAs.
        const auto chainOf = [&snapshot, &declared] (std::string first)
        {
            std::vector<std::string> chain;

            for (std::size_t steps = 0; steps < declared.size() && ! first.empty(); ++steps)
            {
                if (std::find (declared.begin(), declared.end(), first) == declared.end())
                    break;

                chain.push_back (first);
                first = textOf (snapshot, "/godot/dca/" + first + "/dca");
            }

            return chain;
        };

        const auto order = wordsOf (textOf (snapshot, "/godot/run/order"));

        for (std::size_t at = 0; at < order.size(); ++at)
        {
            const auto run = "/godot/run/" + order[at] + "/";
            const auto state = textOf (snapshot, run + "state");

            if (state != "playing" && state != "stopping")
                continue;

            const auto kind = textOf (snapshot, run + "kind");
            const auto picture = kind == "video";
            const auto sound = kind == "media" || kind == "mic";

            if (! picture && ! sound)
                continue;

            const auto started = numberOf (snapshot, run + "started", 0.0);

            /*  ITS OWN CUE'S MARK AND EACH RUN'S ABOVE IT, through the runs'
                parents: a group's, a movie's for its locked sound. */
            auto runId = order[at];

            for (int depth = 0; depth < 64 && ! runId.empty(); ++depth)
            {
                const auto cueId = textOf (snapshot, "/godot/run/" + runId + "/cue");
                const auto cue = "/godot/cue/" + cueId + "/";

                if (const auto dca = textOf (snapshot, cue + "dca"); ! cueId.empty() && ! dca.empty())
                {
                    DcaMark seen;
                    seen.cue = cueId;
                    seen.curve = numberOf (snapshot, cue + "dcaCurve", 0.0);
                    seen.offsetDb = numberOf (snapshot, cue + "dcaOffset", 0.0);
                    seen.picture = picture;
                    seen.sound = sound;
                    seen.started = started;
                    seen.depth = depth;
                    seen.order = static_cast<int> (at);

                    for (const auto& reached : chainOf (dca))
                    {
                        auto& marks = out[reached];
                        const auto found = std::find_if (marks.begin(), marks.end(),
                                                         [&cueId] (const DcaMark& mark) { return mark.cue == cueId; });

                        if (found == marks.end())
                        {
                            marks.push_back (seen);
                            continue;
                        }

                        found->picture = found->picture || picture;
                        found->sound = found->sound || sound;

                        if (shownBefore (seen, *found))
                        {
                            found->started = seen.started;
                            found->depth = seen.depth;
                            found->order = seen.order;
                        }
                    }
                }

                runId = textOf (snapshot, "/godot/run/" + runId + "/parent");
            }
        }

        for (auto& [dca, marks] : out)
            std::stable_sort (marks.begin(), marks.end(), shownBefore);

        return out;
    }

    KnobMode knobStartMode (const DcaContents& contents) noexcept
    {
        return contents.picture ? KnobMode::curve : KnobMode::offset;
    }

    KnobReading knobReading (const std::vector<DcaMark>& marks, KnobMode mode) noexcept
    {
        KnobReading reading;

        //  The marks come shown-first: the first the mode moves is the one shown.
        for (const auto& mark : marks)
        {
            if (! moves (mark, mode))
                continue;

            if (! reading.any)
            {
                reading.any = true;
                reading.value = valueOf (mark, mode);
            }
            else if (std::abs (valueOf (mark, mode) - reading.value) > 1.0e-9)
            {
                reading.disagree = true;
            }
        }

        return reading;
    }

    double knobTurned (KnobMode mode, double value, int steps) noexcept
    {
        const auto moved = mode == KnobMode::curve
                             ? std::clamp (value + knobCurveStep * steps, knobCurveMin, knobCurveMax)
                             : std::clamp (value + knobOffsetStepDb * steps, knobOffsetMinDb, knobOffsetMaxDb);

        //  To a hundredth, so a value typed in the inspector and turned does not grow a tail.
        return std::round (moved * 100.0) / 100.0;
    }

    std::vector<std::pair<std::string, std::string>> knobWrites (const std::vector<DcaMark>& marks,
                                                                   KnobMode mode, double target)
    {
        std::vector<std::pair<std::string, std::string>> writes;
        std::set<std::string> written;

        const auto clamped = mode == KnobMode::curve ? std::clamp (target, knobCurveMin, knobCurveMax)
                                                     : std::clamp (target, knobOffsetMinDb, knobOffsetMaxDb);
        const auto row = mode == KnobMode::curve ? "/dcaCurve" : "/dcaOffset";

        for (const auto& mark : marks)
        {
            if (! moves (mark, mode) || ! written.insert (mark.cue).second)
                continue;

            if (std::abs (valueOf (mark, mode) - clamped) <= 1.0e-9)
                continue;

            writes.emplace_back ("/godot/cue/" + mark.cue + row, osc::formatDouble (clamped));
        }

        return writes;
    }

    std::string knobWords (KnobMode mode, const KnobReading& reading, int width)
    {
        const auto wide = width >= 8;
        std::string word = mode == KnobMode::curve ? (wide ? "pic" : "p") : (wide ? "snd" : "s");

        if (! reading.any)
            return word + " --";

        if (reading.disagree)
            word += "*";
        else
            word += " ";

        return word + signedNumber (reading.value, mode == KnobMode::offset);
    }

    double knobRingFraction (KnobMode mode, double value) noexcept
    {
        if (mode == KnobMode::curve)
            return std::clamp ((value - knobCurveMin) / (knobCurveMax - knobCurveMin), 0.0, 1.0);

        if (value <= 0.0)
            return std::clamp (0.5 - 0.5 * value / knobOffsetMinDb, 0.0, 0.5);

        return std::clamp (0.5 + 0.5 * value / knobOffsetMaxDb, 0.5, 1.0);
    }
}
