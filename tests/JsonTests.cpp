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
    The JSON reader.

    It exists because JUCE's overflows an int64 on a long integer literal and
    returns a different number; MountTests measures that and this file tests
    what replaced it. A namespace file arrives from outside the program, so the
    cases that matter most here are the malformed ones: what it refuses, and
    whether it says where.

    A serialisation surface, so every case runs under fr_FR as well as C - a
    parser that consulted the locale would read "1.5" as 1 wherever a comma is
    the decimal separator, which is the whole reason the number path goes
    through osc::parseDouble.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/osc/OscValue.h>

#include <cstring>
#include <limits>
#include <string>

using namespace wfg;

namespace
{
    json::Value parsed (const std::string& text)
    {
        const auto result = json::parse (text);

        REQUIRE_MESSAGE (result.ok(), "line " << result.line << ": " << result.error);
        return *result.value;
    }

    void refuses (const std::string& text)
    {
        const auto result = json::parse (text);

        INFO ("input: " << text);
        CHECK_FALSE (result.ok());
        CHECK_FALSE (result.error.empty());
    }
}

//==============================================================================
TEST_CASE ("json: the six kinds of value")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto document = parsed (R"({
        "nothing": null,
        "yes": true,
        "no": false,
        "number": -12.5,
        "text": "hello",
        "list": [1, 2, 3],
        "nested": {"a": 1}
    })");

    REQUIRE (document.isObject());
    CHECK (document.size() == 7);

    REQUIRE (document.find ("nothing") != nullptr);
    CHECK (document.find ("nothing")->isNull());

    CHECK (document.find ("yes")->asBool());
    CHECK_FALSE (document.find ("no")->asBool());
    CHECK (document.find ("number")->asNumber() == doctest::Approx (-12.5));
    CHECK (document.find ("text")->asString() == "hello");

    REQUIRE (document.find ("list")->isArray());
    CHECK (document.find ("list")->size() == 3);
    CHECK (document.find ("list")->at (1)->asNumber() == doctest::Approx (2.0));
    CHECK (document.find ("list")->at (3) == nullptr);

    REQUIRE (document.find ("nested")->isObject());
    CHECK (document.find ("nested")->find ("a")->asNumber() == doctest::Approx (1.0));

    // A key that is not there is not there, and asking costs nothing.
    CHECK (document.find ("missing") == nullptr);
}

TEST_CASE ("json: an accessor of the wrong kind gives nothing, never an interpretation")
{
    /*  A file that puts a word where a bound belongs has a problem, and reading
        "abc" as zero would hide it behind a range nobody wrote. */
    const auto document = parsed (R"({"text": "abc", "number": 7})");

    CHECK (document.find ("text")->asNumber() == doctest::Approx (0.0));
    CHECK (document.find ("text")->asInt() == 0);
    CHECK_FALSE (document.find ("text")->asBool());

    CHECK (document.find ("number")->asString().empty());
    CHECK (document.find ("number")->asArray().empty());
    CHECK (document.find ("number")->asObject().empty());
    CHECK (document.find ("number")->at (0) == nullptr);
}

//==============================================================================
TEST_CASE ("json: numbers are exact, and do not go through the locale")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const auto document = parsed (R"({
        "plain": 1.5,
        "negative": -0.25,
        "exponent": 1.5e3,
        "negativeExponent": 2.5E-4,
        "zero": 0,
        "negativeZero": -0.0,
        "big": 40595640456200454144,
        "tiny": 5e-324
    })");

    CHECK (document.find ("plain")->asNumber() == doctest::Approx (1.5));
    CHECK (document.find ("negative")->asNumber() == doctest::Approx (-0.25));
    CHECK (document.find ("exponent")->asNumber() == doctest::Approx (1500.0));
    CHECK (document.find ("negativeExponent")->asNumber() == doctest::Approx (0.00025));

    /*  The literal JUCE gets wrong: twenty digits, no point and no exponent, so
        its int64 accumulator wraps. Ours hands the token to strtod. */
    const auto big = document.find ("big")->asNumber();
    CHECK (osc::formatDouble (big) == "40595640456200454144");

    /*  The smallest subnormal, which is also where the OSC reader once broke.
        Compared as bits rather than with ==, which the strict preset's
        -Wfloat-equal refuses and which would in any case be the wrong question:
        two doubles that print the same are not necessarily the same double. */
    const auto tiny = document.find ("tiny")->asNumber();
    const auto smallest = std::numeric_limits<double>::denorm_min();

    CHECK (tiny > 0.0);
    CHECK (std::memcmp (&tiny, &smallest, sizeof (double)) == 0);

    // Negative zero keeps its sign, which a memcmp can tell from positive zero.
    const auto negativeZero = document.find ("negativeZero")->asNumber();
    const auto positiveZero = 0.0;
    CHECK (std::memcmp (&negativeZero, &positiveZero, sizeof (double)) != 0);
}

