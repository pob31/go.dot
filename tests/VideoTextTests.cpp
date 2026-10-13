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
    A TEXT CUE'S WORDS DRAWN (namespace draft 61, VFX.3): where they lie on the
    canvas's frame, their outline and its blur, how they wrap - asked of what
    does not depend on which fonts a machine has, so the same cases hold on
    Windows, a Mac and a Linux runner: positions within a few percent, colours
    by kind rather than by pixel.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/cue/TextFields.h>
#include <wfg/engine/video/Blanks.h>
#include <wfg/engine/video/Blur.h>
#include <wfg/engine/video/TextLook.h>
#include <wfg/engine/video/TextRaster.h>

#include <juce_graphics/juce_graphics.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    video::TextLook lookOf (const std::string& words)
    {
        video::TextLook look;
        look.words = words;
        return look;
    }

    video::TextRaster drawn (const video::TextLook& look, double scale = 1.0)
    {
        return video::rasteriseText (look, 0xFFFFFF, 1920, 1080, scale);
    }

    double middleX (const video::TextRaster& r) { return (r.x0 + r.x1) / 2.0; }
    double middleY (const video::TextRaster& r) { return (r.y0 + r.y1) / 2.0; }

    /*  How many of the drawing's pixels are near `rgb` and mostly solid. */
    int pixelsNear (const juce::Image& image, std::uint32_t rgb)
    {
        int count = 0;

        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
            {
                const auto c = image.getPixelAt (x, y);

                if (c.getAlpha() > 200
                      && std::abs (static_cast<int> (c.getRed()) - static_cast<int> ((rgb >> 16) & 0xffu)) < 24
                      && std::abs (static_cast<int> (c.getGreen()) - static_cast<int> ((rgb >> 8) & 0xffu)) < 24
                      && std::abs (static_cast<int> (c.getBlue()) - static_cast<int> (rgb & 0xffu)) < 24)
                    ++count;
            }

        return count;
    }

    /*  How many are partly see-through: the soft edge of a blur. */
    int pixelsPartlyClear (const juce::Image& image)
    {
        int count = 0;

        for (int y = 0; y < image.getHeight(); ++y)
            for (int x = 0; x < image.getWidth(); ++x)
            {
                const auto alpha = image.getPixelAt (x, y).getAlpha();

                if (alpha > 8 && alpha < 247)
                    ++count;
            }

        return count;
    }
}

TEST_CASE ("video text: no words draw nothing; a title is drawn in the middle of the canvas, only its own bounds (§61)")
{
    CHECK_FALSE (drawn (lookOf ("")).image.isValid());
    CHECK_FALSE (drawn (lookOf ("  \n ")).image.isValid());

    const auto title = drawn (lookOf ("Title"));
    REQUIRE (title.image.isValid());

    //  In the middle, its bounds and not the canvas.
    CHECK (middleX (title) == doctest::Approx (0.5).epsilon (0.02));
    CHECK (middleY (title) == doctest::Approx (0.5).epsilon (0.03));
    CHECK (title.x1 - title.x0 < 0.6);
    CHECK (title.y1 - title.y0 < 0.3);

    //  Its picture is its bounds at the canvas's pixels.
    CHECK (title.image.getWidth() == doctest::Approx ((title.x1 - title.x0) * 1920.0).epsilon (0.01));
    CHECK (title.image.getHeight() == doctest::Approx ((title.y1 - title.y0) * 1080.0).epsilon (0.01));

    //  Letters in the fill's colour, the rest clear.
    CHECK (pixelsNear (title.image, 0xFFFFFF) > 100);
    CHECK (title.image.getPixelAt (0, 0).getAlpha() == 0);

    //  Bigger letters, bigger bounds.
    auto big = lookOf ("Title");
    big.fontSize = 16.0;
    const auto bigger = drawn (big);
    CHECK (bigger.y1 - bigger.y0 > 1.6 * (title.y1 - title.y0));
}

