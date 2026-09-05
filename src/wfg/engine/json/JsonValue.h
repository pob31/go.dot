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
    JSON, read.

    OURS RATHER THAN juce::JSON, and this one was measured rather than assumed.

    JUCE's parser accumulates a plain integer literal into an int64 - one digit
    at a time, `intValue = intValue * 10 + digit` - and only abandons that for
    the correctly-rounded floating path when it meets a `.` or an `e`
    (juce_JSON.cpp, parseNumber). A number with neither, and more digits than an
    int64 holds, therefore OVERFLOWS SILENTLY and comes back as something else
    entirely. Measured over 19 993 random doubles written in their shortest
    round-trip form: 142 came back wrong, one in 140. The first was
    -40595640456200454144, which returned as -3702152308781350912.

    That matters here because a namespace file's numbers are somebody else's
    range bounds and rate caps, and a bound that changes on the way in is a
    bound Go.dot will enforce against a target that never declared it. It
    matters more in Phase 2, where an OSCQuery client parses replies from real
    devices continuously.

    So the numbers go through osc::parseDouble - strtod in an explicit C locale,
    correctly rounded, the same conversion the document and the log use. One
    number format across the whole program, in both directions, is the point:
    the writer in tree/OscQueryJson.cpp is ours for the mirror-image reason.

    WHAT IT SUPPORTS is RFC 8259: objects, arrays, strings with the full escape
    set including surrogate pairs, numbers, `true`, `false`, `null`. What it
    does NOT is comments, trailing commas, single quotes, unquoted keys and NaN
    or Infinity literals - none of which are JSON, and every one of which is a
    file somebody should be told about rather than have guessed at.

    Vendor-free.
*/

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::json
{
    class Value;

    /*  Sorted by key, which is deterministic and is all any reader here needs -
        nothing reconstructs a document from this, and the tree sorts by address
        anyway. A repeated key is a parse error rather than a silent overwrite. */
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;

    class Value
    {
    public:
        enum class Type { null, boolean, number, string, array, object };

        Value();                                    ///< null
        explicit Value (bool v);
        explicit Value (double v);
        explicit Value (std::string v);
        explicit Value (Array v);
        explicit Value (Object v);

        Value (const Value&);
        Value (Value&&) noexcept;
        Value& operator= (const Value&);
        Value& operator= (Value&&) noexcept;
        ~Value();

        Type type() const noexcept { return kind; }

        bool isNull() const noexcept   { return kind == Type::null; }
        bool isBool() const noexcept   { return kind == Type::boolean; }
        bool isNumber() const noexcept { return kind == Type::number; }
        bool isString() const noexcept { return kind == Type::string; }
        bool isArray() const noexcept  { return kind == Type::array; }
        bool isObject() const noexcept { return kind == Type::object; }

        //======================================================================
        /*  Accessors that never throw and never guess. Asking a string for its
            number gives 0, not an interpretation: a file that puts a word where
            a bound belongs has a problem, and quietly reading "abc" as zero
            would hide it behind a range nobody wrote. */
        bool asBool() const noexcept;
        double asNumber() const noexcept;
        int asInt() const noexcept;
        const std::string& asString() const noexcept;
        const Array& asArray() const noexcept;
        const Object& asObject() const noexcept;

        /** The member of an object, or nullptr. Null for anything else. */
        const Value* find (std::string_view key) const noexcept;

        /** Element `index` of an array, or nullptr. */
        const Value* at (std::size_t index) const noexcept;

        std::size_t size() const noexcept;

    private:
        Type kind = Type::null;
        bool booleanValue = false;
        double numberValue = 0.0;
        std::string stringValue;

        /*  By pointer so that Value is a complete type before Array and Object
            need it. A JSON document nests, and a member of itself by value
            cannot. */
        std::unique_ptr<Array> arrayValue;
        std::unique_ptr<Object> objectValue;
    };

    //==============================================================================
    struct ParseResult
    {
        std::optional<Value> value;

        /** Empty when it parsed. Otherwise says what and where, in that order. */
        std::string error;

        /** 1-based, so an editor can be pointed at it. */
        std::size_t line = 0;

        bool ok() const noexcept { return value.has_value(); }
    };

    /** Parses one JSON document. Trailing content after it is an error. */
    ParseResult parse (std::string_view text);
}
