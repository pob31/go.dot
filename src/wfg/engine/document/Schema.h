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

#pragma once

/*
    What a show document may contain, and what each value means.

    Two halves, and the split is deliberate:

      * the ATTRIBUTES come from docs/parameters/godot-parameters.csv, through
        the generated table. Adding a parameter is a row in that file, and it
        reaches the document, the parameter tree, the RELAX NG schema and the
        OSCQuery reply from that one edit.
      * the CONTAINMENT - which element may hold which - is written here by
        hand, because the table has one row per attribute and containment is a
        property of elements. It is a handful of lines; see elementTable().

    Two rules this class applies that the generated table deliberately does not:

      * a Group is a Cue. PRD §3.6 makes the cue list "virtually a sequence
        group", so a Group carries every Cue attribute plus its own. The table
        says `cue` and `group`; joining them is a decision, so it lives here.
      * a row that persists nowhere is not a document attribute. `persist=none`
        marks a runtime projection - a cue's index among its siblings, the
        engine's tick - which the parameter tree publishes and no file ever
        holds. Those rows are still in the table, because Phase 1.5 needs them.

    Vendor-free, and staying that way: this is consulted from the canonical
    writer, the reader, the tree and the schema generator, and it should not
    drag JUCE into any of them.
*/

#include <wfg/engine/document/SchemaTypes.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    /*  An attribute as the document layer sees it: the generated row, plus the
        element it landed on once the Group-is-a-Cue rule has been applied. */
    struct Attribute
    {
        std::string_view element;
        const AttributeRow* row = nullptr;

        std::string_view name() const noexcept        { return row->name; }
        ValueType type() const noexcept               { return row->type; }
        char oscTypeTag() const noexcept              { return row->oscTypeTag; }
        Access access() const noexcept                { return row->access; }
        Persist persist() const noexcept              { return row->persist; }
        bool hasDefault() const noexcept              { return row->hasDefault; }
        std::string_view defaultText() const noexcept { return row->defaultText; }
        std::string_view description() const noexcept { return row->description; }

        bool isEnum() const noexcept                  { return row->numEnumValues > 0; }
        bool isEnumValue (std::string_view v) const noexcept;

        /** True when the value is within the row's declared range, or when
            there is no range to be outside of. */
        bool isInRange (double value) const noexcept;
    };

    /*  A document element: its name, what it may contain, and whether it is
        identified. */
    struct Element
    {
        std::string_view name;
        std::vector<Attribute> attributes;
        std::vector<std::string_view> childElements;

        /** Objects carry an `id`; containers and the root do not. */
        bool hasIdentity = false;

        const Attribute* attribute (std::string_view attributeName) const;
        bool mayContain (std::string_view childName) const;
    };

    class Schema
    {
    public:
        /** The one instance. Built once, on first use, from the generated
            table; there is nothing to configure and nothing to vary. */
        static const Schema& instance();

        const Element* element (std::string_view name) const;
        const std::vector<Element>& elements() const noexcept { return elementList; }

        /** The attribute, or nullptr if this element has no such attribute. */
        const Attribute* attribute (std::string_view element,
                                    std::string_view attributeName) const;

        /** The root element every show document starts with. */
        static constexpr std::string_view rootElement = "Show";

        //======================================================================
        /*  Value handling, in one place so the reader, the writer and every
            command agree about what a value IS.

            A document value is held as text in the file and as a typed value in
            memory, and the conversion between them is the single most
            error-prone thing in this layer: JUCE's ValueTree::fromXml types
            every attribute as a string, and a comparison that says "1" == 1
            will then drop a typed write without a word. Everything goes through
            here instead. */
        struct Parsed
        {
            bool ok = false;
            std::string error;          ///< empty when ok
        };

        /** Parses `text` against the attribute's declared type and range.
            `out` is only written when the result is ok. */
        static Parsed parseValue (const Attribute& attribute, std::string_view text,
                                  class Value& out);

        /** The canonical text for a typed value: what the writer puts in the
            file, and what the log and the tree would show. */
        static std::string formatValue (const Attribute& attribute, const class Value& value);

    private:
        Schema();

        std::vector<Element> elementList;
    };

    /*  A typed document value. Deliberately not osc::Value: the document holds
        decided values, so it needs neither impulses nor time tags, and a
        document that could hold a nil would have somewhere to put "I do not
        know", which PRD §4.10 says it must not.

        It converts to osc::Value at the tree boundary, in Phase 1.5. */
    class Value
    {
    public:
        Value() = default;

        static Value string (std::string v);
        static Value integer (long long v);
        static Value number (double v);
        static Value boolean (bool v);

        ValueType type() const noexcept { return valueType; }

        bool isString() const noexcept  { return valueType == ValueType::string; }
        bool isInteger() const noexcept { return valueType == ValueType::integer
                                              || valueType == ValueType::integer64; }
        bool isNumber() const noexcept  { return valueType == ValueType::number; }
        bool isBoolean() const noexcept { return valueType == ValueType::boolean; }

        const std::string& getString() const noexcept { return text; }
        long long getInteger() const noexcept         { return integerValue; }
        double getNumber() const noexcept             { return numberValue; }
        bool getBoolean() const noexcept              { return booleanValue; }

        /** Exact identity, type included: integer 1 is not number 1.0, for the
            same reason osc::Value works that way — a replay compares values. */
        bool operator== (const Value& other) const noexcept;
        bool operator!= (const Value& other) const noexcept { return ! (*this == other); }

    private:
        ValueType valueType = ValueType::string;
        std::string text;
        long long integerValue = 0;
        double numberValue = 0.0;
        bool booleanValue = false;
    };
}
