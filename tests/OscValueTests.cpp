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

/*
    The value type of the whole control plane, and the number formatter every
    text surface writes through.

    These cases run twice, under C and under fr_FR, and the second run is the
    one that means something: a comma where a full stop belongs turns a show
    file into one that reopens wrong on another machine, and an event log into
    one that cannot be replayed. Under C most of what follows is a tautology;
    keeping it is deliberate, because a failure there would mean something far
    stranger than a locale bug.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace wfg::osc;

//==============================================================================
TEST_CASE ("osc value: the number formatter is locale-independent")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    /*  Shortest round-trip, so an integral double is "1" rather than "1.0" and
        a half is "0.5" rather than "0.500000". The type is never carried by the
        text - the log tags its atoms and the document schema declares its
        attributes - so the shortest form loses nothing. */
    CHECK (formatDouble (0.5) == "0.5");
    CHECK (formatDouble (-0.25) == "-0.25");
    CHECK (formatDouble (0.0) == "0");
    CHECK (formatDouble (1.0) == "1");
    CHECK (formatDouble (-1.5) == "-1.5");

    // A float is written from the float, not from its double promotion: 0.1f
    // promoted and written exactly is 0.10000000149011612, which is true and
    // useless. Both forms read back as the same float.
    CHECK (formatFloat (0.1f) == "0.1");
    CHECK (formatFloat (0.5f) == "0.5");

    // The round trip matters as much as the formatting: a reader that honoured
    // the locale would parse "0.5" as 0 on a French machine.
    const auto parsed = parseDouble ("0.5");
    REQUIRE (parsed.has_value());
    CHECK (*parsed == doctest::Approx (0.5));
}

TEST_CASE ("osc value: a double survives the round trip exactly, whatever its bits")
{
    /*  The measurement that chose the formatter, kept as a test. JUCE's own
        writer fails this at a rate of 46%: it stops at fifteen significant
        digits, so nearly half of all doubles read back as a different number.
        A show document and an event log both depend on this being zero. */
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    std::mt19937_64 rng { 20260904u };
    std::uniform_int_distribution<std::uint64_t> bits;

    int tested = 0;

    for (int i = 0; i < 20000; ++i)
    {
        const auto pattern = bits (rng);
        double d;
        std::memcpy (&d, &pattern, sizeof (d));

        if (! std::isfinite (d))
            continue;

        ++tested;

        const auto text = formatDouble (d);
        const auto back = parseDouble (text);

        REQUIRE (back.has_value());

        if (std::memcmp (&d, &*back, sizeof (d)) != 0)
        {
            INFO ("bit pattern " << pattern << " wrote " << text);
            FAIL ("a double did not survive the round trip");
        }
    }

    CHECK (tested > 15000);
}

TEST_CASE ("osc value: the parser refuses what is not a number")
{
    /*  Leniency here is how a typo becomes a cue. juce::String::getDoubleValue
        parses "12abc" as 12 and "" as 0; the log and the document must not. */
    CHECK_FALSE (parseDouble ("12abc").has_value());
    CHECK_FALSE (parseDouble ("").has_value());
    CHECK_FALSE (parseDouble ("abc").has_value());
    CHECK_FALSE (parseDouble ("1.2.3").has_value());
    CHECK_FALSE (parseDouble ("1e").has_value());
    CHECK_FALSE (parseDouble ("1e+").has_value());
    CHECK_FALSE (parseDouble (" 1").has_value());
    CHECK_FALSE (parseDouble ("1 ").has_value());
    CHECK_FALSE (parseDouble ("nan").has_value());
    CHECK_FALSE (parseDouble ("inf").has_value());

    CHECK (parseDouble ("-1.5e3").has_value());
    CHECK (parseDouble ("42").has_value());
}

//==============================================================================
TEST_CASE ("osc value: every type round-trips through its log atom")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    Blob blob;
    blob.bytes = { 0x00, 0x01, 0xfe, 0xff, 0x2f };

    const std::vector<Value> values {
        Value::int32 (-12),
        Value::int32 (0),
        Value::int64 (9223372036854775807LL),
        Value::float32 (0.5f),
        Value::float32 (-0.0f),
        Value::float64 (0.25),
        Value::string ("plain"),
        Value::string (""),
        Value::blob (blob),
        Value::boolean (true),
        Value::boolean (false),
        Value::nil(),
        Value::impulse(),
        Value::timeTag (TimeTag { 1 }),
        Value::timeTag (TimeTag { 16777216000000000ULL })
    };

    for (const auto& v : values)
    {
        const auto atom = v.toAtom();
        INFO ("atom: " << atom);

        const auto back = Value::fromAtom (atom);
        REQUIRE (back.has_value());
        CHECK (*back == v);
        CHECK (back->toAtom() == atom);
    }
}

