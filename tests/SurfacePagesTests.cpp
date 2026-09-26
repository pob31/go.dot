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

/*  A SURFACE'S EQ AND SEND PAGES, AS NUMBERS AND WORDS (author, 2026-09-25):
    the map, the laws a detent moves by, what a screen says and where a ring
    stands - pure, so pinned here without a surface or a tree. The bridge's
    own cases, bytes in and bytes out, are in SurfaceBridgeTests.cpp. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/surface/FaderCurve.h>
#include <wfg/engine/surface/SurfacePages.h>
#include <wfg/engine/surface/SurfaceProfile.h>

#include <clocale>
#include <cmath>
#include <set>
#include <string>
#include <string_view>

using namespace wfg;

namespace
{
    bool near (double a, double b, double within = 1.0e-9) { return std::abs (a - b) < within; }

    std::string text (surface::Law law, double value, bool compact = false)
    {
        std::string out;
        surface::valueText (law, value, compact, out);
        return out;
    }

    const doc::AttributeRow* soundRow (std::string_view name)
    {
        for (const auto* row : doc::Schema::rowsForOwner ("sound"))
            if (row->name == name)
                return row;

        return nullptr;
    }
}

//==============================================================================
TEST_CASE ("surface pages: the EQ map is the author's, and every row in it is the table's")
{
    using surface::eqControls;

    /*  "1.1 High Pass frequency ... 2.8 Low Pass frequency": the high-pass
        first, the low-pass last, band one's shape, frequency, gain, width
        before band two's frequency, gain, width - then band three's, band
        four's shape, frequency, gain, width. */
    const std::string_view expected[] = { "eqHpfFreq", "eqB1Shape", "eqB1Freq", "eqB1Gain", "eqB1Q",
                                          "eqB2Freq",  "eqB2Gain",  "eqB2Q",
                                          "eqB3Freq",  "eqB3Gain",  "eqB3Q",
                                          "eqB4Shape", "eqB4Freq",  "eqB4Gain", "eqB4Q",   "eqLpfFreq" };

    REQUIRE (eqControls.size() == std::size (expected));

    for (std::size_t i = 0; i < eqControls.size(); ++i)
    {
        const auto& control = eqControls[i];
        INFO ("control " << i << ", " << std::string (control.row));

        CHECK (control.row == expected[i]);
        CHECK (control.label.size() <= 12u);
        CHECK (control.shortLabel.size() <= 7u);

        //  THE TABLE'S RANGES, so a page never writes what the document refuses.
        const auto* row = soundRow (control.row);
        REQUIRE (row != nullptr);

        if (control.law != surface::Law::shape)
        {
            REQUIRE (row->hasMin);
            REQUIRE (row->hasMax);
            CHECK (near (control.minimum, row->minimum));
            CHECK (near (control.maximum, row->maximum));
        }

        //  And the switch it answers to is a flag of the table.
        const auto* switchRow = soundRow (control.switchRow);
        REQUIRE (switchRow != nullptr);
        CHECK (switchRow->type == doc::ValueType::boolean);
    }

    /*  A PRESS SWITCHES the filters on their frequency, each band on its gain
        ("press to toggle on or off"), and a shape between peak and shelf. */
    std::set<std::string_view> switched;

    for (const auto& control : eqControls)
        if (control.press == surface::Press::toggleSwitch)
            switched.insert (control.row);

    CHECK (switched == std::set<std::string_view> { "eqHpfFreq", "eqB1Gain", "eqB2Gain", "eqB3Gain",
                                                    "eqB4Gain", "eqLpfFreq" });

    CHECK (eqControls[1].press == surface::Press::toggleShape);
    CHECK (eqControls[1].shelf == "lowShelf");
    CHECK (eqControls[11].press == surface::Press::toggleShape);
    CHECK (eqControls[11].shelf == "highShelf");

    //  Each wears its band's colour: the high-pass, the four bands, the low-pass.
    CHECK (eqControls.front().colour == 0);
    CHECK (eqControls[3].colour == 1);
    CHECK (eqControls[9].colour == 3);
    CHECK (eqControls.back().colour == 5);
}

TEST_CASE ("surface pages: sixteen rotaries show the EQ at once, eight in two pages")
{
    CHECK (surface::pageCount (surface::eqControlCount, 16) == 1);
    CHECK (surface::pageCount (surface::eqControlCount, 8) == 2);
    CHECK (surface::pageCount (surface::eqControlCount, 12) == 2);
    CHECK (surface::pageCount (3, 8) == 1);
    CHECK (surface::pageCount (17, 16) == 2);

    //  Never nought: a page with nothing on it is still one page.
    CHECK (surface::pageCount (0, 8) == 1);
    CHECK (surface::pageCount (5, 0) == 1);
}

