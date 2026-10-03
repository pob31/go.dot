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

#include <wfg/engine/cue/FadeMoveRows.h>

#include <wfg/engine/cue/FadeMoves.h>
#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <optional>

namespace wfg::cue
{
    namespace
    {
        constexpr const char* prefix = "/godot/cue/";
        constexpr const char* middle = "/moves/";

        struct MoveAddress
        {
            std::string fadeId, entry;
        };

        std::optional<MoveAddress> splitMove (const std::string& address)
        {
            if (address.rfind (prefix, 0) != 0)
                return std::nullopt;

            const auto rest = address.substr (std::string (prefix).size());
            const auto at = rest.find (middle);

            if (at == std::string::npos || at == 0 || rest.substr (0, at).find ('/') != std::string::npos)
                return std::nullopt;

            return MoveAddress { rest.substr (0, at), rest.substr (at + std::string (middle).size()) };
        }
    }

    bool isFadeMoveAddress (const std::string& address)
    {
        return splitMove (address).has_value();
    }

    doc::LiveWrite fadeMoveWriteFor (doc::ShowDocument& document, const plugin::CatalogueStore* catalogues)
    {
        return [&document, catalogues] (const std::string& address, const std::string& text,
                                        const std::vector<osc::Value>& args) -> std::optional<Outcome>
        {
            const auto target = splitMove (address);

            if (! target.has_value())
                return std::nullopt;

            const auto fade = document.findById (target->fadeId);

            if (! fade.isValid() || ! fade.hasType ("Fade"))
                return Outcome::rejected (reason::unknownId);

            std::string kind, name;
            int index = -1;

            if (! splitMoveEntry (target->entry, kind, name, index))
                return Outcome::rejected (reason::badAddress);

            //  What the entry names has to be this show's.
            if (kind == "send")
            {
                const auto bus = document.findById (name);

                if (! bus.isValid() || ! bus.hasType ("Bus"))
                    return Outcome::rejected (reason::unknownId);
            }
            else if (kind == "fx")
            {
                const auto entry = document.findById (name);

                if (! entry.isValid() || ! entry.hasType ("Plugin"))
                    return Outcome::rejected (reason::unknownId);

                if (catalogues != nullptr)
                    if (const auto catalogue = catalogues->find (entry.getProperty ("identifier").toString().toStdString()))
                        if (index >= static_cast<int> (catalogue->params.size()))
                            return Outcome::rejected (reason::badAddress);
            }

            const auto attribute = std::string (moveListAttribute (kind));
            auto values = parseMoveList (fade.getProperty (juce::Identifier (attribute)).toString().toStdString());
            const auto key = kind == "fx" ? name + "/" + std::to_string (index) : name;

            /*  EMPTY TAKES THE ENTRY OUT - the tick box cleared. One that is
                not there is no change, and still an answer. */
            if (text.empty())
            {
                if (values.erase (key) == 0)
                    return Outcome::ok (args);
            }
            else
            {
                const auto parsed = osc::parseDouble (text);

                if (! parsed.has_value() || ! std::isfinite (*parsed))
                    return Outcome::rejected (reason::typeMismatch);

                double lowest = 0.0, highest = 1.0;

                if (kind == "send")
                {
                    lowest = -120.0;
                    highest = 12.0;
                }
                else if (kind == "eq")
                {
                    eqRowRange (name, lowest, highest);
                }

                if (*parsed < lowest || *parsed > highest)
                    return Outcome::rejected (reason::typeMismatch);

                values[key] = *parsed;
            }

            /*  Through the ordinary door: the lock, the transaction and the
                coalescing are the document's. */
            const auto edit = document.setAttribute (std::string (prefix) + target->fadeId + "/" + attribute,
                                                     formatMoveList (values));

            if (! edit.ok)
                return Outcome::rejected (edit.reason);

            return Outcome::ok (args);
        };
    }
}
