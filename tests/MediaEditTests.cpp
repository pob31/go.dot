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

/*  A SOUND'S EDIT AS ARITHMETIC (namespace draft §55): the timeline its
    sections make, the map from one timeline to the next, and what is carried
    through it - the lane points, the ranges, the start offset - so that a dip
    drawn over the verse is over the verse wherever the verse goes.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/document/MediaEdit.h>
#include <wfg/engine/osc/OscValue.h>

#include <cmath>
#include <string>
#include <vector>

using namespace wfg;
using doc::Section;
using doc::TimeRun;

namespace
{
    Section piece (const char* id, double in, double out, double trim = 0.0, double crossfade = 0.01)
    {
        return { id, in, out, trim, crossfade };
    }

    /*  A thirty-second file split at ten and twenty: the intro, the verse and
        the chorus. */
    std::vector<Section> introVerseChorus()
    {
        return { piece ("A", 0.0, 10.0), piece ("B", 10.0, 20.0), piece ("C", 20.0, 30.0) };
    }

    bool near (double a, double b)
    {
        return std::abs (a - b) < 1.0e-9;
    }

    bool sameRuns (const std::vector<TimeRun>& runs, const std::vector<TimeRun>& wanted)
    {
        if (runs.size() != wanted.size())
            return false;

        for (std::size_t i = 0; i < runs.size(); ++i)
            if (! near (runs[i].oldFrom, wanted[i].oldFrom) || ! near (runs[i].newFrom, wanted[i].newFrom)
                  || ! near (runs[i].length, wanted[i].length))
                return false;

        return true;
    }
}

//==============================================================================
TEST_CASE ("media edit: the timeline is the sections put together, in their order")
{
    const auto sections = introVerseChorus();
    const auto starts = doc::sectionStarts (sections);

    REQUIRE (starts.size() == 3);
    CHECK (near (starts[0], 0.0));
    CHECK (near (starts[1], 10.0));
    CHECK (near (starts[2], 20.0));
    CHECK (near (doc::editedLength (sections), 30.0));

    /*  The chorus first: the timeline is the order, not the file. */
    const std::vector<Section> chorusFirst { sections[2], sections[0], sections[1] };
    CHECK (near (doc::sectionStarts (chorusFirst)[1], 10.0));
    CHECK (near (doc::editedLength (chorusFirst), 30.0));

    /*  Material may be used twice, and the sum says so. */
    const std::vector<Section> twice { piece ("A", 5.0, 8.0), piece ("B", 0.0, 30.0) };
    CHECK (near (doc::sectionStarts (twice)[1], 3.0));
    CHECK (near (doc::editedLength (twice), 33.0));

    CHECK (doc::isContinuousJoin (sections[0], sections[1]));
    CHECK (doc::isContinuousJoin (sections[1], sections[2]));
    CHECK_FALSE (doc::isContinuousJoin (sections[2], sections[0]));

    CHECK (near (doc::crossfadeInto (sections, 0), 0.0));
    CHECK (near (doc::crossfadeInto (sections, 1), 0.01));
    CHECK (near (doc::crossfadeInto (sections, 3), 0.0));
}

TEST_CASE ("media edit: where an edited second falls, in the file")
{
    const auto sections = introVerseChorus();

    auto place = doc::placeOf (sections, 15.0);
    REQUIRE (place.has_value());
    CHECK (place->index == 1);
    CHECK (near (place->fileSecond, 15.0));

    const std::vector<Section> chorusFirst { sections[2], sections[0], sections[1] };
    place = doc::placeOf (chorusFirst, 5.0);
    REQUIRE (place.has_value());
    CHECK (place->index == 0);
    CHECK (near (place->fileSecond, 25.0));

    /*  The very end belongs to the last section; past it is nowhere. */
    place = doc::placeOf (chorusFirst, 30.0);
    REQUIRE (place.has_value());
    CHECK (place->index == 2);
    CHECK (near (place->fileSecond, 20.0));

    CHECK_FALSE (doc::placeOf (chorusFirst, 31.0).has_value());
    CHECK_FALSE (doc::placeOf (chorusFirst, -1.0).has_value());
    CHECK_FALSE (doc::placeOf ({}, 0.0).has_value());
}

