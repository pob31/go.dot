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

#include <wfg/engine/cue/LiveRows.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/cue/DcaTable.h>
#include <wfg/engine/cue/LaneTable.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/TakeTable.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        /*  `/godot/<owner>/<id>/trim`, split into its owner word and its
            identifier, or nothing for any other shape. Four parts exactly: a
            live row is a leaf of an object, never a container's own node. */
        struct LiveAddress
        {
            std::string owner;
            std::string id;
            std::string row;
        };

        /*  THE LEVEL'S RIDE while the faders are flipped to a cue (namespace
            draft §34, UO) - three parts, not an object's leaf, so it is named
            whole. A send's is its mix's leaf, `/godot/bus/<mix>/laneRide`. */
        constexpr std::string_view laneRide = "/godot/surface/laneRide";

        std::optional<LiveAddress> split (std::string_view address)
        {
            constexpr std::string_view godot = "/godot/";

            if (address == laneRide)
                return LiveAddress { "surfaces", {}, "laneRide" };

            if (address.substr (0, godot.size()) != godot)
                return std::nullopt;

            const auto rest = address.substr (godot.size());
            const auto first = rest.find ('/');

            if (first == std::string_view::npos)
                return std::nullopt;

            const auto second = rest.find ('/', first + 1);

            if (second == std::string_view::npos)
                return std::nullopt;

            const auto owner = rest.substr (0, first);
            const auto id = rest.substr (first + 1, second - first - 1);
            const auto row = rest.substr (second + 1);

            if (id.empty())
                return std::nullopt;

            const auto trim = row == "trim" && (owner == "run" || owner == "dca");
            const auto point = owner == "slot" && (row == "loopIn" || row == "loopOut");
            const auto sendRide = owner == "bus" && row == "laneRide";

            if (! trim && ! point && ! sendRide)
                return std::nullopt;

            return LiveAddress { std::string (owner), std::string (id), std::string (row) };
        }

        /*  THE ROW'S OWN PARSER, so a trim written here and a level written to
            the document are the same kind of number: the range, the type and
            the locale-free spelling are the parameter table's, and nothing here
            restates them. */
        std::optional<double> parseNumber (std::string_view owner, std::string_view rowName, const std::string& text)
        {
            for (const auto* row : doc::Schema::rowsForOwner (owner))
            {
                if (row->name != rowName)
                    continue;

                doc::Value value;
                const doc::Attribute attribute { owner, row };

                if (! doc::Schema::parseValue (attribute, text, value).ok)
                    return std::nullopt;

                if (value.isNumber())
                    return value.getNumber();

                if (value.isInteger())
                    return static_cast<double> (value.getInteger());

                return std::nullopt;
            }

            return std::nullopt;
        }
    }

    bool isLiveAddress (std::string_view address)
    {
        return split (address).has_value();
    }

    bool isLiveWrite (const std::string& commandName, const std::vector<osc::Value>& args)
    {
        return commandName == "node.set" && ! args.empty() && args.front().isString()
                 && isLiveAddress (args.front().getString());
    }

    doc::LiveWrite liveWriteFor (RunTable& runs, DcaTable& dcas, const doc::ShowDocument& document,
                                 TakeTable* takes, LaneTable* lanes)
    {
        return [&runs, &dcas, &document, takes, lanes] (const std::string& address, const std::string& text,
                                                        const std::vector<osc::Value>& args)
                   -> std::optional<Outcome>
        {
            const auto live = split (address);

            if (! live.has_value())
                return std::nullopt;

            /*  A LANE'S RIDE (namespace draft §34): the hand's level for the
                pass running, as it is heard (UK), which the Runner reads once
                the ride has been touched in it - the level's, or a send's by
                its mix. With no pass, or on a lane whose REC is off, it is a
                fader a hand is moving and nothing is recorded - applied, and
                ignored (DG, UL). */
            if (live->owner == "surfaces" || (live->owner == "bus" && live->row == "laneRide"))
            {
                const auto decibels = parseNumber (live->owner, "laneRide", text);

                if (! decibels.has_value())
                    return Outcome::rejected (reason::typeMismatch);

                const auto key = live->owner == "bus" ? live->id : std::string (levelLaneKey);

                if (lanes != nullptr && lanes->recording && lanes->isArmed (key))
                {
                    auto& ride = lanes->rideOf (key);
                    ride.handDb = *decibels;
                    ride.handSeen = true;
                }

                return Outcome::ok (args);
            }

            /*  A TAKE'S LOOP POINT (decision CQ): the channel's row parses it,
                the account keeps it in the take and two crossfades apart, and
                the hook hands the recorder both points. */
            if (live->owner == "slot")
            {
                const auto seconds = parseNumber ("rackChannel", live->row, text);

                if (! seconds.has_value())
                    return Outcome::rejected (reason::typeMismatch);

                const auto declared = document.findById (live->id);

                if (! declared.isValid() || declared.getType().toString() != "Channel")
                    return Outcome::rejected (reason::unknownId);

                if (takes != nullptr)
                    takes->setPoint (live->id, live->row == "loopIn", *seconds);

                return Outcome::ok (args);
            }

            const auto decibels = parseNumber (live->owner, "trim", text);

            if (! decibels.has_value())
                return Outcome::rejected (reason::typeMismatch);

            if (live->owner == "dca")
            {
                /*  A DCA THE SHOW DOES NOT DECLARE is a write aimed at nothing,
                    and unlike a finished run it will be aimed at nothing on
                    every write for the rest of the night - so it is said. */
                const auto declared = document.findById (live->id);

                if (! declared.isValid() || declared.getType().toString() != "Dca")
                    return Outcome::rejected (reason::unknownId);

                dcas.setByHand (live->id, *decibels);
                return Outcome::ok (args);
            }

            /*  A RUN THAT HAS GONE, OR NEVER WAS, IS APPLIED AND IGNORED. A
                surface a tick behind a clip that just ended is not making a
                mistake - its hand is still on the fader - and fifty refusals a
                second while it lets go would teach an operator to ignore the
                refusals that mean something. */
            if (auto* run = runs.find (live->id); run != nullptr && ! run->isFinished())
                run->trim = *decibels;

            return Outcome::ok (args);
        };
    }
}