TEST_CASE ("surface pages: a detent moves each control by its law, within its range")
{
    using surface::Law;
    const auto law = surface::FaderLaw::d700;

    SUBCASE ("a frequency, a sixteenth of an octave a detent")
    {
        CHECK (near (surface::turned (Law::frequency, 100.0, 16, 20.0, 20000.0, law), 200.0));
        CHECK (near (surface::turned (Law::frequency, 100.0, 2, 20.0, 20000.0, law), 109.1));
        CHECK (near (surface::turned (Law::frequency, 2000.0, -16, 20.0, 20000.0, law), 1000.0));

        //  Rounded to the hertz above a kilohertz.
        CHECK (near (surface::turned (Law::frequency, 2000.0, 1, 20.0, 20000.0, law), 2089.0));

        //  Held at its ends.
        CHECK (near (surface::turned (Law::frequency, 20.0, -3, 20.0, 20000.0, law), 20.0));
        CHECK (near (surface::turned (Law::frequency, 1900.0, 40, 20.0, 2000.0, law), 2000.0));
    }

    SUBCASE ("a gain, half a decibel a detent")
    {
        CHECK (near (surface::turned (Law::gain, 0.0, 1, -24.0, 24.0, law), 0.5));
        CHECK (near (surface::turned (Law::gain, 0.0, -3, -24.0, 24.0, law), -1.5));
        CHECK (near (surface::turned (Law::gain, 23.8, 2, -24.0, 24.0, law), 24.0));
    }

    SUBCASE ("a width, an eighth of a doubling a detent")
    {
        CHECK (near (surface::turned (Law::width, 0.7, 8, 0.1, 10.0, law), 1.4));
        CHECK (near (surface::turned (Law::width, 0.7, -8, 0.1, 10.0, law), 0.35));
        CHECK (near (surface::turned (Law::width, 0.1, -1, 0.1, 10.0, law), 0.1));
    }

    SUBCASE ("a send's level, along the fader's travel")
    {
        //  From silence, the first step of the travel; back down, silence.
        const auto first = surface::turned (Law::level, -120.0, 1, -120.0, 12.0, law);
        CHECK (first > -120.0);
        CHECK (near (surface::turned (Law::level, first, -1, -120.0, 12.0, law), -120.0));

        //  Near nought a detent is a small step, as a fader's is.
        const auto up = surface::turned (Law::level, 0.0, 1, -120.0, 12.0, law);
        CHECK (up > 0.0);
        CHECK (up < 0.5);
    }

    SUBCASE ("a shape: clockwise the shelf, anticlockwise the peak, and a press between them")
    {
        CHECK (surface::turnedShape ("peak", 1, "lowShelf") == "lowShelf");
        CHECK (surface::turnedShape ("lowShelf", -2, "lowShelf") == "peak");
        CHECK (surface::turnedShape ("lowShelf", 0, "lowShelf") == "lowShelf");
        CHECK (surface::pressedShape ("peak", "highShelf") == "highShelf");
        CHECK (surface::pressedShape ("highShelf", "highShelf") == "peak");
    }
}

TEST_CASE ("surface pages: what a screen says a value is, in every locale")
{
    using surface::Law;

    /*  WRITTEN DIGIT BY DIGIT: under a locale whose point is a comma, a
        screen still reads "2.00 kHz". */
    const auto* before = std::setlocale (LC_NUMERIC, nullptr);
    const std::string restore = before != nullptr ? before : "C";

    for (const char* locale : { "C", "fr_FR.UTF-8", "French_France.1252" })
    {
        if (std::setlocale (LC_NUMERIC, locale) == nullptr)
            continue;

        INFO ("locale " << locale);

        CHECK (text (Law::frequency, 80.0) == "80 Hz");
        CHECK (text (Law::frequency, 20.9) == "20.9 Hz");
        CHECK (text (Law::frequency, 125.0) == "125 Hz");
        CHECK (text (Law::frequency, 2000.0) == "2.00 kHz");
        CHECK (text (Law::frequency, 12500.0) == "12.5 kHz");
        CHECK (text (Law::frequency, 2000.0, true) == "2.00kHz");

        CHECK (text (Law::gain, 3.5) == "+3.5 dB");
        CHECK (text (Law::gain, -12.0) == "-12.0 dB");
        CHECK (text (Law::gain, 0.0) == "0.0 dB");
        CHECK (text (Law::gain, 3.5, true) == "+3.5dB");

        CHECK (text (Law::width, 0.7) == "Q 0.70");
        CHECK (text (Law::width, 10.0) == "Q 10.0");

        CHECK (text (Law::level, -6.0) == "-6.0 dB");
        CHECK (text (Law::level, -120.0) == "-inf dB");
        CHECK (text (Law::level, -120.0, true) == "-inf");
    }

    std::setlocale (LC_NUMERIC, restore.c_str());

    CHECK (surface::shapeText ("peak", false) == "Peak");
    CHECK (surface::shapeText ("lowShelf", false) == "Lo shelf");
    CHECK (surface::shapeText ("highShelf", true) == "HiShelf");

    //  Every label and every text fits its field.
    for (const auto& control : surface::eqControls)
        CHECK (control.label.size() + std::string (" off").size() <= 12u);
}