TEST_CASE ("video text: left, centre and right in the box; top, middle and bottom on the canvas (§61, AKJ)")
{
    auto look = lookOf ("Left or right");

    look.align = video::TextAlign::left;
    const auto left = drawn (look);
    look.align = video::TextAlign::right;
    const auto right = drawn (look);

    //  The box is 90 % of the width, centred: its sides at 5 % and 95 %.
    CHECK (left.x0 == doctest::Approx (0.05).epsilon (0.4));
    CHECK (right.x1 == doctest::Approx (0.95).epsilon (0.02));
    CHECK (left.x0 < right.x0);

    look.align = video::TextAlign::centre;
    look.alignV = video::TextAlignV::top;
    const auto top = drawn (look);
    look.alignV = video::TextAlignV::bottom;
    const auto bottom = drawn (look);

    /*  As far from the top and bottom as the box is from the sides: 5 % of
        1920 is 96 pixels, 8.9 % of 1080. */
    CHECK (top.y0 < 0.12);
    CHECK (top.y0 > 0.05);
    CHECK (bottom.y1 > 0.88);
    CHECK (bottom.y1 < 0.95);
    CHECK (middleY (top) < 0.5);
    CHECK (middleY (bottom) > 0.5);
}

TEST_CASE ("video text: words wrap in their box, lines stack, and the spacing spreads them (§61)")
{
    const std::string sentence = "A sentence long enough to need more than one line when its box is narrow";

    auto look = lookOf (sentence);
    look.wrap = 90.0;
    const auto wide = drawn (look);
    look.wrap = 30.0;
    const auto narrow = drawn (look);

    CHECK (narrow.y1 - narrow.y0 > 2.0 * (wide.y1 - wide.y0));
    CHECK (narrow.x1 - narrow.x0 < 0.31);

    //  Nought never wraps: as wide as it is.
    look.wrap = 0.0;
    const auto unwrapped = drawn (look);
    CHECK (unwrapped.y1 - unwrapped.y0 < 1.2 * (wide.y1 - wide.y0));

    //  Two lines are taller than one; spaced at twice, taller again.
    const auto one = drawn (lookOf ("One"));
    auto twoLines = lookOf ("One\nTwo");
    const auto two = drawn (twoLines);
    twoLines.lineSpacing = 200.0;
    const auto spaced = drawn (twoLines);

    CHECK (two.y1 - two.y0 > 1.7 * (one.y1 - one.y0));
    CHECK (spaced.y1 - spaced.y0 > 1.3 * (two.y1 - two.y0));
}

TEST_CASE ("video text: an outline of its own colour round the letters, and its blur a glow wider than it (§61, AKK, AKL)")
{
    auto look = lookOf ("Outlined");
    const auto plain = drawn (look);
    CHECK (pixelsNear (plain.image, 0xFF0000) == 0);

    look.outline = 6.0;
    look.outlineColour = 0xFF0000;
    const auto outlined = drawn (look);

    //  Red round white: both there, and the bounds wider by the outline.
    CHECK (pixelsNear (outlined.image, 0xFF0000) > 100);
    CHECK (pixelsNear (outlined.image, 0xFFFFFF) > 100);
    CHECK ((outlined.x1 - outlined.x0) * 1920.0 > (plain.x1 - plain.x0) * 1920.0 + 10.0);

    //  Blurred, the bounds reach further and the edge is soft.
    look.outlineBlur = 20.0;
    const auto glowing = drawn (look);
    CHECK ((glowing.x1 - glowing.x0) * 1920.0 > (outlined.x1 - outlined.x0) * 1920.0 + 30.0);
    CHECK (pixelsPartlyClear (glowing.image) > 2 * pixelsPartlyClear (outlined.image));

    //  The letters stay on top, in their own colour.
    CHECK (pixelsNear (glowing.image, 0xFFFFFF) > 100);
}

