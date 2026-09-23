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

#include <wfg/engine/cue/FxRows.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>

namespace wfg::cue
{
    namespace
    {
        constexpr const char* prefix = "/godot/fx/";

        struct FxAddress
        {
            std::string id;
            int index = -1;
        };

        std::optional<FxAddress> splitFx (const std::string& address)
        {
            if (address.rfind (prefix, 0) != 0)
                return std::nullopt;

            const auto rest = address.substr (std::string (prefix).size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos || slash == 0)
                return std::nullopt;

            const auto leaf = rest.substr (slash + 1);

            if (leaf.size() < 2 || leaf.front() != 'p' || leaf.find ('/') != std::string::npos)
                return std::nullopt;

            for (std::size_t i = 1; i < leaf.size(); ++i)
                if (leaf[i] < '0' || leaf[i] > '9')
                    return std::nullopt;

            if (leaf.size() > 2 && leaf[1] == '0')
                return std::nullopt;   // p01 is not a node; p0 is

            FxAddress out;
            out.id = rest.substr (0, slash);
            out.index = std::atoi (leaf.c_str() + 1);
            return out;
        }
    }

    //==============================================================================
    bool isFxParameterAddress (const std::string& address)
    {
        return splitFx (address).has_value();
    }

    std::map<int, double> parseFxValues (const std::string& text)
    {
        std::map<int, double> out;
        std::istringstream in (text);
        std::string pair;

        while (in >> pair)
        {
            const auto colon = pair.find (':');

            if (colon == std::string::npos || colon == 0)
                continue;

            /*  The index is digits and nothing else: atoi reads "x" as nought,
                which would hand a stray word to parameter nought. */
            const auto digits = pair.substr (0, colon);
            const auto numeric = ! digits.empty()
                                   && std::all_of (digits.begin(), digits.end(),
                                                   [] (char c) { return c >= '0' && c <= '9'; });

            if (! numeric || digits.size() > 6)
                continue;

            const auto index = std::atoi (digits.c_str());
            const auto value = osc::parseDouble (pair.substr (colon + 1));

            if (value.has_value() && std::isfinite (*value))
                out[index] = std::clamp (*value, 0.0, 1.0);
        }

        return out;
    }

    std::string formatFxValues (const std::map<int, double>& values)
    {
        std::string out;

        for (const auto& [index, value] : values)
        {
            if (! out.empty())
                out += ' ';

            out += std::to_string (index) + ":" + osc::formatDouble (value);
        }

        return out;
    }

    //==============================================================================
    doc::LiveWrite fxWriteFor (doc::ShowDocument& document, const plugin::CatalogueStore* catalogues)
    {
        return [&document, catalogues] (const std::string& address, const std::string& text,
                                        const std::vector<osc::Value>& args) -> std::optional<Outcome>
        {
            const auto target = splitFx (address);

            if (! target.has_value())
                return std::nullopt;

            const auto fx = document.findById (target->id);

            if (! fx.isValid() || ! fx.hasType ("Fx"))
                return Outcome::rejected (reason::unknownId);

            const auto parsed = osc::parseDouble (text);

            if (! parsed.has_value() || ! std::isfinite (*parsed) || *parsed < 0.0 || *parsed > 1.0)
                return Outcome::rejected (reason::typeMismatch);

            /*  A parameter the catalogue says the plugin does not have is a
                node that does not exist - when the catalogue is there to say. */
            if (catalogues != nullptr)
            {
                const auto entry = document.findById (fx.getProperty ("plugin").toString().toStdString());

                if (entry.isValid() && entry.hasType ("Plugin"))
                    if (const auto catalogue = catalogues->find (entry.getProperty ("identifier").toString().toStdString()))
                        if (target->index >= static_cast<int> (catalogue->params.size()))
                            return Outcome::rejected (reason::badAddress);
            }

            auto values = parseFxValues (fx.getProperty ("values").toString().toStdString());
            values[target->index] = *parsed;

            /*  Through the ordinary door: the lock, the transaction and the
                coalescing are the document's, and the record in the log is
                this node.set, applied. */
            const auto edit = document.setAttribute (std::string (prefix) + target->id + "/values",
                                                     formatFxValues (values));

            if (! edit.ok)
                return Outcome::rejected (edit.reason);

            return Outcome::ok (args);
        };
    }

    doc::LiveWrite eitherOf (doc::LiveWrite first, doc::LiveWrite second)
    {
        return [first = std::move (first), second = std::move (second)]
               (const std::string& address, const std::string& text,
                const std::vector<osc::Value>& args) -> std::optional<Outcome>
        {
            if (first)
                if (auto outcome = first (address, text, args))
                    return outcome;

            if (second)
                return second (address, text, args);

            return std::nullopt;
        };
    }
}