TEST_CASE ("surface pages: a ring stands at its value - a gain from the centre, the rest from the left")
{
    using surface::Law;
    const auto law = surface::FaderLaw::d700;

    //  A gain of nought is the middle of a ring that fills from the centre.
    CHECK (surface::d700RingFor (Law::gain, 0.0, -24.0, 24.0, law) == surface::Ring { 64, 1 });
    CHECK (surface::d700RingFor (Law::gain, 24.0, -24.0, 24.0, law) == surface::Ring { 127, 1 });
    CHECK (surface::mcuRingFor (Law::gain, 0.0, -24.0, 24.0, law) == surface::Ring { 6, 1 });

    //  A frequency evenly in octaves: a kilohertz is 0.566 of the way from 20 to 20000.
    const auto kilohertz = surface::d700RingFor (Law::frequency, 1000.0, 20.0, 20000.0, law);
    CHECK (kilohertz.mode == 2);
    CHECK (kilohertz.value == 1 + static_cast<int> (std::lround (std::log (50.0) / std::log (1000.0) * 126.0)));

    //  The bottom of a frequency's range is not silence: one step stays lit.
    CHECK (surface::d700RingFor (Law::frequency, 20.0, 20.0, 20000.0, law) == surface::Ring { 1, 2 });

    //  A send at silence is an empty ring; at the fader's top, a full one.
    CHECK (surface::d700RingFor (Law::level, -120.0, -120.0, 12.0, law) == surface::Ring { 0, 2 });
    CHECK (surface::d700RingFor (Law::level, 12.0, -120.0, 12.0, law).value == 127);

    //  A shape: empty as a peak, full as a shelf.
    CHECK (surface::d700ShapeRing ("peak") == surface::Ring { 0, 2 });
    CHECK (surface::d700ShapeRing ("lowShelf") == surface::Ring { 127, 2 });
}

TEST_CASE ("surface pages: a page button blinks its page's number every second and a half")
{
    //  One page of its kind: lit steadily.
    for (std::int64_t tick = 0; tick < surface::pageBlinkCycleTicks; ++tick)
        CHECK (surface::pageButtonLit (0, 1, tick));

    /*  With more, page n blinks n times at the start of each cycle: count the
        dark-to-lit edges in one cycle. */
    const auto blinksOf = [] (int index, int count)
    {
        auto edges = 0;
        auto was = false;

        for (std::int64_t tick = 0; tick < surface::pageBlinkCycleTicks; ++tick)
        {
            const auto lit = surface::pageButtonLit (index, count, tick);
            edges += lit && ! was ? 1 : 0;
            was = lit;
        }

        return edges;
    };

    CHECK (blinksOf (0, 2) == 1);
    CHECK (blinksOf (1, 2) == 2);
    CHECK (blinksOf (2, 3) == 3);

    //  And the cycle is a second and a half, and repeats.
    CHECK (surface::pageBlinkCycleTicks == 75);
    CHECK (surface::pageButtonLit (1, 2, 0) == surface::pageButtonLit (1, 2, 75));
    CHECK_FALSE (surface::pageButtonLit (1, 2, 60));
}