TEST_CASE ("media edit: no sections, or the whole file as recorded, is no edit")
{
    CHECK (doc::isIdentityEdit ({}, 30.0));
    CHECK (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0, -1.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 29.9) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.5, 30.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit (introVerseChorus(), 30.0));
}

TEST_CASE ("media edit: the text of an edit is the same in every locale, and leaves out the first crossfade")
{
    auto sections = introVerseChorus();
    const auto text = doc::editText (sections);

    CHECK (text == "0 10 0 0;10 20 0 0.01;20 30 0 0.01;");
    CHECK (text.find (',') == std::string::npos);

    /*  The first section's crossfade is not heard, so it is not in the key. */
    sections[0].crossfade = 0.5;
    CHECK (doc::editText (sections) == text);

    sections[1].trimDb = -6.0;
    CHECK (doc::editText (sections) == "0 10 0 0;10 20 -6 0.01;20 30 0 0.01;");
    CHECK (doc::editText ({}).empty());
}

//==============================================================================
TEST_CASE ("media edit: a split and a join are the identity on the timeline")
{
    const std::vector<Section> whole { piece ("A", 0.0, 30.0) };
    const std::vector<Section> split { piece ("A", 0.0, 10.0), piece ("B", 10.0, 30.0) };

    CHECK (sameRuns (doc::timeMap (whole, split), { { 0.0, 0.0, 10.0 }, { 10.0, 10.0, 20.0 } }));
    CHECK (sameRuns (doc::timeMap (split, whole), { { 0.0, 0.0, 10.0 }, { 10.0, 10.0, 20.0 } }));

    const auto lane = doc::readLevelLane ("4 0 5 -20 25 -20 26 0").points;
    CHECK (doc::laneText (doc::carryLane (lane, doc::timeMap (whole, split))) == "4 0 5 -20 25 -20 26 0");
}

TEST_CASE ("media edit: a move carries what sits on the moved material, and what it passed over")
{
    const auto before = introVerseChorus();
    const std::vector<Section> chorusFirst { before[2], before[0], before[1] };

    const auto runs = doc::timeMap (before, chorusFirst);
    CHECK (sameRuns (runs, { { 0.0, 10.0, 10.0 }, { 10.0, 20.0, 10.0 }, { 20.0, 0.0, 10.0 } }));

    CHECK (near (doc::carried (runs, 25.0), 5.0));
    CHECK (near (doc::carried (runs, 5.0), 15.0));

    /*  The sentence the author wrote: a dip over the verse stays over the
        verse when the verse moves. */
    const auto dip = doc::readLevelLane ("12 0 14 -20 16 -20 18 0").points;
    CHECK (doc::laneText (doc::carryLane (dip, runs)) == "22 0 24 -20 26 -20 28 0");
    CHECK (doc::carryLaneText ("12 0 14 -20 16 -20 18 0", runs) == "22 0 24 -20 26 -20 28 0");

    /*  Points on both sides of the move come back in order. */
    const auto across = doc::readLevelLane ("5 0 25 -10").points;
    CHECK (doc::laneText (doc::carryLane (across, runs)) == "5 -10 15 0");

    /*  A range inside the chorus moves with it; one spanning the intro and
        the chorus is left ending before it begins, and is marked. */
    const auto ranges = doc::carryRanges ({ { "R1", 22.0, 28.0, false }, { "R2", 5.0, 25.0, false } }, runs);
    REQUIRE (ranges.size() == 2);
    CHECK (near (ranges[0].in, 2.0));
    CHECK (near (ranges[0].out, 8.0));
    CHECK_FALSE (ranges[0].removed);
    CHECK (ranges[1].removed);

    CHECK (near (doc::carryStartOffset (22.0, runs), 2.0));
    CHECK (near (doc::carryStartOffset (0.0, runs), 0.0));
}