TEST_CASE ("video text: a title scaled up is drawn at more pixels, where it was (§61, AKL)")
{
    const auto once = drawn (lookOf ("Sharp"), 1.0);
    const auto twice = drawn (lookOf ("Sharp"), 2.0);
    const auto capped = drawn (lookOf ("Sharp"), 9.0);

    CHECK (twice.image.getWidth() == doctest::Approx (2.0 * once.image.getWidth()).epsilon (0.03));
    CHECK (middleX (twice) == doctest::Approx (middleX (once)).epsilon (0.01));
    CHECK (twice.y1 - twice.y0 == doctest::Approx (once.y1 - once.y0).epsilon (0.03));

    //  Four times at most.
    CHECK (capped.image.getWidth() == doctest::Approx (4.0 * once.image.getWidth()).epsilon (0.03));
}

TEST_CASE ("video text: words cut at a character, never half of one (§61, AKM)")
{
    CHECK (video::cutAtCharacter ("abc", 5) == "abc");
    CHECK (video::cutAtCharacter ("abcdef", 3) == "abc");

    //  "é" is two bytes: a cut through it leaves it out whole.
    CHECK (video::cutAtCharacter ("a\xc3\xa9", 2) == "a");
    CHECK (video::cutAtCharacter ("a\xc3\xa9", 3) == "a\xc3\xa9");

    //  A three-byte character cut after its first or second byte.
    CHECK (video::cutAtCharacter ("x\xe2\x80\x94y", 2) == "x");
    CHECK (video::cutAtCharacter ("x\xe2\x80\x94y", 3) == "x");
    CHECK (video::cutAtCharacter ("x\xe2\x80\x94y", 4) == "x\xe2\x80\x94");
}

TEST_CASE ("video blur: nothing at nought, a point spread and its sum kept, a wider radius further, the edges held (§61)")
{
    constexpr int size = 101;
    std::vector<float> plane (size * size, 0.0f);
    plane[50 * size + 50] = 1.0f;

    auto same = plane;
    video::blurPlane (same, size, size, 0.0);
    CHECK (same == plane);

    auto narrow = plane;
    video::blurPlane (narrow, size, size, 6.0);
    auto wide = plane;
    video::blurPlane (wide, size, size, 24.0);

    double sum = 0.0;

    for (const auto value : narrow)
        sum += value;

    CHECK (sum == doctest::Approx (1.0).epsilon (1.0e-3));
    CHECK (narrow[50 * size + 50] < 0.2f);
    CHECK (narrow[50 * size + 50] > wide[50 * size + 50]);
    CHECK (narrow[50 * size + 60] < 1.0e-4f);          // past its radius, nothing
    CHECK (wide[50 * size + 60] > 1.0e-4f);            // inside the wider one's

    //  A plane all one value stays that value, its edges held.
    std::vector<float> flat (size * size, 0.75f);
    video::blurPlane (flat, size, size, 30.0);
    CHECK (flat[0] == doctest::Approx (0.75f));
    CHECK (flat[size * size - 1] == doctest::Approx (0.75f));
}

TEST_CASE ("video text: a font this machine has not got is named, once; the default is never (§61, AKL)")
{
    const auto missing = video::fontsNotInstalled ({ "", "No Such Font Sixty-One", "No Such Font Sixty-One" });
    REQUIRE (missing.size() == 1);
    CHECK (missing.front() == "No Such Font Sixty-One");

    //  A family the machine does have is not named: any one it lists.
    const auto installed = juce::Font::findAllTypefaceNames();

    if (! installed.isEmpty())
        CHECK (video::fontsNotInstalled ({ installed[0].toStdString() }).empty());
}

