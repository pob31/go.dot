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
    /*  A section as a test spells it: the fades the default ten milliseconds
        unless said, no gap unless said (namespace draft 55.9). */
    Section piece (const char* id, double in, double out, double trim = 0.0, double fadeIn = 0.01,
                   double fadeOut = 0.01, double gap = 0.0)
    {
        Section section;
        section.id = id;
        section.in = in;
        section.out = out;
        section.trimDb = trim;
        section.fadeIn = fadeIn;
        section.fadeOut = fadeOut;
        section.gap = gap;
        return section;
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

    /*  THE FADES AS HEARD (55.9): the first's fade in and the last's fade out
        lie inside, from and to silence; the two joins, still one in the
        file, play plain. */
    const auto heard = doc::heardFades (sections);
    REQUIRE (heard.size() == 3u);
    CHECK (near (heard[0].in, 0.01));
    CHECK_FALSE (heard[0].inCentred);
    CHECK (heard[0].plainOut);
    CHECK (near (heard[0].out, 0.0));
    CHECK (heard[1].plainIn);
    CHECK (near (heard[1].in, 0.0));
    CHECK (near (heard[2].out, 0.01));
    CHECK_FALSE (heard[2].outCentred);

    /*  Parted by a move, a join is heard, centred - the intro's fade in held
        to nothing, there being nothing before the file's start. */
    const auto parted = doc::heardFades (chorusFirst);
    CHECK (parted[1].inCentred);
    CHECK (near (parted[1].in, 0.0));
    CHECK (parted[0].outCentred);
    CHECK (near (parted[0].out, 0.01));
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
    CHECK (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0, 0.0, 0.0, 0.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0) }, 30.0));   // ten milliseconds fading in and out
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0, -1.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 29.9) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.5, 30.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit (introVerseChorus(), 30.0));
}