TEST_CASE ("media edit: a removal drops what sat on the removed material and lands the rest on the cut")
{
    const auto before = introVerseChorus();
    const std::vector<Section> noVerse { before[0], before[2] };

    const auto runs = doc::timeMap (before, noVerse);
    CHECK (sameRuns (runs, { { 0.0, 0.0, 10.0 }, { 20.0, 10.0, 10.0 } }));

    CHECK_FALSE (doc::carrySecond (runs, 15.0).has_value());
    CHECK (near (doc::carryToCut (runs, 15.0), 10.0));
    CHECK (near (doc::carried (runs, 25.0), 15.0));

    const auto lane = doc::readLevelLane ("5 0 12 -20 18 -20 25 0").points;
    CHECK (doc::laneText (doc::carryLane (lane, runs)) == "5 0 15 0");

    /*  Nothing left of a range in the verse; one across it is shortened to
        what remains. */
    const auto ranges = doc::carryRanges ({ { "R1", 12.0, 18.0, false }, { "R2", 5.0, 25.0, false } }, runs);
    REQUIRE (ranges.size() == 2);
    CHECK (ranges[0].removed);
    CHECK_FALSE (ranges[1].removed);
    CHECK (near (ranges[1].in, 5.0));
    CHECK (near (ranges[1].out, 15.0));

    CHECK (near (doc::carryStartOffset (15.0, runs), 10.0));

    /*  The whole edit removed but one section. */
    const std::vector<Section> onlyChorus { before[2] };
    CHECK (sameRuns (doc::timeMap (before, onlyChorus), { { 20.0, 0.0, 10.0 } }));
    CHECK (near (doc::carryToCut (doc::timeMap (before, onlyChorus), 5.0), 0.0));
}

TEST_CASE ("media edit: an edge moved loses or gains a sliver, and the rest shifts")
{
    const std::vector<Section> before { piece ("A", 0.0, 10.0), piece ("B", 10.0, 20.0) };
    const std::vector<Section> shorter { piece ("A", 0.0, 8.0), piece ("B", 10.0, 20.0) };

    auto runs = doc::timeMap (before, shorter);
    CHECK (sameRuns (runs, { { 0.0, 0.0, 8.0 }, { 10.0, 8.0, 10.0 } }));

    CHECK_FALSE (doc::carrySecond (runs, 9.0).has_value());
    CHECK (near (doc::carried (runs, 9.0), 8.0));
    CHECK (near (doc::carried (runs, 10.0), 8.0));
    CHECK (near (doc::carried (runs, 15.0), 13.0));

    /*  Two points landing on one instant: the earlier one is kept. */
    const auto lane = doc::readLevelLane ("8 -3 9 -5 10 -7").points;
    CHECK (doc::laneText (doc::carryLane (lane, runs)) == "8 -3");

    /*  Grown at the front: new material carries nothing, and what was there
        shifts. */
    const std::vector<Section> longer { piece ("A", 0.0, 10.0), piece ("B", 7.0, 20.0) };
    runs = doc::timeMap (before, longer);
    CHECK (sameRuns (runs, { { 0.0, 0.0, 10.0 }, { 10.0, 13.0, 10.0 } }));
    CHECK (near (doc::carried (runs, 12.0), 15.0));
}

TEST_CASE ("media edit: clearing the edit carries everything back to the file's own time")
{
    const auto before = introVerseChorus();
    const std::vector<Section> chorusFirst { before[2], before[0], before[1] };

    const auto runs = doc::timeMapToFile (chorusFirst);
    CHECK (sameRuns (runs, { { 0.0, 20.0, 10.0 }, { 10.0, 0.0, 10.0 }, { 20.0, 10.0, 10.0 } }));
    CHECK (near (doc::carried (runs, 5.0), 25.0));
    CHECK (doc::carryLaneText ("2 -6 12 0", runs) == "2 0 22 -6");
}

