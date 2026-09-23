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
#include <wfg/engine/cue/Run.h>
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
        };

        std::optional<LiveAddress> split (std::string_view address)
        {
            constexpr std::string_view godot = "/godot/";

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

            if (row != "trim" || id.empty() || (owner != "run" && owner != "dca"))
                return std::nullopt;

            return LiveAddress { std::string (owner), std::string (id) };
        }

        /*  THE ROW'S OWN PARSER, so a trim written here and a level written to
            the document are the same kind of number: the range, the type and
            the locale-free spelling are the parameter table's, and nothing here
            restates them. */
        std::optional<double> parseTrim (std::string_view owner, const std::string& text)
        {
            for (const auto* row : doc::Schema::rowsForOwner (owner))
            {
                if (row->name != "trim")
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

    doc::LiveWrite liveWriteFor (RunTable& runs, DcaTable& dcas, const doc::ShowDocument& document)
    {
        return [&runs, &dcas, &document] (const std::string& address, const std::string& text,
                                          const std::vector<osc::Value>& args)
                   -> std::optional<Outcome>
        {
            const auto live = split (address);

            if (! live.has_value())
                return std::nullopt;

            const auto decibels = parseTrim (live->owner, text);

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

                dcas.set (live->id, *decibels);
                return Outcome::ok (args);
            }

            /*  A RUN THAT HAS GONE, OR NEVER WAS, IS APPLIED AND IGNORED. A
                surface a tick behind a clip that just ended is not making a
                mistake - its hand is still on the fader - and fifty refusals a
                second while it lets go would teach an operator to ignore the
                refusals that mean something. */
            if (auto* run = runs.find (live->id); run != nullptr && ! run->isFinished())
            {
                run->trim = *decibels;
                run->ridden = true;
            }

            return Outcome::ok (args);
        };
    }
}