TEST_CASE ("media edit: the text of an edit is the same in every locale, and holds what is heard")
{
    auto sections = introVerseChorus();
    const auto text = doc::editText (sections);

    //  in out trim gap fadeIn fadeOut curveIn curveOut, as heard.
    CHECK (text == "0 10 0 0 0.01 0 0 0;10 20 0 0 0 0 0 0;20 30 0 0 0 0.01 0 0;");
    CHECK (text.find (',') == std::string::npos);

    /*  A fade at a join still one in the file is not heard, so it is not in the key. */
    sections[1].fadeIn = 0.5;
    sections[0].fadeOut = 0.5;
    CHECK (doc::editText (sections) == text);

    sections[1].trimDb = -6.0;
    CHECK (doc::editText (sections) == "0 10 0 0 0.01 0 0 0;10 20 -6 0 0 0 0 0;20 30 0 0 0 0.01 0 0;");

    /*  A gap and a curve are: the silence the timeline has, the shape heard. */
    sections[2].gap = 2.5;
    sections[2].fadeOutCurve = -0.5;
    CHECK (doc::editText (sections) == "0 10 0 0 0.01 0 0 0;10 20 -6 0 0 0.01 0 0;20 30 0 2.5 0.01 0.01 0 -0.5;");
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
    CHECK (doc::writeLaneText (doc::carryLane (lane, doc::timeMap (whole, split))) == "4 0 5 -20 25 -20 26 0");
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
    CHECK (doc::writeLaneText (doc::carryLane (dip, runs)) == "22 0 24 -20 26 -20 28 0");
    CHECK (doc::carryLaneText ("12 0 14 -20 16 -20 18 0", runs) == "22 0 24 -20 26 -20 28 0");

    /*  Points on both sides of the move come back in order. */
    const auto across = doc::readLevelLane ("5 0 25 -10").points;
    CHECK (doc::writeLaneText (doc::carryLane (across, runs)) == "5 -10 15 0");

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
    CHECK (doc::writeLaneText (doc::carryLane (lane, runs)) == "5 0 15 0");

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
    CHECK (doc::writeLaneText (doc::carryLane (lane, runs)) == "8 -3");

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
TEST_CASE ("media edit: the fades are held to the material and to the section's length")
{
    /*  Nothing before the file's start: a fade in centred on a join into a
        section whose in point is nought is a hard cut, and one at two
        seconds may reach back over four. */
    auto held = doc::clampFades ({ piece ("A", 10.0, 20.0), piece ("B", 0.0, 5.0, 0.0, 0.2) });
    CHECK (near (held[1].fadeIn, 0.0));

    held = doc::clampFades ({ piece ("A", 10.0, 20.0), piece ("B", 2.0, 5.0, 0.0, 10.0) });
    CHECK (near (held[1].fadeIn, 4.0));

    /*  Two fades on a short section, both at joins: the larger shrinks until
        half of each fits inside it. */
    held = doc::clampFades ({ piece ("A", 0.0, 10.0), piece ("B", 2.0, 3.0, 0.0, 1.6, 0.6), piece ("C", 20.0, 30.0) });
    CHECK (near (held[1].fadeIn, 1.4));
    CHECK (near (held[1].fadeOut, 0.6));

    /*  Both too large for a very short section: the larger to what the other
        leaves, the other to the section itself. */
    held = doc::clampFades ({ piece ("A", 0.0, 10.0), piece ("B", 2.0, 2.1, 0.0, 1.0, 0.3), piece ("C", 20.0, 30.0) });
    CHECK (held[1].fadeIn / 2.0 + held[1].fadeOut / 2.0 <= 0.1 + 1.0e-9);

    /*  AT A FREE EDGE (55.9) a fade lies inside its section, all of it: the
        first section's fade in from silence is held to what is left of it. */
    held = doc::clampFades ({ piece ("A", 0.0, 1.0, 0.0, 5.0), piece ("B", 5.0, 9.0, 0.0, 2.0) });
    CHECK (near (held[0].fadeIn, 1.0 - 0.005));
    CHECK (near (held[1].fadeIn, 2.0));

    /*  A gap makes both edges free: no reach before the in point, the whole
        fade inside. */
    held = doc::clampFades ({ piece ("A", 0.0, 10.0, 0.0, 0.0, 4.0), piece ("B", 0.2, 1.2, 0.0, 4.0, 0.01, 2.0) });
    CHECK (near (held[0].fadeOut, 4.0));
    CHECK (near (held[1].fadeIn, 0.99));

    /*  Curves to -1..1, a negative or a non-number to nought. */
    auto bent = introVerseChorus();
    bent[0].fadeInCurve = 3.0;
    bent[1].fadeOutCurve = -2.0;
    bent[2].fadeIn = -1.0;
    bent[2].fadeOut = std::nan ("");
    held = doc::clampFades (bent);
    CHECK (near (held[0].fadeInCurve, 1.0));
    CHECK (near (held[1].fadeOutCurve, -1.0));
    CHECK (near (held[2].fadeIn, 0.0));
    CHECK (near (held[2].fadeOut, 0.0));

    /*  Nothing to do leaves everything as it was. */
    const auto fine = introVerseChorus();
    held = doc::clampFades (fine);
    CHECK (near (held[1].fadeIn, 0.01));
    CHECK (near (held[2].fadeOut, 0.01));
}

//==============================================================================
TEST_CASE ("media edit: a gap is silence on the timeline - the starts, the length, no place, and a join is two sections touching")
{
    const std::vector<Section> gapped { piece ("A", 0.0, 10.0), piece ("B", 20.0, 30.0, 0.0, 0.01, 0.01, 4.0),
                                        piece ("C", 10.0, 12.0) };
    const auto starts = doc::sectionStarts (gapped);
    CHECK (near (starts[1], 14.0));
    CHECK (near (starts[2], 24.0));
    CHECK (near (doc::editedLength (gapped), 26.0));

    CHECK_FALSE (doc::placeOf (gapped, 12.0).has_value());
    CHECK (doc::placeOf (gapped, 15.0)->index == 1);
    CHECK (near (doc::placeOf (gapped, 15.0)->fileSecond, 21.0));

    CHECK_FALSE (doc::isJoin (gapped, 0));
    CHECK_FALSE (doc::isJoin (gapped, 1));
    CHECK (doc::isJoin (gapped, 2));

    /*  A gap under a millisecond is none, and the timeline says so too. */
    auto tiny = gapped;
    tiny[1].gap = 0.0004;
    CHECK (doc::isJoin (tiny, 1));
    CHECK (near (doc::sectionStarts (tiny)[1], 10.0));

    /*  A section with a gap before it, or at the start, is no identity edit. */
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0, 0.0, 0.0, 0.0, 1.0) }, 30.0));
    CHECK (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0, 0.0, 0.0, 0.0) }, 30.0));
    CHECK_FALSE (doc::isIdentityEdit ({ piece ("A", 0.0, 30.0, 0.0, 0.5, 0.0) }, 30.0));
}

TEST_CASE ("media edit: a fade's gain - equal power for a sound, straight for a picture, bent by its curve")
{
    CHECK (near (doc::fadeGain (0.0, 0.0, false), 0.0));
    CHECK (near (doc::fadeGain (1.0, 0.0, false), 1.0));
    CHECK (std::abs (doc::fadeGain (0.5, 0.0, false) - std::sqrt (0.5)) < 1.0e-12);   // -3 dB
    CHECK (std::abs (doc::fadeGain (0.5, 1.0, false) - std::pow (0.5, 0.25)) < 1.0e-12);   // -1.5 dB
    CHECK (std::abs (doc::fadeGain (0.5, -1.0, false) - 0.5) < 1.0e-12);              // -6 dB
    CHECK (near (doc::fadeGain (0.25, 0.0, true), 0.25));
    CHECK (near (doc::fadeGain (0.25, 1.0, true), 0.5));
    CHECK (near (doc::fadeGain (2.0, 0.0, false), 1.0));
    CHECK (near (doc::fadeGain (-1.0, 0.0, false), 0.0));

    /*  At -1 on both sides of a sound's join the two gains add to one
        everywhere (AEB): sin squared and cos squared. */
    for (const auto p : { 0.1, 0.3, 0.5, 0.8 })
        CHECK (std::abs (doc::fadeGain (p, -1.0, false) + doc::fadeGain (1.0 - p, -1.0, false) - 1.0) < 1.0e-12);
}