TEST_CASE ("pages: a plugin parameter turns along its travel, a step at a time when it has steps, its ring from the centre when bipolar")
{
    //  Continuous: a hundred-and-twenty-eighth a detent, kept within 0..1.
    CHECK (surface::turnedParameter (0.5, 1, 0) == doctest::Approx (0.5 + 1.0 / 128.0));
    CHECK (surface::turnedParameter (0.5, -4, 0) == doctest::Approx (0.5 - 4.0 / 128.0));
    CHECK (surface::turnedParameter (0.999, 5, 0) == doctest::Approx (1.0));
    CHECK (surface::turnedParameter (0.001, -5, 0) == doctest::Approx (0.0));

    //  Stepped: one step a detent, landing on the steps.
    CHECK (surface::turnedParameter (0.5, 1, 3) == doctest::Approx (1.0));
    CHECK (surface::turnedParameter (0.0, 1, 5) == doctest::Approx (0.25));
    CHECK (surface::turnedParameter (0.26, -1, 5) == doctest::Approx (0.0));
    CHECK (surface::turnedParameter (1.0, 3, 2) == doctest::Approx (1.0));

    //  Rings: bipolar from the centre (fill 1), the rest from the left (fill 2).
    CHECK (surface::d700ParameterRing (0.5, true) == surface::Ring { 64, 1 });
    CHECK (surface::d700ParameterRing (0.0, false) == surface::Ring { 0, 2 });
    CHECK (surface::d700ParameterRing (1.0, false) == surface::Ring { 127, 2 });
    CHECK (surface::mcuParameterRing (0.5, true) == surface::Ring { 6, 1 });
    CHECK (surface::mcuParameterRing (1.0, false) == surface::Ring { 11, 2 });

    CHECK (surface::pageWord (surface::Page::fx) == "fx");
}

TEST_CASE ("surface pages: the master dial turns a number by what its own row says")
{
    /*  THE MASTER DIAL (author, 2026-09-26) turns whatever number was last
        clicked in the window, so its law is read off the row: type, unit and
        range. Each case here is a real row of the table, so a row whose unit
        or range changes shows up here as a law that changed with it. */
    const auto law = surface::FaderLaw::d700;

    const auto rangeOf = [] (std::string_view owner, std::string_view name)
    {
        surface::DialRange range;

        for (const auto* row : doc::Schema::rowsForOwner (owner))
            if (row->name == name)
            {
                range.integer = row->type == doc::ValueType::integer;
                range.hasMinimum = row->hasMin;
                range.minimum = row->minimum;
                range.hasMaximum = row->hasMax;
                range.maximum = row->maximum;
                range.unit = row->unit;
                return range;
            }

        FAIL ("no such row");
        return range;
    };

    const auto turned = [&] (std::string_view owner, std::string_view name, double value, int steps)
    {
        return surface::dialTurned (rangeOf (owner, name), value, steps, law);
    };

    SUBCASE ("a frequency, as a band's rotary turns it")
    {
        CHECK (near (turned ("sound", "eqB1Freq", 100.0, 16), 200.0));
        CHECK (near (turned ("sound", "eqHpfFreq", 1900.0, 40), 2000.0));
    }

    SUBCASE ("a decibel that reaches silence is a level, along the fader")
    {
        CHECK (near (turned ("sound", "level", -6.0, 1),
                     surface::turned (surface::Law::level, -6.0, 1, -120.0, 12.0, law)));
        CHECK (near (turned ("send", "level", -120.0, -1), -120.0));
    }

    SUBCASE ("a narrower decibel is a gain, half a decibel a detent")
    {
        CHECK (near (turned ("sound", "eqB2Gain", 0.0, 3), 1.5));
        CHECK (near (turned ("sound", "eqB2Gain", 23.5, 4), 24.0));
    }

    SUBCASE ("a width spans a hundredfold with no unit, and turns in ratios")
    {
        CHECK (near (turned ("sound", "eqB1Q", 0.7, 8), 1.4));
    }

    SUBCASE ("a time, a tenth of a second, and a whole one past ten")
    {
        CHECK (near (turned ("cue", "preWait", 0.0, 3), 0.3));
        CHECK (near (turned ("cue", "preWait", 0.0, -1), 0.0));
        CHECK (near (turned ("cue", "preWait", 9.9, 2), 11.0));
        CHECK (near (turned ("cue", "preWait", 11.0, -2), 9.9));
        CHECK (near (turned ("cue", "preWait", 30.0, 5), 35.0));
    }

    SUBCASE ("a whole number, one a detent, held at its ends")
    {
        CHECK (near (turned ("group", "loops", 3.0, 2), 5.0));
        CHECK (near (turned ("group", "loops", 0.0, -1), 0.0));
        CHECK (near (turned ("midi", "channel", 16.0, 3), 16.0));
    }

    SUBCASE ("no detent, no move")
    {
        CHECK (near (turned ("sound", "level", -6.0, 0), -6.0));
    }
}
