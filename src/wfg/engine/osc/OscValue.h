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
    The one value type of the control plane.

    Command arguments, event arguments, node values, the wire codec and the
    event log all carry wfg::osc::Value, so the OSC 1.1 type set is the
    vocabulary of everything Go.dot decides at control rate. It is deliberately
    NOT juce::OSCArgument: at the pinned JUCE 8.0.13 that type knows int32,
    float32, string, blob and colour only (juce_OSCArgument.h) — no booleans, no
    int64, no double, no time tag — and the OSCQuery surface, the event log and
    Phase 4's time-tagged bundles all need the rest.

    This header is vendor-free on purpose (std only). Number formatting lives in
    the .cpp, where JUCE's classic-locale formatter does the work; see
    formatDouble() for why that formatter and not std::to_chars.

    Type tags, as in the OSC 1.1 specification:

        i int32   h int64   f float32   d float64   s string   b blob
        T true    F false   N nil       I impulse   t time tag
*/

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wfg::osc
{
    struct Nil     { bool operator== (const Nil&) const noexcept { return true; } };
    struct Impulse { bool operator== (const Impulse&) const noexcept { return true; } };

    /** An NTP-style OSC time tag. The raw value 1 means "immediately". */
    struct TimeTag
    {
        std::uint64_t raw = 1;
        bool operator== (const TimeTag& other) const noexcept { return raw == other.raw; }
    };

    struct Blob
    {
        std::vector<std::uint8_t> bytes;
        bool operator== (const Blob& other) const noexcept { return bytes == other.bytes; }
    };

    class Value
    {
    public:
        enum class Type : char
        {
            int32     = 'i',
            int64     = 'h',
            float32   = 'f',
            float64   = 'd',
            string    = 's',
            blob      = 'b',
            boolTrue  = 'T',
            boolFalse = 'F',
            nil       = 'N',
            impulse   = 'I',
            timeTag   = 't'
        };

        /*  Factories rather than constructors, because a std::variant that holds
            both bool and int32_t would otherwise turn Value (1) and Value (true)
            into a guessing game at every call site. */
        static Value int32   (std::int32_t v)   { return Value (Storage { v }); }
        static Value int64   (std::int64_t v)   { return Value (Storage { v }); }
        static Value float32 (float v)          { return Value (Storage { v }); }
        static Value float64 (double v)         { return Value (Storage { v }); }
        static Value string  (std::string v)    { return Value (Storage { std::move (v) }); }
        static Value blob    (Blob v)           { return Value (Storage { std::move (v) }); }
        static Value boolean (bool v)           { return Value (Storage { v }); }
        static Value nil()                      { return Value (Storage { Nil {} }); }
        static Value impulse()                  { return Value (Storage { Impulse {} }); }
        static Value timeTag (TimeTag v)        { return Value (Storage { v }); }

        Value() : storage (Nil {}) {}

        Type type() const noexcept;
        char typeTag() const noexcept               { return static_cast<char> (type()); }

        bool isInt32() const noexcept               { return std::holds_alternative<std::int32_t> (storage); }
        bool isInt64() const noexcept               { return std::holds_alternative<std::int64_t> (storage); }
        bool isFloat32() const noexcept             { return std::holds_alternative<float> (storage); }
        bool isFloat64() const noexcept             { return std::holds_alternative<double> (storage); }
        bool isString() const noexcept              { return std::holds_alternative<std::string> (storage); }
        bool isBlob() const noexcept                { return std::holds_alternative<Blob> (storage); }
        bool isBool() const noexcept                { return std::holds_alternative<bool> (storage); }
        bool isNil() const noexcept                 { return std::holds_alternative<Nil> (storage); }
        bool isImpulse() const noexcept             { return std::holds_alternative<Impulse> (storage); }
        bool isTimeTag() const noexcept             { return std::holds_alternative<TimeTag> (storage); }

        /** True for i, h, f and d. */
        bool isNumber() const noexcept;

        /*  Accessors return the stored value or a documented default; they never
            throw, because a Value that arrived off the network is not a reason
            to unwind the tick thread. Check the type first when the difference
            between "0" and "not an int" matters. */
        std::int32_t getInt32() const noexcept;
        std::int64_t getInt64() const noexcept;
        float getFloat32() const noexcept;
        double getFloat64() const noexcept;
        const std::string& getString() const noexcept;
        const Blob& getBlob() const noexcept;
        bool getBool() const noexcept;
        TimeTag getTimeTag() const noexcept;

        /** Any numeric type as a double; 0 for everything else. */
        double asDouble() const noexcept;

        /** True when the value is a float32 or float64 that is NaN or infinite.
            Such values are rejected at every entry point (wire, log, command). */
        bool isNonFinite() const noexcept;

        /*  The event-log atom grammar (docs/godot-namespace-draft-0.1.md, §7):

                i:-12   h:1234   f:0.5   d:0.25   s:"text"   b:<base64>
                T   F   N   I   t:<uint64>

            String escapes are \\ \" \n \r \t, and \uXXXX for any other control
            character. fromAtom() returns nullopt for anything malformed —
            including non-finite floats — and never guesses. */
        std::string toAtom() const;
        static std::optional<Value> fromAtom (std::string_view atom);

        /*  Exact identity, not numeric equivalence: int32 (1) != float32 (1.0f),
            and two floats are equal only if their bit patterns are, which is what
            a byte-for-byte replay needs (and what keeps -Wfloat-equal quiet). */
        bool operator== (const Value& other) const noexcept;
        bool operator!= (const Value& other) const noexcept { return ! (*this == other); }

    private:
        using Storage = std::variant<std::int32_t, std::int64_t, float, double,
                                     std::string, Blob, bool, Nil, Impulse, TimeTag>;

        explicit Value (Storage s) : storage (std::move (s)) {}

        Storage storage;
    };

    /** The OSC type-tag string (without the leading comma) for an argument list. */
    std::string typeTagString (const std::vector<Value>& values);

    /*  Number formatting for every text surface Go.dot writes — the event log,
        the document, the OSCQuery JSON. One formatter, so a value looks the same
        wherever it appears. Locale-independent by construction, deterministic
        across the three platforms, "as many decimal places as necessary". */
    std::string formatDouble (double value);

    /** Locale-independent parse. nullopt for anything that is not a finite number. */
    std::optional<double> parseDouble (std::string_view text);
}