TEST_CASE ("media edit: the very end of the material survives, and nothing survives an empty map")
{
    const std::vector<Section> before { piece ("A", 0.0, 10.0) };
    const std::vector<Section> after { piece ("A", 0.0, 10.0) };

    const auto runs = doc::timeMap (before, after);
    CHECK (doc::carrySecond (runs, 10.0).has_value());
    CHECK (near (*doc::carrySecond (runs, 10.0), 10.0));
    CHECK_FALSE (doc::carrySecond (runs, 10.5).has_value());

    CHECK_FALSE (doc::carrySecond ({}, 3.0).has_value());
    CHECK (near (doc::carryToCut ({}, 3.0), 0.0));
    CHECK (doc::carryLane (doc::readLevelLane ("1 0 2 -3").points, {}).empty());
    CHECK (doc::carryLaneText ("1 0 2 -3", {}).empty());

    /*  A text that is no lane goes back as it came: the door never lets one
        into a document, so this is only what a carry does with garbage. */
    CHECK (doc::carryLaneText ("1 0 2", runs) == "1 0 2");
}

//==============================================================================
TEST_CASE ("media edit: a crossfade is held to the material and to the section's length")
{
    /*  Nothing before the file's start: a join into a section whose in point
        is nought is a hard cut, and one at two seconds may fade over four. */
    auto held = doc::clampCrossfades ({ piece ("A", 10.0, 20.0), piece ("B", 0.0, 5.0, 0.0, 0.2) });
    CHECK (near (held[1].crossfade, 0.0));

    held = doc::clampCrossfades ({ piece ("A", 10.0, 20.0), piece ("B", 2.0, 5.0, 0.0, 10.0) });
    CHECK (near (held[1].crossfade, 4.0));

    /*  Two crossfades on a short section: the larger shrinks until half of
        each fits inside it. */
    held = doc::clampCrossfades ({ piece ("A", 0.0, 10.0), piece ("B", 2.0, 3.0, 0.0, 1.6), piece ("C", 20.0, 30.0, 0.0, 0.6) });
    CHECK (near (held[1].crossfade, 1.4));
    CHECK (near (held[2].crossfade, 0.6));

    /*  Both too large for a very short section: the larger to what the other
        leaves, the other to the section itself. */
    held = doc::clampCrossfades ({ piece ("A", 0.0, 10.0), piece ("B", 2.0, 2.1, 0.0, 1.0), piece ("C", 20.0, 30.0, 0.0, 0.3) });
    CHECK (held[1].crossfade / 2.0 + held[2].crossfade / 2.0 <= 0.1 + 1.0e-9);

    /*  The first section's is left as stored - it is not heard - and the pair
        constraint counts it as nothing. */
    held = doc::clampCrossfades ({ piece ("A", 0.0, 1.0, 0.0, 5.0), piece ("B", 5.0, 6.0, 0.0, 2.0) });
    CHECK (near (held[0].crossfade, 5.0));
    CHECK (near (held[1].crossfade, 2.0));

    /*  Nothing to do leaves everything as it was. */
    const auto fine = introVerseChorus();
    held = doc::clampCrossfades (fine);
    CHECK (near (held[1].crossfade, 0.01));
    CHECK (near (held[2].crossfade, 0.01));
}

TEST_CASE ("media edit: why a list of sections is no edit")
{
    CHECK (doc::whyNotSections (introVerseChorus()).empty());
    CHECK (doc::whyNotSections ({}).empty());

    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0), piece ("B", 10.0, 10.0) }).find ("section 2: its out point is not after")
             != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", -1.0, 10.0) }).find ("before the file's start") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0, 13.0) }).find ("trim outside") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0, 0.0, -0.5) }).find ("crossfade below nought") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, std::nan ("")) }).find ("not a number") != std::string::npos);
}