TEST_CASE ("json: a malformed number is refused rather than half-read")
{
    for (const auto& text : { R"({"v": 01})", R"({"v": .5})", R"({"v": 1.})",
                              R"({"v": 1e})", R"({"v": 1e+})", R"({"v": +1})",
                              R"({"v": -})", R"({"v": NaN})", R"({"v": Infinity})" })
        refuses (text);
}

//==============================================================================
TEST_CASE ("json: strings carry every escape, surrogate pairs included")
{
    const auto document = parsed (R"({
        "escapes": "a\"b\\c\/d\be\ff\ng\rh\ti",
        "unicode": "é€",
        "surrogate": "🎛"
    })");

    CHECK (document.find ("escapes")->asString() == "a\"b\\c/d\be\ff\ng\rh\ti");

    // e-acute and a euro sign, as UTF-8.
    CHECK (document.find ("unicode")->asString() == "\xc3\xa9\xe2\x82\xac");

    /*  One character, not two. A reader that emitted the halves separately
        would produce text no editor agrees with - and a control surface is
        exactly the kind of thing whose node names carry symbols. */
    CHECK (document.find ("surrogate")->asString() == "\xf0\x9f\x8e\x9b");
}

TEST_CASE ("json: a broken string is refused")
{
    refuses (R"({"v": "unterminated})");
    refuses (R"({"v": "\q"})");              // not an escape
    refuses (R"({"v": "\u12"})");            // too few hex digits
    refuses (R"({"v": "\ud83c"})");          // a high surrogate with no low one
    refuses (R"({"v": "\ud83cx"})");         // ... nor is that a low one
    refuses ("{\"v\": \"a\tb\"}");           // a raw control character
}

//==============================================================================
TEST_CASE ("json: the same member name twice is refused, not resolved")
{
    /*  JSON does not say which of two wins, so a file carrying both is one
        whose author disagrees with themselves. Picking one would be inventing
        an answer, and the one not picked might be the range that mattered. */
    refuses (R"({"a": 1, "a": 2})");

    // Different names that merely look alike are fine.
    const auto document = parsed (R"({"a": 1, "A": 2})");
    CHECK (document.size() == 2);
}

TEST_CASE ("json: what is not JSON is refused, however common it is")
{
    refuses ("");
    refuses ("   ");
    refuses ("{");
    refuses ("}");
    refuses ("{\"a\": 1,}");                 // trailing comma
    refuses ("[1, 2,]");
    refuses ("{'a': 1}");                    // single quotes
    refuses ("{a: 1}");                      // unquoted key
    refuses ("{\"a\" 1}");                   // missing colon
    refuses ("// a comment\n{}");
    refuses ("{} trailing");
    refuses ("{}{}");
}

TEST_CASE ("json: an empty object and an empty array are both fine")
{
    CHECK (parsed ("{}").isObject());
    CHECK (parsed ("{}").size() == 0);
    CHECK (parsed ("[]").isArray());
    CHECK (parsed ("[]").size() == 0);
    CHECK (parsed ("  \n  {\"a\":[]}  \n ").isObject());
}

TEST_CASE ("json: a document nested past the limit is refused rather than crashing")
{
    /*  A namespace file arrives from outside the program. This reader walks the
        tree on the C++ stack, so a file of a hundred thousand open brackets
        would overflow it - which is a crash rather than a message, and is
        exactly what an outside input should not be able to cause. */
    std::string deep;

    for (int i = 0; i < 200; ++i)
        deep += "[";

    for (int i = 0; i < 200; ++i)
        deep += "]";

    refuses (deep);

    // Well within the limit, and fine.
    std::string shallow;

    for (int i = 0; i < 30; ++i)
        shallow += "[";

    shallow += "1";

    for (int i = 0; i < 30; ++i)
        shallow += "]";

    CHECK (json::parse (shallow).ok());
}

TEST_CASE ("json: a problem says which line it is on")
{
    const auto result = json::parse ("{\n  \"a\": 1,\n  \"b\": oops\n}");

    CHECK_FALSE (result.ok());
    CHECK (result.line == 3);
    INFO ("error: " << result.error);
    CHECK_FALSE (result.error.empty());
}

//==============================================================================
TEST_CASE ("json: a value can be copied and moved without losing its children")
{
    /*  Value holds its array and object by pointer, because a JSON document
        nests and a member of itself by value cannot. That makes the copy
        constructor something written by hand, and something written by hand is
        something to test. */
    const auto original = parsed (R"({"list": [1, 2], "nested": {"a": "x"}})");

    auto copy = original;

    CHECK (copy.find ("list")->size() == 2);
    CHECK (copy.find ("nested")->find ("a")->asString() == "x");

    // The copy is its own: the two do not share children.
    CHECK (original.find ("nested")->find ("a")->asString() == "x");

    const auto moved = std::move (copy);
    CHECK (moved.find ("list")->at (1)->asNumber() == doctest::Approx (2.0));

    json::Value assigned;
    assigned = original;
    CHECK (assigned.find ("nested")->find ("a")->asString() == "x");

    // Self-assignment does not empty it.
    assigned = *&assigned;
    CHECK (assigned.find ("nested") != nullptr);
}