TEST_CASE ("osc value: a string atom survives spaces, quotes and control characters")
{
    /*  The log tokeniser splits on spaces, so a cue name with a space in it is
        exactly the case that breaks a naive format. Quotes and newlines are the
        next two. */
    const std::vector<std::string> awkward {
        "House to half",
        "she said \"go\"",
        "back\\slash",
        "line\nbreak",
        "tab\there",
        "carriage\rreturn",
        "Repetition generale a 20h",
        "\x01\x02"
    };

    for (const auto& s : awkward)
    {
        const auto v = Value::string (s);
        const auto atom = v.toAtom();

        INFO ("atom: " << atom);

        // Every string atom is quoted, so a space inside one cannot be mistaken
        // for the separator between two atoms.
        REQUIRE (atom.size() > 2);
        CHECK (atom.substr (0, 3) == "s:\"");
        CHECK (atom.back() == '"');

        const auto back = Value::fromAtom (atom);
        REQUIRE (back.has_value());
        CHECK (back->getString() == s);
    }
}

TEST_CASE ("osc value: a malformed atom is refused rather than guessed at")
{
    const std::vector<std::string> bad {
        "",
        "i",
        "i:",
        "i:abc",
        "i:1.5",
        "i:99999999999999999999",
        "h:xyz",
        "f:",
        "f:nan",
        "f:inf",
        "d:1.2.3",
        "s:unquoted",
        "s:\"unterminated",
        "s:\"bad\\escape\"",
        "b:not base64!",
        "q:1",
        "TT",
        "t:-1"
    };

    for (const auto& atom : bad)
    {
        INFO ("atom: " << atom);
        CHECK_FALSE (Value::fromAtom (atom).has_value());
    }
}

//==============================================================================
TEST_CASE ("osc value: float32 survives the log exactly, for any bit pattern")
{
    /*  The log writes floats through the same formatter as everything else, at
        double precision, and reads them back as double before narrowing. That
        is only safe if every float32 comes back bit-identical - so sweep a few
        thousand random bit patterns rather than trusting the argument.

        Non-finite patterns are skipped: they are refused at every entry point
        by design, and the case below pins that separately. */
    std::mt19937 rng { 20260904u };
    std::uniform_int_distribution<std::uint32_t> bits;

    int tested = 0;

    for (int i = 0; i < 5000; ++i)
    {
        const auto pattern = bits (rng);
        float f;
        std::memcpy (&f, &pattern, sizeof (f));

        if (! std::isfinite (f))
            continue;

        ++tested;

        const auto atom = Value::float32 (f).toAtom();
        const auto back = Value::fromAtom (atom);

        REQUIRE (back.has_value());
        REQUIRE (back->isFloat32());

        const auto recovered = back->getFloat32();

        if (std::memcmp (&f, &recovered, sizeof (f)) != 0)
        {
            INFO ("bit pattern " << pattern << " wrote atom " << atom);
            FAIL ("a float32 did not survive the log");
        }
    }

    CHECK (tested > 4000);
}

TEST_CASE ("osc value: non-finite floats are refused, not written")
{
    const auto nan = Value::float32 (std::numeric_limits<float>::quiet_NaN());
    const auto inf = Value::float64 (std::numeric_limits<double>::infinity());

    CHECK (nan.isNonFinite());
    CHECK (inf.isNonFinite());
    CHECK_FALSE (Value::float32 (0.0f).isNonFinite());
    CHECK_FALSE (Value::int32 (0).isNonFinite());

    CHECK_FALSE (Value::fromAtom (nan.toAtom()).has_value());
    CHECK_FALSE (Value::fromAtom (inf.toAtom()).has_value());
}

//==============================================================================
TEST_CASE ("osc value: equality is exact identity, not numeric equivalence")
{
    /*  A replay compares records, and a record that "matches" because 1 equals
        1.0 would let a type change through unnoticed. */
    CHECK (Value::int32 (1) != Value::float32 (1.0f));
    CHECK (Value::int32 (1) != Value::int64 (1));
    CHECK (Value::float32 (1.0f) != Value::float64 (1.0));
    CHECK (Value::boolean (true) != Value::int32 (1));

    CHECK (Value::int32 (1) == Value::int32 (1));
    CHECK (Value::string ("a") == Value::string ("a"));
    CHECK (Value::string ("a") != Value::string ("b"));

    // -0.0 and 0.0 compare equal as numbers and differ as bit patterns; the
    // second is what a byte-for-byte replay needs.
    CHECK (Value::float32 (-0.0f) != Value::float32 (0.0f));
}

TEST_CASE ("osc value: the type tag string is the OSC one")
{
    const std::vector<Value> args {
        Value::int32 (1), Value::float32 (2.0f), Value::string ("three"),
        Value::boolean (true), Value::boolean (false), Value::nil()
    };

    CHECK (typeTagString (args) == "ifsTFN");
    CHECK (typeTagString ({}) == "");
}

