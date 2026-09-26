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

#include <wfg/client/model/Surfaces.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view surfacePrefix = "/godot/surface/";
        constexpr std::string_view slotPrefix = "/godot/slot/";
        constexpr std::string_view dcaPrefix = "/godot/dca/";

        constexpr std::string_view surfaceOrderAddress = "/godot/surface/order";
        constexpr std::string_view surfaceAimAddress = "/godot/surface/aim";
        constexpr std::string_view dcaOrderAddress = "/godot/dca/order";

        /*  THE FOUR PROFILES, and the words a person reads for each. One table
            for the menu and for a surface nobody has named, so the two cannot
            call one box two things. The order is the menu's: the panel first,
            because it is the one that works with nothing plugged in. */
        const std::vector<std::pair<std::string, std::string>>& profiles()
        {
            static const std::vector<std::pair<std::string, std::string>> table
            {
                { "virtual",  "Virtual panel" },
                { "mcu",      "Mackie Control" },
                { "d700",     "Asparion D700" },
                { "midiPads", "Pads" },
            };

            return table;
        }

        /*  THE NODE'S OWN TYPED VALUE, read rather than parsed back out of
            text: the tree already knows a trim is a double and a count is an
            integer, and a second reading of the same fact is a second place for
            the two to disagree. A node that is absent, or is not the type its
            row declares, answers the fallback - which is the row's own default
            at every call below. */
        double numberOf (const tree::Node* node, double fallback)
        {
            if (node == nullptr)
                return fallback;

            const auto sole = node->soleValue();
            return sole.has_value() && sole->isNumber() ? sole->asDouble() : fallback;
        }

        int integerOf (const tree::Node* node, int fallback)
        {
            if (node == nullptr)
                return fallback;

            const auto sole = node->soleValue();

            if (! sole.has_value() || ! sole->isNumber())
                return fallback;

            return sole->isInt32() ? sole->getInt32() : static_cast<int> (sole->asDouble());
        }

        bool isTrue (const tree::Node* node, bool fallback)
        {
            if (node == nullptr)
                return fallback;

            const auto sole = node->soleValue();
            return sole.has_value() && sole->isBool() ? sole->getBool() : fallback;
        }

        /*  WHERE A NODE SITS UNDER ONE OF THE THREE PREFIXES: the identifier in
            the middle, and the row's own name after it. False for the
            container's own rows - `order` has no identifier in the middle - and
            for anything a level deeper, which belongs to something else. */
        bool splitAddress (const std::string& address, std::string_view prefix,
                           std::string& id, std::string& name)
        {
            if (address.rfind (prefix, 0) != 0)
                return false;

            const auto rest = address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string::npos)
                return false;

            name = rest.substr (slash + 1);

            if (name.find ('/') != std::string::npos)
                return false;

            id = rest.substr (0, slash);
            return true;
        }

        /*  IN THE ORDER THE SHOW DECLARES THEM, which is document order and the
            order a sampler group fills strips in - and never the identifiers'
            alphabet, which is an accident of what the engine drew. Anything the
            order does not name goes last rather than vanishing: a row nobody
            can see is a row nobody can delete. */
        template <typename Row>
        std::vector<Row> inOrder (std::map<std::string, Row>& found, const std::vector<std::string>& order)
        {
            std::vector<Row> rows;
            rows.reserve (found.size());

            for (const auto& id : order)
            {
                const auto at = found.find (id);

                if (at == found.end())
                    continue;

                rows.push_back (std::move (at->second));
                found.erase (at);
            }

            for (auto& entry : found)
                rows.push_back (std::move (entry.second));

            return rows;
        }
    }

    //==============================================================================
    std::string SurfaceRow::label() const
    {
        if (! name.empty())
            return name;

        for (const auto& choice : profiles())
            if (choice.first == profile)
                return choice.second;

        return profile.empty() ? id : profile;
    }

    std::string SurfaceRow::stateWord() const
    {
        /*  IN WORDS, NEVER COLOUR ALONE (§4.8), and the order is what matters.

            OFF FIRST, because it is the one somebody decided: a surface
            switched off is off whatever its cables are doing, and a row that
            said "connected" over it would be the window hiding a switch the
            operator threw. Then the one question the rest answer - is anything
            talking to it - where a virtual surface always is, being this
            client's own panel, and the engine publishes it so. A problem is the
            ENGINE'S sentence and is never rewritten here: a client inventing
            its own words for a refusal is a second place for them to be wrong. */
        if (! enabled)
            return "off";

        if (connected || profile == "virtual" || profile.empty())
            return "connected";

        if (! problem.empty())
            return problem;

        return "not connected";
    }

    std::string StripRow::label() const
    {
        /*  NEVER BLANK, AND NEVER CUT HERE. How many characters fit is the
            caller's to know - seven on a scribble strip, more on a panel - and
            an authored short name exists precisely so that nothing has to be
            cut by the machine (PRD §3.16). The em dash is a free strip, which
            is a thing to show rather than an absence. */
        if (role == "dca")
        {
            if (! dcaName.empty())
                return dcaName;

            return dca.empty() ? "—" : dca;
        }

        if (cue.empty())
            return "—";

        if (! cueShortName.empty())
            return cueShortName;

        if (! cueName.empty())
            return cueName;

        return cueNumber.empty() ? cue : cueNumber;
    }

    std::string DcaRow::label() const
    {
        if (! shortName.empty())
            return shortName;

        return name.empty() ? id : name;
    }

    //==============================================================================
    std::vector<SurfaceRow> readSurfaces (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS, gathering by identifier, as `readPorts` and
            `readDevices` do: `childrenOf` walks the whole tree per call and is
            banned for it. */
        std::map<std::string, SurfaceRow> found;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == surfaceOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, surfacePrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")              row.name = text (node);
            else if (name == "profile")      row.profile = text (node);
            else if (name == "ports")        row.ports = words (text (node));
            else if (name == "preset")       row.preset = text (node);
            else if (name == "enabled")      row.enabled = isTrue (node, true);
            else if (name == "strips")       row.strips = integerOf (node, 0);
            else if (name == "connected")    row.connected = isTrue (node, false);
            else if (name == "problem")      row.problem = text (node);
            else if (name == "serial")       row.serial = text (node);
            else if (name == "channel")      row.channel = integerOf (node, 0);
            else if (name == "firstNote")    row.firstNote = integerOf (node, 36);
        }

        return inOrder (found, words (order));
    }

    bool hasMasterDial (const tree::TreeSnapshot& snapshot)
    {
        for (const auto& surface : readSurfaces (snapshot))
            if (surface.enabled && (surface.profile == "d700" || surface.profile == "mcu"))
                return true;

        return false;
    }

    std::vector<StripRow> readStrips (const tree::TreeSnapshot& snapshot)
    {
        /*  ONE PASS for the strips themselves, which live among the other
            slots - a processor input, a rack channel - and are told apart by
            the `kind` the engine derives from the element. */
        std::map<std::string, StripRow> found;
        std::set<std::string> strips;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == surfaceOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, slotPrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "kind")
            {
                if (text (node) == "strip")
                    strips.insert (id);
            }
            else if (name == "surface")      row.surface = text (node);
            else if (name == "index")        row.index = integerOf (node, 0);
            else if (name == "role")         row.role = text (node);
            else if (name == "dca")          row.dca = text (node);
            else if (name == "endpoint")     row.endpoint = text (node);
            else if (name == "target")       row.target = text (node);
            else if (name == "word")         row.word = text (node);
            else if (name == "cue")          row.cue = text (node);
            else if (name == "holder")       row.holder = text (node);
        }

        std::vector<StripRow> rows;
        rows.reserve (strips.size());

        for (auto& entry : found)
            if (strips.count (entry.first) != 0)
                rows.push_back (std::move (entry.second));

        /*  WHAT A COLUMN DRAWS BESIDE WHAT THE ENGINE SAID, looked up where it
            lives rather than gathered in the pass above: a show has hundreds of
            cues and a desk has sixteen strips, so a handful of binary searches
            per strip is cheaper than holding four rows of every cue in the show
            on the chance that one of them is on a strip. */
        for (auto& row : rows)
        {
            if (! row.cue.empty())
            {
                const auto cueBase = "/godot/cue/" + row.cue + "/";

                row.cueName = text (snapshot, cueBase + "name");
                row.cueShortName = text (snapshot, cueBase + "shortName");
                row.cueNumber = text (snapshot, cueBase + "number");
                row.cueColour = text (snapshot, cueBase + "colour");
            }

            /*  THE HOLDER'S, NOT THE CUE'S: what the clip sounds like where it
                has got to (PRD §3.30), and whether a pad is down on it. Both
                belong to a run, and a strip with only a waiter on it has
                neither. */
            if (! row.holder.empty())
            {
                const auto runBase = "/godot/run/" + row.holder + "/";

                row.timbre = text (snapshot, runBase + "timbre");
                row.held = isTrue (snapshot.find (runBase + "held"), false);
            }

            /*  A DCA STRIP IS CALLED WHAT ITS DCA IS, the short name first: that
                is what a scribble strip shows, and a short name is written for
                exactly that (§3.16). The ROLE decides and not the row - a
                sampler strip may still carry the `dca` somebody set before
                changing its role, and the engine reads it only on a dca strip. */
            if (row.role == "dca" && ! row.dca.empty())
            {
                const auto dcaBase = std::string (dcaPrefix) + row.dca + "/";
                const auto authored = text (snapshot, dcaBase + "shortName");

                row.dcaName = authored.empty() ? text (snapshot, dcaBase + "name") : authored;
            }

            /*  WHAT IS UNDER THE FADER NOW, read off the node the fader rides -
                a run's trim, or a DCA's - so a panel draws the value the engine
                holds rather than the last one this hand sent. A strip riding
                nothing has no level, which is not a level of silence, and a
                column says so by drawing no position at all. */
            if (row.target.empty())
                continue;

            if (const auto* riding = snapshot.find (row.target); riding != nullptr)
            {
                if (const auto sole = riding->soleValue(); sole.has_value() && sole->isNumber())
                {
                    row.hasLevel = true;
                    row.levelDb = sole->asDouble();
                }
            }
        }

        /*  SURFACE ORDER, THEN INDEX: the order a sampler group fills strips
            in, left to right across every surface (/godot/surface/order's own
            description), so the panel's columns are in the order the strips
            are used. A strip whose surface the order does not name goes last,
            by surface and then by index, rather than vanishing. */
        const auto surfaceOrder = words (order);
        std::map<std::string, std::size_t> placeOf;

        for (std::size_t at = 0; at < surfaceOrder.size(); ++at)
            placeOf.emplace (surfaceOrder[at], at);

        const auto place = [&placeOf, &surfaceOrder] (const std::string& surfaceId)
        {
            const auto known = placeOf.find (surfaceId);
            return known != placeOf.end() ? known->second : surfaceOrder.size();
        };

        std::stable_sort (rows.begin(), rows.end(),
                          [&place] (const StripRow& a, const StripRow& b)
                          {
                              if (place (a.surface) != place (b.surface))
                                  return place (a.surface) < place (b.surface);

                              if (a.surface != b.surface)
                                  return a.surface < b.surface;

                              if (a.index != b.index)
                                  return a.index < b.index;

                              return a.id < b.id;
                          });

        return rows;
    }

    std::vector<StripRow> stripsOf (const std::vector<StripRow>& strips, const std::string& surfaceId)
    {
        std::vector<StripRow> out;

        for (const auto& strip : strips)
            if (strip.surface == surfaceId)
                out.push_back (strip);

        /*  Sorted here as well, although `readStrips` already did: a caller
            that filtered or rebuilt the list its own way still gets the order
            the hardware has, and sixteen rows cost nothing to sort. */
        std::stable_sort (out.begin(), out.end(),
                          [] (const StripRow& a, const StripRow& b) { return a.index < b.index; });

        return out;
    }

    std::vector<DcaRow> readDcas (const tree::TreeSnapshot& snapshot)
    {
        std::map<std::string, DcaRow> found;
        std::string order;

        for (const auto* node : snapshot.all())
        {
            if (node->address == dcaOrderAddress)
            {
                order = text (node);
                continue;
            }

            std::string id, name;

            if (! splitAddress (node->address, dcaPrefix, id, name))
                continue;

            auto& row = found[id];
            row.id = id;

            if (name == "name")              row.name = text (node);
            else if (name == "shortName")    row.shortName = text (node);
            else if (name == "dca")          row.parent = text (node);
            else if (name == "trim")         row.trimDb = numberOf (node, 0.0);
        }

        return inOrder (found, words (order));
    }

    std::vector<std::pair<std::string, std::string>> dcaChoices (const std::vector<DcaRow>& dcas)
    {
        std::vector<std::pair<std::string, std::string>> choices;
        choices.reserve (dcas.size() + 1);

        /*  EMPTY IS A CHOICE AND NOT AN ABSENCE, as it is for a port and for an
            output: a cue marked with no DCA is what every cue is until somebody
            assigns one, and it has to be possible to go back to. First, because
            that is where a hand goes looking for it. */
        choices.push_back ({ "", "(none)" });

        /*  THE WHOLE NAME IN A MENU, and the short one only when there is no
            other. A short name is written for a seven-character scribble strip
            (§3.16), which is why `label()` puts it first; a menu has the room
            for "Ambiences" that a strip does not. The key is the identifier
            either way, because that is what the row stores - renaming a DCA
            must never unassign the cues marked with it. */
        for (const auto& dca : dcas)
            choices.push_back ({ dca.id, dca.name.empty() ? dca.label() : dca.name });

        return choices;
    }

    std::vector<std::pair<std::string, std::string>> stripChoices (const tree::TreeSnapshot& snapshot,
                                                                   const std::string& cueId)
    {
        const auto base = "/godot/cue/" + cueId + "/";
        const auto pin = text (snapshot, base + "strip");
        const auto now = text (snapshot, base + "stripNow");

        const auto strips = readStrips (snapshot);
        const auto surfaces = readSurfaces (snapshot);

        /*  "Asparion D700 · fader 3", "Pads · pad 5": the surface, then the
            strip as its hardware is counted, from one - and fader or pad,
            since the author asked for both in one menu. */
        const auto stripWords = [&surfaces] (const StripRow& strip)
        {
            std::string surface = strip.surface;

            for (const auto& row : surfaces)
                if (row.id == strip.surface)
                    surface = row.label();

            return surface + " · " + (strip.endpoint == "gate" ? "pad " : "fader ")
                   + std::to_string (strip.index + 1);
        };

        const auto cueWords = [&snapshot] (const std::string& id)
        {
            auto name = text (snapshot, "/godot/cue/" + id + "/name");

            if (name.empty())
                name = text (snapshot, "/godot/cue/" + id + "/number");

            return "\"" + (name.empty() ? id : name) + "\"";
        };

        //  What the list put on each strip before this member's group: <strip> <cue> pairs.
        std::map<std::string, std::string> before;
        {
            const auto pairs = words (text (snapshot, base + "stripsBefore"));

            for (std::size_t at = 0; at + 1 < pairs.size(); at += 2)
                before[pairs[at]] = pairs[at + 1];
        }

        //  Where the other members of this group are played from now.
        std::map<std::string, std::string> sibling;
        {
            const auto parent = text (snapshot, base + "parent");

            if (! parent.empty())
                for (const auto& member : words (text (snapshot, "/godot/cue/" + parent + "/order")))
                    if (member != cueId)
                        if (const auto on = text (snapshot, "/godot/cue/" + member + "/stripNow");
                            ! on.empty())
                            sibling[on] = member;
        }

        std::vector<std::pair<std::string, std::string>> choices;

        /*  AUTOMATIC FIRST, AND IT SAYS WHERE THAT IS: a member on no pin is
            on the next strip free in member order, and the menu names it so
            the choice between "wherever" and "here" is made knowing both. */
        std::string automatic = "automatic";

        if (! pin.empty())
            automatic += ", on the next strip free";
        else if (now.empty())
            automatic += " \xe2\x80\x94 no strip left";
        else
        {
            for (const auto& strip : strips)
                if (strip.id == now)
                    automatic += " \xe2\x80\x94 " + stripWords (strip);
        }

        choices.push_back ({ "", automatic });

        auto pinListed = pin.empty();

        for (const auto& strip : strips)
        {
            if (strip.role != "sampler")
                continue;

            if (strip.id == pin)
                pinListed = true;

            auto label = stripWords (strip) + " \xe2\x80\x94 ";

            if (const auto found = sibling.find (strip.id); found != sibling.end())
                label += cueWords (found->second) + " in this group";
            else if (const auto earlier = before.find (strip.id); earlier != before.end())
                label += "previously " + cueWords (earlier->second);
            else
                label += "free";

            choices.push_back ({ strip.id, label });
        }

        /*  A PIN THAT NAMES NO SAMPLER STRIP - deleted since, or made a DCA
            strip - is still shown as what is written, with what it does:
            nothing, the member is placed automatically. A menu that could not
            show the current value would read as an empty one. */
        if (! pinListed)
            choices.push_back ({ pin, pin + " \xe2\x80\x94 not a sampler strip: played from the next strip free" });

        return choices;
    }

    std::vector<std::pair<std::string, std::string>> profileChoices()
    {
        return profiles();
    }

    //==========================================================================
    SurfacePage readSurfacePage (const tree::TreeSnapshot& snapshot)
    {
        SurfacePage out;
        out.aim = text (snapshot, std::string (surfaceAimAddress));

        const auto order = text (snapshot, std::string (surfaceOrderAddress));
        std::size_t at = 0;

        while (at < order.size())
        {
            const auto start = order.find_first_not_of (' ', at);

            if (start == std::string::npos)
                break;

            const auto end = order.find (' ', start);
            const auto id = order.substr (start, end == std::string::npos ? std::string::npos : end - start);
            at = end == std::string::npos ? order.size() : end;

            const auto base = std::string (surfacePrefix) + id + "/";
            const auto word = text (snapshot, base + "page");

            if (word != "eq" && word != "send" && word != "fx")
                continue;

            out.up = true;
            out.surface = id;
            out.word = word;
            out.index = static_cast<int> (osc::parseDouble (text (snapshot, base + "pageIndex")).value_or (0.0));
            out.count = static_cast<int> (osc::parseDouble (text (snapshot, base + "pageCount")).value_or (1.0));
            out.edited = text (snapshot, base + "edited");
            return out;
        }

        return out;
    }
}
