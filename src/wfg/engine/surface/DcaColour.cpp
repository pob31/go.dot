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

#include <wfg/engine/surface/DcaColour.h>

#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string_view>
#include <vector>

namespace wfg::surface
{
    namespace
    {
        const std::string& noText()
        {
            static const std::string none;
            return none;
        }

        const std::string& textOf (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->values.size() != 1 || ! node->values.front().isString())
                return noText();

            return node->values.front().getString();
        }

        std::optional<double> numberOf (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->values.size() != 1 || ! node->values.front().isNumber()
                  || node->values.front().isNonFinite())
                return std::nullopt;

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

        /*  A TINT, "#RRGGBB", as a colour at full brightness and how bright it
            was: its brightest component over 255. Nothing for anything else. */
        std::optional<std::pair<Rgb, double>> fromTint (const std::string& text)
        {
            if (text.size() != 7 || text[0] != '#')
                return std::nullopt;

            const auto hex = [&text] (std::size_t at) -> int
            {
                int value = 0;

                for (std::size_t n = at; n < at + 2; ++n)
                {
                    const auto c = text[n];
                    const auto digit = c >= '0' && c <= '9' ? c - '0'
                                     : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                            : -1;
                    if (digit < 0)
                        return -1;

                    value = value * 16 + digit;
                }

                return value;
            };

            const auto red = hex (1), green = hex (3), blue = hex (5);

            if (red < 0 || green < 0 || blue < 0)
                return std::nullopt;

            const auto brightest = std::max ({ red, green, blue });

            if (brightest == 0)
                return std::make_pair (Rgb {}, 0.0);

            const auto lift = [brightest] (int component)
            {
                return static_cast<int> (std::lround (127.0 * component / brightest));
            };

            return std::make_pair (Rgb { lift (red), lift (green), lift (blue) }, brightest / 255.0);
        }

        struct Sum
        {
            double red = 0.0, green = 0.0, blue = 0.0, weight = 0.0;
            double loudest = -1.0;
            std::string loudestRun;
            double pictureLight = 0.0;
            bool pictureUp = false;

            void add (const Rgb& colour, double by)
            {
                red += colour.red * by;
                green += colour.green * by;
                blue += colour.blue * by;
                weight += by;
            }
        };
    }

    std::map<std::string, DcaLight> dcaLights (const tree::TreeSnapshot& snapshot)
    {
        //  THE NESTING: each DCA and the one it sits inside.
        const auto declared = wordsOf (textOf (snapshot, "/godot/dca/order"));

        if (declared.empty())
            return {};

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

        std::map<std::string, Sum> sums;

        const auto addPicture = [&sums] (const std::set<std::string>& reached, const std::string& tint)
        {
            const auto colour = fromTint (tint);

            for (const auto& dca : reached)
            {
                auto& sum = sums[dca];
                sum.pictureUp = true;

                if (colour.has_value() && colour->second > 0.0)
                {
                    sum.add (colour->first, colour->second);
                    sum.pictureLight = std::max (sum.pictureLight, colour->second);
                }
            }
        };

        for (const auto& runId : wordsOf (textOf (snapshot, "/godot/run/order")))
        {
            const auto run = "/godot/run/" + runId + "/";
            const auto state = textOf (snapshot, run + "state");

            if (state != "playing" && state != "stopping")
                continue;

            const auto cueId = textOf (snapshot, run + "cue");
            const auto cue = "/godot/cue/" + cueId + "/";
            const auto kind = textOf (snapshot, cue + "kind");

            if (kind != "media" && kind != "mic" && kind != "video")
                continue;

            /*  WHAT THE DCAS REACH OF IT: its own mark and every group's above
                it, each up its nesting - and for a sound, every output it
                plays through, by its direct out and its sends. */
            std::set<std::string> reached;
            const auto mark = [&reached, &chainOf] (const std::string& dca)
            {
                for (auto& each : chainOf (dca))
                    reached.insert (std::move (each));
            };

            mark (textOf (snapshot, cue + "dca"));

            auto parent = textOf (snapshot, cue + "parent");

            for (int depth = 0; depth < 64 && ! parent.empty(); ++depth)
            {
                mark (textOf (snapshot, "/godot/cue/" + parent + "/dca"));
                parent = textOf (snapshot, "/godot/cue/" + parent + "/parent");
            }

            if (kind == "video")
            {
                addPicture (reached, textOf (snapshot, run + "tint"));
                continue;
            }

            if (const auto out = textOf (snapshot, cue + "directOut"); ! out.empty())
                mark (textOf (snapshot, "/godot/bus/" + out + "/dca"));

            for (const auto& send : wordsOf (textOf (snapshot, cue + "sends")))
                if (const auto bus = textOf (snapshot, "/godot/send/" + send + "/bus"); ! bus.empty())
                    mark (textOf (snapshot, "/godot/bus/" + bus + "/dca"));

            /*  A SOUND by its timbre, weighted by how loud it left its track. A
                mic has no analysis: it moves the light but gives no hue. */
            const auto meter = numberOf (snapshot, run + "meter").value_or (-120.0);
            const auto loudness = std::clamp (std::pow (10.0, meter / 20.0), 0.0, 1.0);
            const auto colour = colourFromTimbre (textOf (snapshot, run + "timbre"));

            for (const auto& dca : reached)
            {
                auto& sum = sums[dca];

                if (colour.has_value())
                    sum.add (*colour, std::max (loudness, 1.0e-4));

                if (loudness > sum.loudest)
                {
                    sum.loudest = loudness;
                    sum.loudestRun = runId;
                }
            }
        }

        //  EACH CANVAS A DCA RIDES, whole.
        for (const auto& canvasId : wordsOf (textOf (snapshot, "/godot/canvas/order")))
        {
            const auto base = "/godot/canvas/" + canvasId + "/";
            const auto dca = textOf (snapshot, base + "dca");
            const auto tint = textOf (snapshot, base + "tint");

            if (dca.empty() || tint.empty())
                continue;

            std::set<std::string> reached;

            for (auto& each : chainOf (dca))
                reached.insert (std::move (each));

            /*  A CANVAS SHOWING BLACK IS NOT A PICTURE UP: only a canvas with
                something on it lights the ring. */
            if (const auto colour = fromTint (tint); colour.has_value() && colour->second > 0.0)
                addPicture (reached, tint);
        }

        std::map<std::string, DcaLight> lights;

        for (const auto& [dca, sum] : sums)
        {
            DcaLight light;

            if (sum.weight > 0.0)
                light.colour = Rgb { static_cast<int> (std::lround (sum.red / sum.weight)),
                                     static_cast<int> (std::lround (sum.green / sum.weight)),
                                     static_cast<int> (std::lround (sum.blue / sum.weight)) };
            else if (sum.pictureUp)
                light.colour = Rgb { 127, 127, 127 };   // up but see-through: a tenth of white, below

            if (! light.colour.has_value() && sum.loudestRun.empty())
                continue;

            light.loudestRun = sum.loudestRun;
            light.pictureLight = std::max (0.1, sum.pictureLight);

            if (! light.colour.has_value())
                light.colour = Rgb { 127, 127, 127 };   // a mic alone: its movement, in white

            lights.emplace (dca, light);
        }

        return lights;
    }

    Rgb restingLight (const DcaLight& light) noexcept
    {
        const auto colour = light.colour.value_or (Rgb {});
        const auto scale = std::clamp (light.pictureLight, 0.0, 1.0);

        return { static_cast<int> (std::lround (colour.red * scale)),
                 static_cast<int> (std::lround (colour.green * scale)),
                 static_cast<int> (std::lround (colour.blue * scale)) };
    }
}