TEST_CASE ("video blanks: a field's name, its blanks found once each and filled, braces written out, a loose brace shown and said (§61, AKM)")
{
    CHECK (video::isFieldName ("score"));
    CHECK (video::isFieldName ("home_team-2"));
    CHECK (video::isFieldName (std::string (32, 'a')));
    CHECK_FALSE (video::isFieldName (std::string (33, 'a')));
    CHECK_FALSE (video::isFieldName (""));
    CHECK_FALSE (video::isFieldName ("two words"));
    CHECK_FALSE (video::isFieldName ("caf\xc3\xa9"));

    CHECK (video::blanksIn ("{home} {away} {home}") == std::vector<std::string> { "home", "away" });
    CHECK (video::blanksIn ("{{home}} {not a name} {} {open").empty());

    const auto values = [] (const std::string& name) { return name == "home" ? std::string ("3") : std::string {}; };

    CHECK (video::fillBlanks ("Score: {home} - {away}", values) == "Score: 3 - ");
    CHECK (video::fillBlanks ("{{home}} is {home}", values) == "{home} is 3");
    CHECK (video::fillBlanks ("a } b }} c", values) == "a } b } c");
    CHECK (video::fillBlanks ("{not a name} {open", values) == "{not a name} {open");
    CHECK (video::fillBlanks ("", values).empty());

    CHECK (video::hasLooseBrace ("{not a name}"));
    CHECK (video::hasLooseBrace ("an {open"));
    CHECK_FALSE (video::hasLooseBrace ("{home} {{x}} and a } alone"));
}

TEST_CASE ("text fields: a value set and cut at a character, the revision moving only on a change; a name that does not read, and a 257th, refused (§61, AKM)")
{
    cue::TextFields fields;
    CHECK (fields.revision() == 0u);
    CHECK (fields.valueOf ("score").empty());

    CHECK (fields.set ("score", "12") == cue::TextFields::Set::written);
    CHECK (fields.valueOf ("score") == "12");
    CHECK (fields.revision() == 1u);

    //  The same value again moves nothing.
    CHECK (fields.set ("score", "12") == cue::TextFields::Set::written);
    CHECK (fields.revision() == 1u);

    CHECK (fields.set ("two words", "x") == cue::TextFields::Set::badName);
    CHECK (fields.set (std::string (33, 'a'), "x") == cue::TextFields::Set::badName);
    CHECK (fields.revision() == 1u);

    //  At most 1024 bytes, never half a letter.
    CHECK (fields.set ("long", std::string (1023, 'a') + "\xc3\xa9") == cue::TextFields::Set::written);
    CHECK (fields.valueOf ("long").size() == 1023u);

    //  At most 256 names: a new one past them refused, one held still written.
    for (int n = 0; fields.all().size() < cue::TextFields::mostNames; ++n)
        REQUIRE (fields.set ("f" + std::to_string (n), "x") == cue::TextFields::Set::written);

    CHECK (fields.set ("oneMore", "x") == cue::TextFields::Set::full);
    CHECK (fields.set ("score", "13") == cue::TextFields::Set::written);

    //  A seat's whole set replaces them; the same set again moves nothing; a close forgets them.
    const auto before = fields.revision();
    fields.replaceAll ({ { "home", "1" } });
    CHECK (fields.all().size() == 1u);
    CHECK (fields.revision() == before + 1);
    fields.replaceAll ({ { "home", "1" } });
    CHECK (fields.revision() == before + 1);
    fields.clear();
    CHECK (fields.all().empty());

    CHECK (cue::isTextFieldAddress ("/godot/text/score"));
    CHECK_FALSE (cue::isTextFieldAddress ("/godot/text/"));
    CHECK_FALSE (cue::isTextFieldAddress ("/godot/textual/score"));
}

namespace
{
    /*  A PICTURE OF THREE CHANNELS, all one value - or one point lit in its middle. */
    std::vector<double> flatPicture (int width, int height, double value)
    {
        return std::vector<double> (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 3, value);
    }

    double at (const std::vector<double>& rgb, int x, int y, int width, int channel = 0)
    {
        return rgb[video::pyramid::indexOf (x, y, width, channel)];
    }

    /*  How far a lit point spread across, as the standard deviation of its
        row through the middle, weighed by what it holds. */
    double measuredSpread (const std::vector<double>& rgb, int width, int y)
    {
        double sum = 0.0, mean = 0.0;

        for (int x = 0; x < width; ++x)
        {
            sum += at (rgb, x, y, width);
            mean += x * at (rgb, x, y, width);
        }

        mean /= sum;
        double variance = 0.0;

        for (int x = 0; x < width; ++x)
            variance += (x - mean) * (x - mean) * at (rgb, x, y, width);

        return std::sqrt (variance / sum);
    }
}

