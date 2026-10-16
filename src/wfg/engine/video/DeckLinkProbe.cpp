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

#include <wfg/engine/video/DeckLinkProbe.h>

namespace wfg::video
{
    namespace
    {
        const char* yesNo (bool value)  { return value ? "yes" : "no"; }

        std::string keyers (const card::Card& card)
        {
            if (card.internalKey && card.externalKey)  return "internal and external";
            if (card.internalKey)                      return "internal";
            if (card.externalKey)                      return "external";
            return "none";
        }

        void listFormats (std::ostream& out, const char* heading, const card::Card& card, bool output)
        {
            std::string line;

            for (const auto& mode : card.modes)
                if (output ? mode.output : mode.input)
                    line += (line.empty() ? "" : " ") + mode.word;

            out << "  " << heading << ": " << (line.empty() ? "none" : line) << "\n";
        }
    }

    int runDeckLinkProbe (card::System& cards, const std::vector<std::string>&, std::ostream& out)
    {
        const card::ThreadScope apartment;
        const auto found = cards.find();

        out << "Go.dot DeckLink probe\n";
        out << "Desktop Video: " << (found.version.empty() ? "not found" : found.version) << "\n";

        if (! found.problem.empty())
        {
            out << found.problem << "\n";
            return 2;
        }

        out << found.cards.size() << (found.cards.size() == 1 ? " card\n" : " cards\n");

        for (const auto& card : found.cards)
        {
            out << "\n" << card.name << "  (" << card.id << ")\n";
            out << "  sends pictures: " << yesNo (card.output) << "   takes pictures in: " << yesNo (card.input) << "\n";
            out << "  keyers: " << keyers (card) << "   10-bit: " << yesNo (card.tenBit)
                << "   sound channels: " << card.audioChannels << "\n";
            listFormats (out, "formats it sends", card, true);
            listFormats (out, "formats it takes in", card, false);
        }

        return 0;
    }
}
