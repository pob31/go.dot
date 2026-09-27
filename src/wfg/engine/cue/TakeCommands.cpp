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

#include <wfg/engine/cue/TakeCommands.h>

#include <wfg/engine/command/Command.h>

#include <cmath>

namespace wfg::cue
{
    SamplingChannel samplingChannelOf (const doc::ShowDocument& document, const std::string& channelId)
    {
        SamplingChannel out;

        if (channelId.empty())
            return out;

        const auto channel = document.findById (channelId);

        if (! channel.isValid() || ! channel.hasType ("Channel"))
            return out;

        /*  THROUGH THE SCHEMA'S DEFAULTS: the canonical writer leaves out a
            value equal to its default, so a channel saved with four layers has
            no `layers` attribute at all. */
        out.declared = true;
        out.takeSeconds = static_cast<double> (channel.getProperty ("takeSeconds", 0.0));
        out.layers = static_cast<int> (channel.getProperty ("layers", 4));
        return out;
    }

    bool isSoundingOn (const RunTable& runs, const std::string& channelId)
    {
        const auto* holder = runs.holderOf (channelId);
        return holder != nullptr && holder->kind == "mic" && holder->state == runState::playing;
    }

    bool takeVerbOf (const std::string& word, TakeVerb& verb)
    {
        if (word == "record")        verb = TakeVerb::record;
        else if (word == "loop")     verb = TakeVerb::loop;
        else if (word == "overdub")  verb = TakeVerb::overdub;
        else if (word == "undo")     verb = TakeVerb::undo;
        else if (word == "clear")    verb = TakeVerb::clear;
        else                         return false;

        return true;
    }

    void registerTakeCommands (CommandRegistry& registry, TakeTable& takes, RunTable& runs,
                               const doc::ShowDocument& document)
    {
        struct Press
        {
            const char* name;
            TakeVerb verb;
            const char* description;
        };

        static constexpr Press presses[] = {
            { "take.record",  TakeVerb::record,
              "Rec on a sampling channel's take: record the first pass, close it and loop it, begin a layer,"
              " close the layer - by what the take is doing (namespace draft 19.3)." },
            { "take.loop",    TakeVerb::loop,
              "Loop: close the take or the layer being laid and loop it, or loop a take held silent." },
            { "take.overdub", TakeVerb::overdub,
              "A layer begun while the take loops or is held, or closed while one is laid." },
            { "take.undo",    TakeVerb::undo,
              "The top layer taken off, or the pass being laid abandoned - never the take itself." },
            { "take.clear",   TakeVerb::clear,
              "The channel emptied." },
        };

        for (const auto& each : presses)
        {
            const auto verb = each.verb;

            registry.add ({ each.name, each.description,
                            { { "channel", 's', false } },
                            true,
                            [&takes, &runs, &document, verb] (CommandContext&, const std::vector<osc::Value>& args)
                            {
                                const auto channelId = args[0].getString();
                                const auto channel = samplingChannelOf (document, channelId);

                                if (! channel.declared)
                                    return Outcome::rejected (reason::unknownId);

                                //  A rack channel with no recorder has no take to press.
                                if (! channel.samples())
                                    return Outcome::rejected (reason::badValue);

                                /*  WHAT IS RECORDED IS A CUE'S INPUT: Rec, Loop and
                                    a layer want a sounding mic cue on the channel.
                                    Undo and Clear only take something away. */
                                const auto needsCue = verb == TakeVerb::record || verb == TakeVerb::loop
                                                        || verb == TakeVerb::overdub;

                                if (needsCue && ! isSoundingOn (runs, channelId))
                                    return Outcome::rejected (reason::notRunning);

                                if (takes.wouldStartLayer (channelId, verb)
                                      && takes.of (channelId).layers >= channel.layers)
                                {
                                    /*  Refused, and a refusal changes nothing: §19.3's
                                        sentence - "Looper holds its 2 layers: Undo one
                                        or Clear" - is the take panel's to say, from
                                        `takeLayers` against `layers` (9c.4). */
                                    return Outcome::rejected (reason::layersFull);
                                }

                                takes.press (channelId, verb, channel.layers);
                                return Outcome::ok (args);
                            } });
        }

        registry.add ({ "take.closed",
                        "The audio thread closed a take at so many seconds - pressed, full, or held as its cue let"
                        " go - so that a replay knows how long it is.",
                        { { "channel", 's', false }, { "seconds", 'd', false }, { "how", 's', false } },
                        true,
                        [&takes, &document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto channelId = args[0].getString();
                            const auto channel = samplingChannelOf (document, channelId);

                            if (! channel.declared)
                                return Outcome::rejected (reason::unknownId);

                            const auto how = args[2].getString();
                            const auto seconds = args[1].asDouble();

                            if ((how != "pressed" && how != "full" && how != "held") || ! std::isfinite (seconds))
                                return Outcome::rejected (reason::badValue);

                            takes.closed (channelId, seconds, how, channel.takeSeconds);
                            return Outcome::ok (args);
                        } });
    }
}
