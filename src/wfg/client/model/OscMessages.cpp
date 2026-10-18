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

#include <wfg/client/model/OscMessages.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        constexpr std::string_view messagePrefix = "/godot/message/";
        constexpr std::string_view curvePrefix = "/godot/curve/";

        /*  An address's id and row under a prefix, or nothing. */
        bool split (std::string_view address, std::string_view prefix, std::string& id, std::string& row)
        {
            if (address.rfind (prefix, 0) != 0)
                return false;

            const auto rest = address.substr (prefix.size());
            const auto slash = rest.find ('/');

            if (slash == std::string_view::npos)
                return false;

            id = std::string (rest.substr (0, slash));
            row = std::string (rest.substr (slash + 1));
            return true;
        }

        /*  What the value box shows of one value: the number as the atom spells
            it, the words unescaped, nothing for a type that carries none. */
        std::string payloadOf (const osc::Value& value)
        {
            if (value.isString())
                return value.getString();

            const auto atom = value.toAtom();
            return atom.size() > 2 && atom[1] == ':' ? atom.substr (2) : std::string {};
        }

        void fillArguments (OscMessageRow& row)
        {
            const auto values = osc::valuesFromAtoms (row.value);
            row.spells = values.has_value();
            row.args.clear();

            if (! values.has_value())
                return;

            for (const auto& value : *values)
            {
                OscArgument argument;
                argument.tag = value.typeTag();
                argument.payload = payloadOf (value);
                argument.number = value.isNumber();
                row.args.push_back (std::move (argument));
            }
        }
    }

    namespace
    {
        /*  ONE PASS OVER THE WHOLE SNAPSHOT, the way the messages were read until
            §88 - kept for a snapshot that names no `messages` row for the cue,
            which a tree made by hand in a test may not. */
        OscMessagesReading readByWalk (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            OscMessagesReading out;
            out.cueId = cueId;

            if (cueId.empty() || text (snapshot, "/godot/cue/" + cueId + "/kind") != "osc")
                return out;

            out.locked = isYes (flag (snapshot, "/godot/document/locked"));

            OscMessageRow own;
            own.base = "/godot/cue/" + cueId + "/";
            own.address = text (snapshot, own.base + "address");
            own.value = text (snapshot, own.base + "value");

            /*  ONE PASS for the further messages and every curve, gathered by
                identifier and kept to this cue - the shape `readRanges` uses. */
            struct Further
            {
                OscMessageRow row;
                std::string cue;
                int index = 0;
            };

            struct Curve
            {
                std::string cue, message;
                int arg = 0;
            };

            std::map<std::string, Further> messages;
            std::map<std::string, Curve> curves;

            for (const auto* node : snapshot.all())
            {
                std::string id, row;

                if (split (node->address, messagePrefix, id, row))
                {
                    auto& message = messages[id];
                    message.row.id = id;
                    message.row.base = std::string (messagePrefix) + id + "/";

                    if (row == "cue")           message.cue = text (node);
                    else if (row == "address")  message.row.address = text (node);
                    else if (row == "value")    message.row.value = text (node);
                    else if (row == "index")    message.index = static_cast<int> (osc::parseDouble (text (node)).value_or (0.0));
                }
                else if (split (node->address, curvePrefix, id, row))
                {
                    auto& curve = curves[id];

                    if (row == "cue")           curve.cue = text (node);
                    else if (row == "message")  curve.message = text (node);
                    else if (row == "arg")      curve.arg = static_cast<int> (osc::parseDouble (text (node)).value_or (0.0));
                }
            }

            std::vector<Further> mine;

            for (auto& [id, message] : messages)
                if (message.cue == cueId)
                    mine.push_back (std::move (message));

            std::stable_sort (mine.begin(), mine.end(),
                              [] (const Further& a, const Further& b) { return a.index < b.index; });

            out.rows.push_back (std::move (own));

            for (auto& message : mine)
                out.rows.push_back (std::move (message.row));

            for (auto& row : out.rows)
                fillArguments (row);

            //  Each curve on the value it moves, of the message it sits under.
            for (const auto& [id, curve] : curves)
            {
                if (curve.cue != cueId || curve.arg < 0)
                    continue;

                for (auto& row : out.rows)
                    if (row.id == curve.message && static_cast<std::size_t> (curve.arg) < row.args.size())
                        row.args[static_cast<std::size_t> (curve.arg)].curveId = id;
            }

            return out;
        }

        /*  BY ADDRESS (namespace draft §88): the cue's `messages` row names its
            further messages in their order, the cue's and each message's
            `curves` row its curves, and every row is looked up where it is.
            The walk above went over every node of the tree to find them - every
            message of every cue - twice a pass, twenty-five passes a second. */
        OscMessagesReading readByAddress (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                                          const std::string& listed)
        {
            OscMessagesReading out;
            out.cueId = cueId;
            out.locked = isYes (flag (snapshot, "/godot/document/locked"));

            OscMessageRow own;
            own.base = "/godot/cue/" + cueId + "/";
            own.address = text (snapshot, own.base + "address");
            own.value = text (snapshot, own.base + "value");
            out.rows.push_back (std::move (own));

            for (const auto& id : words (listed))
            {
                OscMessageRow row;
                row.id = id;
                row.base = std::string (messagePrefix) + id + "/";
                row.address = text (snapshot, row.base + "address");
                row.value = text (snapshot, row.base + "value");
                out.rows.push_back (std::move (row));
            }

            for (auto& row : out.rows)
            {
                fillArguments (row);

                //  Each curve on the value it moves.
                for (const auto& curveId : words (text (snapshot, row.base + "curves")))
                {
                    const auto arg = static_cast<int> (osc::parseDouble (text (snapshot, std::string (curvePrefix) + curveId + "/arg"))
                                                         .value_or (-1.0));

                    if (arg >= 0 && static_cast<std::size_t> (arg) < row.args.size())
                        row.args[static_cast<std::size_t> (arg)].curveId = curveId;
                }
            }

            return out;
        }
    }

    OscMessagesReading readOscMessages (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        return *sharedOscMessages (snapshot, cueId);
    }

    std::shared_ptr<const OscMessagesReading> sharedOscMessages (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        if (cueId.empty() || text (snapshot, "/godot/cue/" + cueId + "/kind") != "osc")
        {
            OscMessagesReading none;
            none.cueId = cueId;
            return std::make_shared<const OscMessagesReading> (std::move (none));
        }

        const auto base = "/godot/cue/" + cueId + "/";
        const auto* listed = snapshot.find (base + "messages");

        if (listed == nullptr)
            return std::make_shared<const OscMessagesReading> (readByWalk (snapshot, cueId));

        /*  KEPT WHILE NOTHING IT WAS READ FROM HAS MOVED (namespace draft §88):
            the messages half the same object, and the cue's own rows - its
            address, value, list of messages and curves - and the lock as they
            were. A GO rebuilds the rest of the show's half and leaves these
            alone, so a cue of 5,300 messages is read again only when an edit
            touched it. One reading kept, for the cue being looked at. */
        struct Kept
        {
            std::shared_ptr<const std::vector<tree::Node>> half;
            std::string cueId, address, value, messages, curves;
            bool locked = false;
            std::shared_ptr<const OscMessagesReading> reading;
        };

        static thread_local Kept kept;

        const auto& half = snapshot.messagesHalf();
        const auto address = text (snapshot, base + "address");
        const auto value = text (snapshot, base + "value");
        const auto messages = text (listed);
        const auto curves = text (snapshot, base + "curves");
        const auto locked = isYes (flag (snapshot, "/godot/document/locked"));

        if (half != nullptr && half == kept.half && cueId == kept.cueId && address == kept.address
              && value == kept.value && messages == kept.messages && curves == kept.curves && locked == kept.locked)
            return kept.reading;

        auto reading = std::make_shared<const OscMessagesReading> (readByAddress (snapshot, cueId, messages));

        if (half != nullptr)
            kept = Kept { half, cueId, address, value, messages, curves, locked, reading };

        return reading;
    }

    //==============================================================================
    const std::vector<std::pair<char, std::string>>& argumentTypes()
    {
        /*  The numbers first, as what a curve moves; then words and the four
            values that carry nothing of their own. */
        static const std::vector<std::pair<char, std::string>> types {
            { 'f', "float" }, { 'i', "integer" }, { 'd', "double" }, { 'h', "long" },
            { 's', "text" }, { 'T', "true" }, { 'F', "false" }, { 'N', "nil" }, { 'I', "impulse" } };

        return types;
    }

    std::optional<std::string> atomFor (char tag, const std::string& typed)
    {
        const auto number = osc::parseDouble (typed);

        /*  A WHOLE NUMBER ONLY, for the integers: "2.5" as an integer would be
            a value nobody typed. */
        const auto whole = [&number] (double low, double high) -> bool
        {
            double integral = 0.0;
            return number.has_value() && ! (std::fabs (std::modf (*number, &integral)) > 0.0)
                     && *number >= low && *number <= high;
        };

        switch (tag)
        {
            case 'f':
                if (! number.has_value() || ! std::isfinite (static_cast<float> (*number)))
                    return std::nullopt;
                return osc::Value::float32 (static_cast<float> (*number)).toAtom();

            case 'd':
                if (! number.has_value())
                    return std::nullopt;
                return osc::Value::float64 (*number).toAtom();

            case 'i':
                if (! whole (std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()))
                    return std::nullopt;
                return osc::Value::int32 (static_cast<std::int32_t> (*number)).toAtom();

            case 'h':
                if (! whole (-9.0e15, 9.0e15))
                    return std::nullopt;
                return osc::Value::int64 (static_cast<std::int64_t> (*number)).toAtom();

            case 's':  return osc::Value::string (typed).toAtom();
            case 'T':  return std::string ("T");
            case 'F':  return std::string ("F");
            case 'N':  return std::string ("N");
            case 'I':  return std::string ("I");
            default:   return std::nullopt;
        }
    }

    std::optional<std::string> withArgument (const std::string& list, std::size_t index, const std::string& atom)
    {
        auto values = osc::valuesFromAtoms (list);
        const auto replacement = osc::Value::fromAtom (atom);

        if (! values.has_value() || ! replacement.has_value() || index >= values->size())
            return std::nullopt;

        (*values)[index] = *replacement;
        return osc::atomsOf (*values);
    }

    std::optional<std::string> withArgumentAppended (const std::string& list, const std::string& atom)
    {
        auto values = osc::valuesFromAtoms (list);
        const auto added = osc::Value::fromAtom (atom);

        if (! values.has_value() || ! added.has_value())
            return std::nullopt;

        values->push_back (*added);
        return osc::atomsOf (*values);
    }

    std::optional<std::string> withoutArgument (const std::string& list, std::size_t index)
    {
        auto values = osc::valuesFromAtoms (list);

        if (! values.has_value() || index >= values->size())
            return std::nullopt;

        values->erase (values->begin() + static_cast<std::ptrdiff_t> (index));
        return osc::atomsOf (*values);
    }

    std::optional<std::string> retyped (const std::string& list, std::size_t index, char tag)
    {
        const auto values = osc::valuesFromAtoms (list);

        if (! values.has_value() || index >= values->size())
            return std::nullopt;

        const auto said = payloadOf ((*values)[index]);

        if (auto atom = atomFor (tag, said))
            return withArgument (list, index, *atom);

        //  Its resting value where what it said cannot be said in the new type.
        if (auto resting = atomFor (tag, tag == 's' ? std::string {} : std::string ("0")))
            return withArgument (list, index, *resting);

        return std::nullopt;
    }
}
