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
    A media cue's level lane, on its own (namespace draft §20.3): what makes a
    list a lane, and what a lane asks for at a second of the file.

    The door, the loader and undo are asked beside their fade twins
    (`DocumentTests`, `UndoTests`); the Runner's term beside the other terms of
    the level sum (`GoTests`, `RangeTests`). What is here needs no document.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/document/LevelLane.h>

#include <string>
#include <vector>

using namespace wfg;

TEST_CASE ("level lane: a list of pairs is read as a lane, and a list that is not one says why")
{
    /*  Nothing at all is no lane, and not a refusal: every media cue written
        before there were lanes goes on meaning what it meant. */
    CHECK (doc::readLevelLane ("").points.empty());
    CHECK (doc::readLevelLane ("").problem.empty());
    CHECK (doc::readLevelLane ("   ").problem.empty());

    /*  Seconds of the file and offsets in dB, spaced however a hand spaced them. */
    const auto read = doc::readLevelLane ("  4 0\t5.5 -12   6 0 ");
    REQUIRE (read.problem.empty());
    REQUIRE (read.points.size() == 3u);
    CHECK (read.points[1].seconds == doctest::Approx (5.5));
    CHECK (read.points[1].levelDb == doctest::Approx (-12.0));

    /*  A REFUSAL NAMES WHERE, in words: the reader of a refused file is
        somebody with an editor open, looking for the element at fault. */
    struct Case { const char* text; const char* mentions; };

    const Case cases[] = {
        { "4 0 5.5 x",           "element 3" },
        { "4 0 5.5",             "odd number" },
        { "-0.25 0",             "before the file starts" },
        { "4 0 3.5 -6",          "point 1: second 3.5 does not come after 4" },
        { "4 0 4 -6",            "does not come after" },
        { "4 0 5 12.5",          "point 1: level 12.5 dB is outside -120..12" },
        { "4 -121",              "outside -120..12" },
    };

    for (const auto& c : cases)
    {
        const auto judged = doc::readLevelLane (c.text);

        INFO ("\"" << c.text << "\" -> " << judged.problem);
        CHECK (judged.points.empty());
        CHECK (judged.problem.find (c.mentions) != std::string::npos);
    }

    /*  The range's own ends are levels a cue may be written at, and so a lane's. */
    CHECK (doc::readLevelLane ("0 -120 1 12").problem.empty());
}

TEST_CASE ("level lane: straight in dB between points, held beyond both ends, nought when there is none")
{
    using doc::laneLevelDb;
    using doc::LanePoint;

    /*  NO LANE IS THE CUE AS WRITTEN, wherever the file is. */
    CHECK (laneLevelDb ({}, 0.0) == doctest::Approx (0.0));
    CHECK (laneLevelDb ({}, 1000.0) == doctest::Approx (0.0));

    /*  ONE POINT IS A CONSTANT OFFSET, before it and after it alike. */
    const std::vector<LanePoint> one { { 10.0, -6.0 } };
    CHECK (laneLevelDb (one, 0.0) == doctest::Approx (-6.0));
    CHECK (laneLevelDb (one, 10.0) == doctest::Approx (-6.0));
    CHECK (laneLevelDb (one, 99.0) == doctest::Approx (-6.0));

    /*  A dip: level to -20 over a second, held, and back. */
    const std::vector<LanePoint> dip { { 4.0, 0.0 }, { 5.0, -20.0 }, { 8.0, -20.0 }, { 9.0, 0.0 } };

    /*  HELD AT BOTH ENDS: before the first point it is the first's level, and
        after the last the last's - a lane drawn over the middle of a file
        does not decide anything about the rest of it. */
    CHECK (laneLevelDb (dip, 0.0) == doctest::Approx (0.0));
    CHECK (laneLevelDb (dip, 3.99) == doctest::Approx (0.0));
    CHECK (laneLevelDb (dip, 12.0) == doctest::Approx (0.0));

    /*  Exactly the drawn level at each point. */
    CHECK (laneLevelDb (dip, 4.0) == doctest::Approx (0.0));
    CHECK (laneLevelDb (dip, 5.0) == doctest::Approx (-20.0));
    CHECK (laneLevelDb (dip, 8.0) == doctest::Approx (-20.0));
    CHECK (laneLevelDb (dip, 9.0) == doctest::Approx (0.0));

    /*  Straight in dB between them, which is what a drawn fade does. */
    CHECK (laneLevelDb (dip, 4.25) == doctest::Approx (-5.0));
    CHECK (laneLevelDb (dip, 4.5) == doctest::Approx (-10.0));
    CHECK (laneLevelDb (dip, 6.5) == doctest::Approx (-20.0));
    CHECK (laneLevelDb (dip, 8.75) == doctest::Approx (-5.0));

    /*  And what the judge reads is what is played. */
    const auto judged = doc::readLevelLane ("4 0 5 -20 8 -20 9 0");
    REQUIRE (judged.problem.empty());
    CHECK (laneLevelDb (judged.points, 4.5) == doctest::Approx (-10.0));
}

//==============================================================================
/*  AN OSC CUE'S CURVE (namespace draft 45): the level lane's pairs and rules,
    the seconds the cue's and the values the number itself, held to the curve's
    own range when it has one. */
TEST_CASE ("curve: read as a lane with its own range, or none, and found by halving")
{
    std::string problem;

    //  A range is two numbers, the lowest first; empty is none.
    CHECK_FALSE (doc::readLaneRange ("", problem).has_value());
    CHECK (problem.empty());

    const auto metres = doc::readLaneRange ("-10 10", problem);
    REQUIRE (metres.has_value());
    CHECK (metres->low == doctest::Approx (-10.0));
    CHECK (metres->high == doctest::Approx (10.0));

    CHECK_FALSE (doc::readLaneRange ("10 -10", problem).has_value());
    CHECK_FALSE (problem.empty());
    problem.clear();
    CHECK_FALSE (doc::readLaneRange ("1 2 3", problem).has_value());
    CHECK_FALSE (problem.empty());

    //  No range: any value at all - metres, degrees, a scene number.
    CHECK (doc::readLane ("0 -400 2.5 1200", std::nullopt).problem.empty());

    //  With one, a value outside it is refused, and says so.
    const auto outside = doc::readLane ("0 0 1 12", doc::LaneRange { -10.0, 10.0 });
    CHECK_FALSE (outside.problem.empty());
    CHECK (outside.points.empty());

    //  The level lane's own rules still hold: pairs, climbing from nought.
    CHECK_FALSE (doc::readLane ("0 1 2", std::nullopt).problem.empty());
    CHECK_FALSE (doc::readLane ("1 0 1 1", std::nullopt).problem.empty());
    CHECK_FALSE (doc::readLane ("-1 0", std::nullopt).problem.empty());

    //  Straight between points, held beyond them - over a thousand points.
    std::vector<doc::LanePoint> points;

    for (int n = 0; n <= 1000; ++n)
        points.push_back ({ n * 0.01, n * 0.5 });

    CHECK (doc::laneValueAt (points, -1.0) == doctest::Approx (0.0));
    CHECK (doc::laneValueAt (points, 5.005) == doctest::Approx (250.25));
    CHECK (doc::laneValueAt (points, 99.0) == doctest::Approx (500.0));
    CHECK (doc::laneValueAt ({}, 3.0) == doctest::Approx (0.0));
}