TEST_CASE ("video blur cue: the halvings planned so the spread left is eight points or less, and what they blur taken off (§61, AKO)")
{
    CHECK (video::pyramid::planFor (0.0).halvings == 0);
    CHECK (video::pyramid::planFor (0.0).spread == doctest::Approx (0.0));

    const auto small = video::pyramid::planFor (5.0);
    CHECK (small.halvings == 0);
    CHECK (small.spread == doctest::Approx (5.0));

    const auto edge = video::pyramid::planFor (8.0);
    CHECK (edge.halvings == 0);

    //  Past eight, halved once: (8.5^2 - 3/12 - 4/6) / 4, rooted.
    const auto once = video::pyramid::planFor (8.5);
    CHECK (once.halvings == 1);
    CHECK (once.spread == doctest::Approx (std::sqrt ((8.5 * 8.5 - 0.25 - 4.0 / 6.0) / 4.0)));

    CHECK (video::pyramid::planFor (40.0).halvings == 3);
    CHECK (video::pyramid::planFor (166.0).halvings == 5);

    //  Taps summing to one, out to three spreads.
    const auto taps = video::pyramid::tapsFor (2.0);
    CHECK (taps.size() == 7u);
    double sum = taps[0];

    for (std::size_t n = 1; n < taps.size(); ++n)
        sum += 2.0 * taps[n];

    CHECK (sum == doctest::Approx (1.0));
}

TEST_CASE ("video blur cue: nothing at nought, a flat picture left as it is, a lit point spread as asked at every level, its sum kept (§61, AKO)")
{
    auto flat = flatPicture (64, 48, 200.0);
    const auto before = flat;
    video::blurPicture (flat, 64, 48, 0.0);
    CHECK (flat == before);

    video::blurPicture (flat, 64, 48, 30.0);

    for (const auto value : flat)
        CHECK (value == doctest::Approx (200.0));

    //  A lit point, blurred at spreads on either side of each halving: the spread measured is the spread asked.
    for (const auto sigma : { 3.0, 7.9, 8.1, 15.0, 17.0, 40.0 })
    {
        INFO ("sigma " << sigma);
        const auto size = sigma > 20.0 ? 512 : 256;
        auto point = flatPicture (size, size, 0.0);

        for (int c = 0; c < 3; ++c)
            point[video::pyramid::indexOf (size / 2, size / 2, size, c)] = 1000.0;

        video::blurPicture (point, size, size, sigma);

        double total = 0.0;

        for (std::size_t n = 0; n < point.size(); n += 3)
            total += point[n];

        CHECK (total == doctest::Approx (1000.0).epsilon (0.01));
        CHECK (measuredSpread (point, size, size / 2) == doctest::Approx (sigma).epsilon (0.06));
    }
}

TEST_CASE ("video blur cue: a radius fading through the place one more halving is taken never jumps (§61, AKO)")
{
    //  A hard edge, black to white, across a picture.
    constexpr int width = 200, height = 40;
    auto edge = flatPicture (width, height, 0.0);

    for (int y = 0; y < height; ++y)
        for (int x = width / 2; x < width; ++x)
            for (int c = 0; c < 3; ++c)
                edge[video::pyramid::indexOf (x, y, width, c)] = 255.0;

    for (const auto from : { 7.5, 15.5 })
    {
        std::vector<double> last;

        for (int step = 0; step <= 10; ++step)
        {
            const auto sigma = from + 0.1 * step;
            auto blurred = edge;
            video::blurPicture (blurred, width, height, sigma);

            if (! last.empty())
            {
                double worst = 0.0;

                for (std::size_t n = 0; n < blurred.size(); ++n)
                    worst = std::max (worst, std::abs (blurred[n] - last[n]));

                INFO ("from sigma " << sigma - 0.1 << " to " << sigma << ": a point moved by " << worst);
                CHECK (worst <= 3.0);
            }

            last = std::move (blurred);
        }
    }
}
