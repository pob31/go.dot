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

#include <wfg/client/model/OutputList.h>

#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view prefix = "/godot/bus/";

        /*  `reading` and not `text`, which is the free function three lines
            below this one: GCC's -Wshadow reports a parameter shadowing a
            global declaration, and the repo's convention is to rename the
            parameter rather than the thing it hid. */
        /*  A decimal the engine wrote: '.' always, whatever the locale -
            read by hand, since `std::stod` would read "1.5" as 1 under
            fr_FR. */
        double decimal (const std::string& reading, double fallback)
        {
            if (reading.empty())
                return fallback;

            auto at = std::size_t { 0 };
            auto sign = 1.0;

            if (reading[at] == '-' || reading[at] == '+')
                sign = reading[at++] == '-' ? -1.0 : 1.0;

            auto value = 0.0;
            auto digits = 0;

            for (; at < reading.size() && reading[at] >= '0' && reading[at] <= '9'; ++at, ++digits)
                value = value * 10.0 + static_cast<double> (reading[at] - '0');

            if (at < reading.size() && reading[at] == '.')
                for (auto scale = 0.1; ++at < reading.size() && reading[at] >= '0' && reading[at] <= '9'; scale *= 0.1, ++digits)
                    value += scale * static_cast<double> (reading[at] - '0');

            return digits > 0 ? sign * value : fallback;
        }

        int number (const std::string& reading, int fallback)
        {
            /*  The tree's own text, which came through `osc::formatDouble` or
                an integer, so this is never a locale question - but it is
                still a place a malformed reading must not throw. */
            try
            {
                return reading.empty() ? fallback : std::stoi (reading);
            }
            catch (...)
            {
                return fallback;
            }
        }
    }

    std::string OutputRow::kindWord() const
    {
        return kind == "mix" ? "Mix channel" : "Direct out";
    }

    std::string OutputRow::widthWord() const
    {
        if (width == 1) return "Mono";
        if (width == 2) return "Stereo";

        return std::to_string (width) + " channels";
    }

    std::string OutputRow::channelWord() const
    {
        /*  COUNTED FROM ONE, because that is how a patch panel and an
            interface are labelled and this column is read beside them. Every
            address inside the engine counts from nought, and the two meet
            here. */
        const auto first = firstChannel + 1;

        if (width <= 1)
            return std::to_string (first);

        return std::to_string (first) + "-" + std::to_string (firstChannel + width);
    }

    std::vector<OutputRow> readOutputs (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS over the tree, gathering by identifier, the way `inspect`
            does: `childrenOf` walks the whole tree per call and is banned for
            it. */
        std::map<std::string, OutputRow> found;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (prefix, 0) != 0)
                continue;

            const auto rest = node->address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                continue;

            const auto id = rest.substr (0, slash);
            const auto name = rest.substr (slash + 1);

            auto& row = found[id];
            row.id = id;

            if (name == "name")              row.name = text (node);
            else if (name == "kind")         row.kind = text (node);
            else if (name == "width")        row.width = number (text (node), 1);
            else if (name == "firstChannel") row.firstChannel = number (text (node), 0);
            else if (name == "trim")         row.trimDb = decimal (text (node), 0.0);
            else if (name == "dca")          row.dca = text (node);
        }

        std::vector<OutputRow> rows;
        rows.reserve (found.size());

        for (auto& [id, row] : found)
        {
            if (row.kind.empty())
                row.kind = "direct";

            /*  A bus with no name is still a row somebody has to point at, so
                it gets one here rather than being drawn blank. The engine
                names every bus it creates; this is for a show written by
                hand. */
            if (row.name.empty())
                row.name = "Output " + std::to_string (row.firstChannel + 1);

            rows.push_back (row);
        }

        /*  Channel order, ties by identifier so the list never flickers
            between two readings of the same show. The engine keeps document
            order the same; a show written by hand may not, and the list is
            read beside an interface rather than beside a file. */
        std::stable_sort (rows.begin(), rows.end(),
                          [] (const OutputRow& a, const OutputRow& b)
                          {
                              if (a.firstChannel != b.firstChannel)
                                  return a.firstChannel < b.firstChannel;

                              return a.id < b.id;
                          });

        return rows;
    }

    int outputChannelCount (const std::vector<OutputRow>& rows)
    {
        auto total = 0;

        for (const auto& row : rows)
            total = std::max (total, row.firstChannel + row.width);

        return total;
    }

    bool patchHasSettled (const tree::TreeSnapshot& snapshot)
    {
        if (isYes (flag (snapshot, "/godot/audio/patchSettled")))
            return true;

        if (! text (snapshot, "/godot/audio/outputPatch").empty())
            return true;

        /*  AND A LAYOUT THAT IS NOT PACKED, which is the third of
            `doc::applyLayoutEdit`'s three and the one a client would forget:
            a show written by hand with a hole in its channels is already
            settled, and telling the designer it will follow their edits
            would be a promise the engine does not keep. */
        auto expected = 0;

        for (const auto& row : readOutputs (snapshot))
        {
            if (row.firstChannel != expected)
                return true;

            expected += row.width;
        }

        return false;
    }

    std::string outputRegime (bool settled)
    {
        if (settled)
            return "The interface patch is set: each output keeps the channels it is on, "
                   "and a new one takes the next free ones.";

        return "The interface patch follows this list: add, move and widen outputs freely, "
               "until something plays or you patch by hand.";
    }

    std::vector<std::string> channelLabels (const std::vector<OutputRow>& rows, int atLeast)
    {
        std::vector<std::string> labels;
        labels.resize (static_cast<std::size_t> (std::max (atLeast, outputChannelCount (rows))));

        for (std::size_t at = 0; at < labels.size(); ++at)
            labels[at] = "Output " + std::to_string (at + 1);

        for (const auto& row : rows)
            for (auto channel = 0; channel < row.width; ++channel)
            {
                const auto at = static_cast<std::size_t> (row.firstChannel + channel);

                if (at >= labels.size())
                    continue;

                /*  A STEREO PAIR IS NAMED BY ITS SIDES and anything wider by
                    its channel number: "Main L/R · L" is what somebody wiring
                    it would say, and "WFS send · 3" is what they would say for
                    a twelve-channel processor feed. A mono output is its own
                    name and nothing else - "Voice · 1" would be noise. */
                if (row.width == 1)
                    labels[at] = row.name;
                else if (row.width == 2)
                    labels[at] = row.name + " · " + (channel == 0 ? "L" : "R");
                else
                    labels[at] = row.name + " · " + std::to_string (channel + 1);
            }

        return labels;
    }

    std::string OutputRow::trimWord() const
    {
        //  To the tenth, built from integers so no locale's comma gets in.
        const auto tenths = static_cast<long> (trimDb * 10.0 + (trimDb < 0.0 ? -0.5 : 0.5));

        if (tenths == 0)
            return "0 dB";

        const auto magnitude = tenths < 0 ? -tenths : tenths;
        auto out = std::string (tenths < 0 ? "-" : "+") + std::to_string (magnitude / 10);

        if (magnitude % 10 != 0)
            out += "." + std::to_string (magnitude % 10);

        return out + " dB";
    }

    std::vector<std::string> dcaTwiceSentences (const tree::TreeSnapshot& snapshot,
                                                const std::vector<OutputRow>& outputs,
                                                const std::vector<DcaRow>& dcas)
    {
        /*  UP A DCA'S NESTING, bounded by how many there are, as the engine
            walks it: a circle the door refused cannot hang a window either. */
        const auto chainOf = [&dcas] (std::string first)
        {
            std::vector<std::string> chain;

            for (std::size_t steps = 0; steps < dcas.size() && ! first.empty(); ++steps)
            {
                const auto found = std::find_if (dcas.begin(), dcas.end(),
                                                 [&first] (const DcaRow& d) { return d.id == first; });

                if (found == dcas.end())
                    break;

                chain.push_back (first);
                first = found->parent;
            }

            return chain;
        };

        /*  ONE PASS over the tree, as `readOutputs` makes: each cue's mark and
            direct out, and each send's cue and mix. */
        struct Cue { std::string dca, directOut; };
        std::map<std::string, Cue> cues;
        std::map<std::string, std::pair<std::string, std::string>> sends;   // send -> cue, bus

        for (const auto* node : snapshot.all())
        {
            const auto& address = node->address;

            if (address.rfind ("/godot/cue/", 0) == 0)
            {
                const auto rest = address.substr (11);
                const auto slash = rest.find ('/');

                if (slash == std::string::npos)
                    continue;

                const auto field = rest.substr (slash + 1);

                if (field == "dca")
                    cues[rest.substr (0, slash)].dca = text (node);
                else if (field == "directOut")
                    cues[rest.substr (0, slash)].directOut = text (node);
            }
            else if (address.rfind ("/godot/send/", 0) == 0)
            {
                const auto rest = address.substr (12);
                const auto slash = rest.find ('/');

                if (slash == std::string::npos)
                    continue;

                const auto field = rest.substr (slash + 1);

                if (field == "cue")
                    sends[rest.substr (0, slash)].first = text (node);
                else if (field == "bus")
                    sends[rest.substr (0, slash)].second = text (node);
            }
        }

        std::vector<std::string> sentences;

        for (const auto& output : outputs)
        {
            const auto outputChain = chainOf (output.dca);

            if (outputChain.empty())
                continue;

            //  The cues routed here, once each.
            std::vector<std::string> routed;

            for (const auto& [id, cue] : cues)
                if (cue.directOut == output.id)
                    routed.push_back (id);

            for (const auto& [send, toWhere] : sends)
                if (toWhere.second == output.id
                      && std::find (routed.begin(), routed.end(), toWhere.first) == routed.end())
                    routed.push_back (toWhere.first);

            /*  The DCA they share with the output: the first of the output's
                chain - its own mark before the ones it sits inside - that is
                also in the cue's. */
            std::string shared;
            std::size_t count = 0;

            for (const auto& id : routed)
            {
                const auto found = cues.find (id);

                if (found == cues.end())
                    continue;

                const auto cueChain = chainOf (found->second.dca);

                for (const auto& dca : outputChain)
                {
                    if (std::find (cueChain.begin(), cueChain.end(), dca) == cueChain.end())
                        continue;

                    ++count;

                    if (shared.empty())
                        shared = dca;

                    break;
                }
            }

            if (count == 0)
                continue;

            const auto named = std::find_if (dcas.begin(), dcas.end(),
                                             [&shared] (const DcaRow& d) { return d.id == shared; });
            const auto dcaName = named == dcas.end() ? shared
                                 : named->name.empty() ? named->label() : named->name;

            sentences.push_back (dcaName + " also trims " + std::to_string (count)
                                   + (count == 1 ? " cue" : " cues") + " played through "
                                   + output.name + ": there it counts twice.");
        }

        return sentences;
    }
}