TEST_CASE ("media edit: a section's weight - nothing before or after where it is heard, one between, its fades centred on a join or inside a free edge")
{
    doc::Fades centred;
    centred.in = 0.2;
    centred.out = 0.4;
    centred.inCentred = true;
    centred.outCentred = true;

    //  A section from 10 to 20: heard from 9.9 to 20.2.
    CHECK (near (doc::heardFrom (10.0, centred), 9.9));
    CHECK (near (doc::heardTo (10.0, 10.0, centred), 20.2));
    CHECK (near (doc::fadeWeight (centred, 10.0, 10.0, 9.85, false), 0.0));
    CHECK (std::abs (doc::fadeWeight (centred, 10.0, 10.0, 10.0, false) - std::sqrt (0.5)) < 1.0e-12);
    CHECK (near (doc::fadeWeight (centred, 10.0, 10.0, 15.0, false), 1.0));
    CHECK (std::abs (doc::fadeWeight (centred, 10.0, 10.0, 20.0, false) - std::sqrt (0.5)) < 1.0e-12);
    CHECK (near (doc::fadeWeight (centred, 10.0, 10.0, 20.25, false), 0.0));

    doc::Fades free;
    free.in = 1.0;
    free.out = 0.0;

    //  Inside: from silence at the edge, full at its length; cut at the end.
    CHECK (near (doc::heardFrom (10.0, free), 10.0));
    CHECK (near (doc::fadeWeight (free, 10.0, 10.0, 9.99, true), 0.0));
    CHECK (near (doc::fadeWeight (free, 10.0, 10.0, 10.25, true), 0.25));
    CHECK (near (doc::fadeWeight (free, 10.0, 10.0, 19.99, true), 1.0));
    CHECK (near (doc::fadeWeight (free, 10.0, 10.0, 20.0, true), 0.0));
}

TEST_CASE ("media edit: the silence before a section is carried - in place for a trim into it, with the section for a ripple")
{
    /*  A TRIM INTO THE SILENCE (AEC): B's in point a second later and its
        gap a second longer - its material where it was, so a point over
        the old silence and one over the kept material stay; one over the
        trimmed second goes. */
    const std::vector<Section> before { piece ("A", 0.0, 10.0), piece ("B", 20.0, 30.0, 0.0, 0.01, 0.01, 2.0) };
    const std::vector<Section> trimmed { piece ("A", 0.0, 10.0), piece ("B", 21.0, 30.0, 0.0, 0.01, 0.01, 3.0) };
    auto runs = doc::timeMap (before, trimmed);
    CHECK (near (doc::carried (runs, 11.0), 11.0));
    CHECK (near (doc::carried (runs, 16.0), 16.0));
    CHECK_FALSE (doc::carrySecond (runs, 12.5).has_value());

    /*  A RIPPLE: A removed, B moved earlier with its silence before it. */
    const std::vector<Section> rippled { piece ("B", 20.0, 30.0, 0.0, 0.01, 0.01, 2.0) };
    runs = doc::timeMap (before, rippled);
    CHECK (near (doc::carried (runs, 11.0), 1.0));
    CHECK (near (doc::carried (runs, 15.0), 5.0));

    /*  LEFT AS SILENCE: A removed, its time on B's gap - nothing after moves. */
    const std::vector<Section> lifted { piece ("B", 20.0, 30.0, 0.0, 0.01, 0.01, 12.0) };
    runs = doc::timeMap (before, lifted);
    CHECK (near (doc::carried (runs, 11.0), 11.0));
    CHECK (near (doc::carried (runs, 15.0), 15.0));
    CHECK_FALSE (doc::carrySecond (runs, 5.0).has_value());
}

TEST_CASE ("media edit: why a list of sections is no edit")
{
    CHECK (doc::whyNotSections (introVerseChorus()).empty());
    CHECK (doc::whyNotSections ({}).empty());

    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0), piece ("B", 10.0, 10.0) }).find ("section 2: its out point is not after")
             != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", -1.0, 10.0) }).find ("before the file's start") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0, 13.0) }).find ("trim outside") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0, 0.0, -0.5) }).find ("fade below nought") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, 10.0, 0.0, 0.01, 0.01, -1.0) }).find ("gap below nought") != std::string::npos);
    auto bent = piece ("A", 0.0, 10.0);
    bent.fadeOutCurve = 1.5;
    CHECK (doc::whyNotSections ({ bent }).find ("curve outside") != std::string::npos);
    CHECK (doc::whyNotSections ({ piece ("A", 0.0, std::nan ("")) }).find ("not a number") != std::string::npos);
}
