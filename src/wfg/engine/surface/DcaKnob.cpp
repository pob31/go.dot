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
#include <wfg/engine/video/DcaOpacity.h>

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

        //  The range of the row a mark's value is kept in: an output's trim reaches lower (§109, BBM).
        std::pair<double, double> rangeOf (KnobMode mode, MarkPlace place) noexcept
        {
            if (mode == KnobMode::curve)
                return { knobCurveMin, knobCurveMax };

            return place == MarkPlace::output ? std::pair { knobTrimMinDb, knobTrimMaxDb }
                                              : std::pair { knobOffsetMinDb, knobOffsetMaxDb };
        }

        //  To a hundredth, so a value typed in the inspector and turned does not grow a tail.
        double hundredths (double value) noexcept
        {
            return std::round (value * 100.0) / 100.0;
        }

        std::string addressOf (const DcaMark& mark, KnobMode mode)
        {
            switch (mark.place)
            {
                case MarkPlace::output: return "/godot/bus/" + mark.id + "/trim";
                case MarkPlace::canvas: return "/godot/canvas/" + mark.id + "/dcaCurve";
                case MarkPlace::cue:    break;
            }

            return "/godot/cue/" + mark.id + (mode == KnobMode::curve ? "/dcaCurve" : "/dcaOffset");
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

    std::map<std::string, std::vector<DcaMark>> dcaMarksUnder (const tree::TreeSnapshot& snapshot)
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
                    seen.id = cueId;
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
                                                         [&cueId] (const DcaMark& mark) { return mark.id == cueId; });

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

        /*  THEN THE OUTPUTS AND THE CANVASES THE DCA RIDES (namespace draft
            §109, BBI, BBL), playing or not, after every cue's mark: outputs in
            channel order, ties by identifier as the Outputs tab lists them,
            then canvases in the show's order. */
        struct Output
        {
            std::string id;
            int firstChannel = 0;
            double trimDb = 0.0;
            std::string dca;
        };

        std::map<std::string, Output> outputs;
        constexpr std::string_view busPrefix = "/godot/bus/";

        for (const auto* node : snapshot.under (busPrefix))
        {
            const auto rest = std::string_view (node->address).substr (busPrefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string_view::npos || rest.find ('/', slash + 1) != std::string_view::npos)
                continue;

            const auto row = rest.substr (slash + 1);

            if (row != "firstChannel" && row != "trim" && row != "dca")
                continue;

            auto& output = outputs[std::string (rest.substr (0, slash))];
            output.id = std::string (rest.substr (0, slash));

            if (row == "firstChannel")
                output.firstChannel = static_cast<int> (numberOf (snapshot, node->address, 0.0));
            else if (row == "trim")
                output.trimDb = numberOf (snapshot, node->address, 0.0);
            else
                output.dca = textOf (snapshot, node->address);
        }

        std::vector<const Output*> byChannel;

        for (const auto& [id, output] : outputs)
            if (! output.dca.empty())
                byChannel.push_back (&output);

        std::stable_sort (byChannel.begin(), byChannel.end(),
                          [] (const Output* a, const Output* b) { return a->firstChannel < b->firstChannel; });

        for (const auto* output : byChannel)
        {
            DcaMark place;
            place.id = output->id;
            place.place = MarkPlace::output;
            place.offsetDb = output->trimDb;
            place.sound = true;

            for (const auto& reached : chainOf (output->dca))
                out[reached].push_back (place);
        }

        for (const auto& canvasId : wordsOf (textOf (snapshot, "/godot/canvas/order")))
        {
            const auto base = "/godot/canvas/" + canvasId + "/";

            if (const auto dca = textOf (snapshot, base + "dca"); ! dca.empty())
            {
                DcaMark place;
                place.id = canvasId;
                place.place = MarkPlace::canvas;
                place.curve = numberOf (snapshot, base + "dcaCurve", 0.0);
                place.picture = true;

                for (const auto& reached : chainOf (dca))
                    out[reached].push_back (place);
            }
        }

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
                reading.place = mark.place;
            }
            else if (std::abs (valueOf (mark, mode) - reading.value) > 1.0e-9)
            {
                reading.disagree = true;
            }
        }

        return reading;
    }

    double knobTurned (KnobMode mode, double value, int steps, MarkPlace place) noexcept
    {
        const auto [low, high] = rangeOf (mode, place);
        const auto step = mode == KnobMode::curve ? knobCurveStep : knobOffsetStepDb;

        return hundredths (std::clamp (value + step * steps, low, high));
    }

    std::vector<std::pair<std::string, std::string>> knobWrites (const std::vector<DcaMark>& marks,
                                                                   KnobMode mode, int steps)
    {
        std::vector<std::pair<std::string, std::string>> writes;
        std::set<std::string> written;

        /*  THE CUES' MARKS COME FIRST, so the value shown is a cue's whenever
            one moves, and every cue's mark is set to it turned (ABY). */
        const auto reading = knobReading (marks, mode);
        const auto target = knobTurned (mode, reading.value, steps, MarkPlace::cue);

        for (const auto& mark : marks)
        {
            if (! moves (mark, mode) || ! written.insert (addressOf (mark, mode)).second)
                continue;

            //  An output or a canvas from its own value, by the same detents (§109, BBM).
            const auto wanted = mark.place == MarkPlace::cue ? target
                                                             : knobTurned (mode, valueOf (mark, mode), steps, mark.place);

            if (std::abs (valueOf (mark, mode) - wanted) <= 1.0e-9)
                continue;

            writes.emplace_back (addressOf (mark, mode), osc::formatDouble (wanted));
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

    std::string dcaLevelWords (double levelDb, const std::vector<DcaMark>& marks, const DcaContents& assigned,
                               int width)
    {
        auto picture = assigned.picture;
        auto sound = assigned.sound;
        auto curve = 0.0;

        //  What plays decides: the cues' marks, never the outputs and canvases always there.
        if (std::any_of (marks.begin(), marks.end(), [] (const DcaMark& mark) { return mark.place == MarkPlace::cue; }))
        {
            picture = std::any_of (marks.begin(), marks.end(),
                                   [] (const DcaMark& mark) { return mark.place == MarkPlace::cue && mark.picture; });
            sound = std::any_of (marks.begin(), marks.end(),
                                 [] (const DcaMark& mark) { return mark.place == MarkPlace::cue && mark.sound; });
        }

        /*  The mark shown first that holds a picture: the run that started
            last, else the first canvas the DCA rides (§109, BBO) - the cues'
            marks come first. */
        for (const auto& mark : marks)
            if (mark.picture)
            {
                curve = mark.curve;
                break;
            }

        const auto silent = levelDb <= -120.0;
        const auto clamped = std::clamp (levelDb, -120.0, 12.0);

        //  "-6.2", one decimal, or the whole number when `whole`.
        const auto decibels = [&] (bool whole)
        {
            if (silent)
                return std::string ("-inf");

            const auto scaled = static_cast<long long> (std::llround (std::abs (clamped) * (whole ? 1.0 : 10.0)));
            auto digits = whole ? std::to_string (scaled)
                                : std::to_string (scaled / 10) + "." + std::to_string (scaled % 10);

            return (clamped < 0.0 && scaled != 0 ? "-" : "") + digits;
        };

        if (! picture)
            return decibels (false) + " dB";

        const auto percent = static_cast<long long> (std::llround (
                                 video::shapedOpacity (video::opacityForTrim (silent ? -120.0 : clamped), curve) * 100.0));

        if (! sound)
            return std::to_string (percent) + (width >= 5 ? " %" : "%");

        return width >= 11 ? decibels (false) + " dB " + std::to_string (percent) + "%"
                           : decibels (true) + " " + std::to_string (percent) + "%";
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
