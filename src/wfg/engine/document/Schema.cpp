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

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/SchemaTable.generated.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace wfg::doc
{
    //==============================================================================
    bool Attribute::isEnumValue (std::string_view v) const noexcept
    {
        for (std::size_t i = 0; i < row->numEnumValues; ++i)
            if (row->enumValues[i] == v)
                return true;

        return false;
    }

    bool Attribute::isInRange (double value) const noexcept
    {
        if (row->hasMin && value < row->minimum)
            return false;

        if (row->hasMax && value > row->maximum)
            return false;

        return true;
    }

    const Attribute* Element::attribute (std::string_view attributeName) const
    {
        const auto it = std::find_if (attributes.begin(), attributes.end(),
                                      [attributeName] (const Attribute& a)
                                      { return a.name() == attributeName; });

        return it == attributes.end() ? nullptr : &*it;
    }

    bool Element::mayContain (std::string_view childName) const
    {
        return std::find (childElements.begin(), childElements.end(), childName)
                 != childElements.end();
    }

    //==============================================================================
    namespace
    {
        /*  CONTAINMENT, and it is the only structural fact in this layer that
            the parameter table does not carry.

            The table has one row per attribute; "a List may hold Cues and
            Groups" is a property of elements, so writing it here is honest
            rather than lazy. It is also short enough to read in one go, which a
            thirteenth CSV column would not be.

            The shape is PRD §3.5 and §3.6: a List has the same child grammar as
            a Group, because "the cue list is, virtually, a sequence group in
            manual mode - one model, no special case at the top".

                Show
                  Lists
                    List           <- id, name
                      Cue          <- id and the cue attributes
                      Group        <- a Cue that also holds Cues and Groups
                  Mounts
                    Mount          <- id, a foreign namespace

            Containers (Lists, Mounts) carry nothing and exist so the file has
            somewhere obvious to put a new List, and so a diff of one list does
            not touch the mounts.
        */
        struct Containment
        {
            std::string_view element;
            bool hasIdentity;
            std::vector<std::string_view> children;
            std::vector<std::string_view> attributeOwners;   // which table rows land here
        };

        const std::vector<Containment>& containmentTable()
        {
            static const std::vector<Containment> table {
                { "Show",   false, { "Lists", "Mounts" },  { "document" } },
                { "Lists",  false, { "List" },             {} },
                { "List",   true,  { "Cue", "Group" },     { "list" } },
                { "Cue",    true,  {},                     { "cue" } },
                { "Group",  true,  { "Cue", "Group" },     { "cue", "group" } },
                { "Mounts", false, { "Mount" },            {} },
                { "Mount",  true,  {},                     { "mount" } },
            };

            return table;
        }
    }

    //==============================================================================
    Schema::Schema()
    {
        for (const auto& c : containmentTable())
        {
            Element element;
            element.name = c.element;
            element.hasIdentity = c.hasIdentity;
            element.childElements = c.children;

            /*  Rows land on an element when their owner is one this element
                takes AND they persist somewhere. A row that persists nowhere is
                a runtime projection - a cue's index among its siblings, the
                engine's tick - published by the parameter tree and held in no
                file. Those rows stay in the generated table because Phase 1.5
                needs them; they are simply not document attributes.

                Owners are visited in the order the containment table lists
                them, so a Group's own rows follow the Cue rows it inherits,
                which is the order they read best in. */
            for (const auto& owner : c.attributeOwners)
                for (const auto& row : generated::attributes)
                    if (row.owner == owner && row.persist != Persist::none)
                        element.attributes.push_back (Attribute { c.element, &row });

            elementList.push_back (std::move (element));
        }
    }

    const Schema& Schema::instance()
    {
        /*  Function-local static: built on first use, thread-safe since C++11,
            and with no dependence on the order static objects elsewhere are
            constructed. */
        static const Schema schema;
        return schema;
    }

    const Element* Schema::element (std::string_view name) const
    {
        const auto it = std::find_if (elementList.begin(), elementList.end(),
                                      [name] (const Element& e) { return e.name == name; });

        return it == elementList.end() ? nullptr : &*it;
    }

    const Attribute* Schema::attribute (std::string_view elementName,
                                        std::string_view attributeName) const
    {
        const auto* e = element (elementName);
        return e == nullptr ? nullptr : e->attribute (attributeName);
    }

    //==============================================================================
    Value Value::string (std::string v)
    {
        Value out;
        out.valueType = ValueType::string;
        out.text = std::move (v);
        return out;
    }

    Value Value::integer (long long v)
    {
        Value out;
        out.valueType = ValueType::integer;
        out.integerValue = v;
        return out;
    }

    Value Value::number (double v)
    {
        Value out;
        out.valueType = ValueType::number;
        out.numberValue = v;
        return out;
    }

    Value Value::boolean (bool v)
    {
        Value out;
        out.valueType = ValueType::boolean;
        out.booleanValue = v;
        return out;
    }

    bool Value::operator== (const Value& other) const noexcept
    {
        if (valueType != other.valueType)
            return false;

        switch (valueType)
        {
            case ValueType::string:    return text == other.text;
            case ValueType::integer:
            case ValueType::integer64: return integerValue == other.integerValue;
            case ValueType::boolean:   return booleanValue == other.booleanValue;
            case ValueType::number:
            {
                /*  Bit comparison, not numeric: a replay compares documents, and
                    two numbers that print the same must not be called equal if
                    they are different doubles. It also keeps -Wfloat-equal
                    quiet, which is on under the strict preset. */
                const auto a = numberValue;
                const auto b = other.numberValue;
                return std::memcmp (&a, &b, sizeof (double)) == 0;
            }
            case ValueType::blob:      return true;   // nothing in Phase 1 holds one
        }

        return false;
    }

    //==============================================================================
    Schema::Parsed Schema::parseValue (const Attribute& attribute, std::string_view text,
                                       Value& out)
    {
        Parsed result;

        const auto reject = [&result] (std::string why)
        {
            result.ok = false;
            result.error = std::move (why);
            return result;
        };

        switch (attribute.type())
        {
            case ValueType::string:
            {
                /*  An enum is a string with a list. Rejecting an unknown value
                    rather than keeping it is the point: a mode of "sequnce" is a
                    typo that would otherwise sit in the file behaving like a
                    timeline. */
                if (attribute.isEnum() && ! attribute.isEnumValue (text))
                {
                    std::string allowed;

                    for (std::size_t i = 0; i < attribute.row->numEnumValues; ++i)
                    {
                        if (! allowed.empty())
                            allowed += '|';

                        allowed += std::string (attribute.row->enumValues[i]);
                    }

                    return reject ("expected one of " + allowed + ", found \""
                                   + std::string (text) + "\"");
                }

                out = Value::string (std::string (text));
                break;
            }

            case ValueType::integer:
            case ValueType::integer64:
            {
                long long parsed = 0;
                const auto* first = text.data();
                const auto* last = text.data() + text.size();
                const auto r = std::from_chars (first, last, parsed);

                if (r.ec != std::errc {} || r.ptr != last)
                    return reject ("expected a whole number, found \"" + std::string (text) + "\"");

                if (! attribute.isInRange (static_cast<double> (parsed)))
                    return reject ("out of range: " + std::string (text));

                out = Value::integer (parsed);
                break;
            }

            case ValueType::number:
            {
                const auto parsed = osc::parseDouble (text);

                if (! parsed)
                    return reject ("expected a number, found \"" + std::string (text) + "\"");

                if (! attribute.isInRange (*parsed))
                    return reject ("out of range: " + std::string (text));

                out = Value::number (*parsed);
                break;
            }

            case ValueType::boolean:
            {
                /*  Exactly "true" and "false". Not 1 and 0, not "yes": a
                    document is written by this program and read by a person, and
                    accepting four spellings means writing one and reading four
                    forever. */
                if (text == "true")       out = Value::boolean (true);
                else if (text == "false") out = Value::boolean (false);
                else return reject ("expected true or false, found \"" + std::string (text) + "\"");

                break;
            }

            case ValueType::blob:
                return reject ("blob attributes are not supported in a document");
        }

        result.ok = true;
        return result;
    }

    std::string Schema::formatValue (const Attribute& attribute, const Value& value)
    {
        switch (attribute.type())
        {
            case ValueType::string:    return value.getString();
            case ValueType::integer:
            case ValueType::integer64: return std::to_string (value.getInteger());
            case ValueType::boolean:   return value.getBoolean() ? "true" : "false";

            case ValueType::number:
                /*  The same formatter the event log uses: shortest text that
                    reads back as the identical value. A document whose numbers
                    change when it is reopened is not a document. */
                return osc::formatDouble (value.getNumber());

            case ValueType::blob:
                return {};
        }

        return {};
    }
}
