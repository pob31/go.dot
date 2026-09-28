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
#include <wfg/engine/document/Sequence.h>

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
        out.name = channel.getProperty ("name").toString().toStdString();
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
                               doc::ShowDocument& document)
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

                                /*  WHILE A KEEP WRITES THE TAKE (§19.8), nothing
                                    empties what it reads: Undo and Clear wait. */
                                if ((verb == TakeVerb::undo || verb == TakeVerb::clear)
                                      && takes.of (channelId).keeping)
                                    return Outcome::rejected (reason::busy);

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

        registry.add ({ "take.keep",
                        "A sampling channel's closed take made a file under the show's media/takes - the take and"
                        " every closed layer on it, summed - and with asCue a media cue after the one given, or the"
                        " one sounding on the channel, that loops it between the take's points.",
                        { { "channel", 's', false }, { "asCue", 'T', true }, { "after", 's', true } },
                        true,
                        [&takes, &runs, &document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto channelId = args[0].getString();
                            const auto channel = samplingChannelOf (document, channelId);

                            if (! channel.declared)
                                return Outcome::rejected (reason::unknownId);

                            if (! channel.samples())
                                return Outcome::rejected (reason::badValue);

                            const auto& take = takes.of (channelId);

                            if (take.state == "empty" || take.state == "recording")
                                return Outcome::rejected (reason::notClosed);

                            if (take.keeping)
                                return Outcome::rejected (reason::busy);

                            KeepRequest request;
                            request.channel = channelId;
                            request.stem = channel.name.empty() ? channelId : channel.name;
                            request.asCue = args.size() > 1 && args[1].getBool();
                            request.loopIn = take.loopIn;
                            request.loopOut = take.loopOut;

                            if (request.asCue)
                            {
                                //  A CHANGE OF THE SHOW, refused under the lock as any is; the file alone is not one.
                                if (document.isLocked())
                                    return Outcome::rejected (reason::locked);

                                request.afterCue = args.size() > 2 ? args[2].getString() : std::string {};

                                if (request.afterCue.empty())
                                    if (const auto* holder = runs.holderOf (channelId))
                                        request.afterCue = holder->cue;

                                if (request.afterCue.empty() || ! document.findById (request.afterCue).isValid())
                                    return Outcome::rejected (reason::unknownId);
                            }

                            takes.keep (std::move (request));
                            return Outcome::ok (args);
                        } });

        registry.add ({ "take.kept",
                        "The writer made a kept take a file in the show's media, or says why it could not - so a"
                        " replay knows the name it found - and for a Keep as cue, the media cue, range and route"
                        " it made to loop the file.",
                        { { "channel", 's', false }, { "file", 's', false }, { "error", 's', false },
                          { "cue", 's', true }, { "range", 's', true }, { "route", 's', true } },
                        true,
                        [&takes, &document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto channelId = args[0].getString();

                            if (! samplingChannelOf (document, channelId).declared)
                                return Outcome::rejected (reason::unknownId);

                            const auto file = args[1].getString();
                            const auto error = args[2].getString();
                            const auto request = takes.kept (channelId, file, error);

                            if (! error.empty() || ! request.has_value() || ! request->asCue)
                                return Outcome::ok (args);

                            /*  KEEP AS CUE: a media cue after the one asked, its
                                file the one just written, a range at the take's
                                points looping for ever, and a route to the first
                                bus - one step to undo. The show locked since, or
                                the cue it was to follow gone, and the file stays
                                kept, with a sentence and no cue. */
                            if (document.isLocked())
                            {
                                takes.say (channelId, "kept as " + file + ", but the show is locked: no cue was made");
                                return Outcome::ok (args);
                            }

                            const auto after = document.findById (request->afterCue);
                            const auto parent = after.isValid() ? after.getParent() : juce::ValueTree {};
                            const auto parentId = parent.isValid() ? parent.getProperty ("id").toString().toStdString()
                                                                   : std::string {};

                            if (parentId.empty())
                            {
                                takes.say (channelId, "kept as " + file + ", but the cue it was to follow is gone");
                                return Outcome::ok (args);
                            }

                            auto position = 0;

                            for (const auto& child : parent)
                            {
                                if (child == after)
                                    break;

                                if (doc::isSequenceChild (child))
                                    ++position;
                            }

                            const auto given = [&args] (std::size_t index)
                            {
                                return args.size() > index ? args[index].getString() : std::string {};
                            };

                            auto name = file.substr (file.rfind ('/') == std::string::npos ? 0 : file.rfind ('/') + 1);

                            if (name.size() > 4 && name.compare (name.size() - 4, 4, ".wav") == 0)
                                name.resize (name.size() - 4);

                            const auto cue = document.createCue (parentId, position + 1, "media", name, given (3));

                            if (! cue.ok)
                                return Outcome::rejected (cue.reason);

                            if (const auto set = document.setAttribute ("/godot/cue/" + cue.id + "/file", file); ! set.ok)
                                return Outcome::rejected (set.reason);

                            const auto range = document.createRange (cue.id, request->loopIn, request->loopOut, given (4));

                            if (! range.ok)
                                return Outcome::rejected (range.reason);

                            if (const auto set = document.setAttribute ("/godot/range/" + range.id + "/loops", "0"); ! set.ok)
                                return Outcome::rejected (set.reason);

                            const auto route = document.defaultMediaRoute (cue.id, 2, given (5));

                            if (! route.ok)
                                return Outcome::rejected (route.reason);

                            //  THE IDENTIFIERS IT DREW, in the record, so a replay draws none.
                            auto applied = args;
                            applied.resize (3, osc::Value::string (std::string {}));
                            applied.push_back (osc::Value::string (cue.id));
                            applied.push_back (osc::Value::string (range.id));
                            applied.push_back (osc::Value::string (route.id));
                            return Outcome::ok (applied);
                        } });

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
