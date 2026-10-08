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
    THE SPACEMOUSE (namespace draft 45, O.11): a report read into six axes, a
    push turned into a step - spatcore's `AxisMapping::process` in doubles -
    the step a curve the puck moves takes each tick, and the movement guessed
    when a curve is made. The puck itself is the bench's: these are the rules
    either side of it.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/cue/CurveCommands.h>
#include <wfg/engine/cue/CurveTable.h>
#include <wfg/engine/cue/RateStep.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/surface/PuckReport.h>
#include <wfg/engine/surface/SpaceMouse.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace wfg;

TEST_CASE ("spacemouse: a report is read into six axes and two buttons, the rest left as they were")
{
    surface::PuckState state;

    //  The newer pucks' one report: the three pushes, then the three twists.
    const std::uint8_t both[] { 1, 0x5e, 0x01, 0xa2, 0xfe, 0x00, 0x00,     //  350, -350, 0
                                   0xaf, 0x00, 0x00, 0x00, 0x51, 0xff };   //  175, 0, -175
    surface::readPuckReport (both, sizeof (both), state);

    CHECK (state.axes[0] == doctest::Approx (1.0));
    CHECK (state.axes[1] == doctest::Approx (-1.0));
    CHECK (state.axes[2] == doctest::Approx (0.0));
    CHECK (state.axes[3] == doctest::Approx (0.5));
    CHECK (state.axes[5] == doctest::Approx (-0.5));

    //  The older ones' twists alone, and pushes past the stop held at one.
    const std::uint8_t twists[] { 2, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00 };   //  0, 1000, 0
    surface::readPuckReport (twists, sizeof (twists), state);
    CHECK (state.axes[3] == doctest::Approx (0.0));
    CHECK (state.axes[4] == doctest::Approx (1.0));
    CHECK (state.axes[0] == doctest::Approx (1.0));       // untouched

    const std::uint8_t buttons[] { 3, 0x02 };
    surface::readPuckReport (buttons, sizeof (buttons), state);
    CHECK_FALSE (state.buttons[0]);
    CHECK (state.buttons[1]);

    //  A short report, or one of another kind, changes nothing.
    const std::uint8_t shortOne[] { 1, 0x10 };
    const std::uint8_t led[] { 4, 1 };
    surface::readPuckReport (shortOne, sizeof (shortOne), state);
    surface::readPuckReport (led, sizeof (led), state);
    CHECK (state.axes[0] == doctest::Approx (1.0));
}

TEST_CASE ("spacemouse: a push counts past the dead zone, rescaled from its edge, and a step moves at the speed")
{
    //  Inside 0.05, nothing; at the stop, one; half way past the edge, half.
    CHECK (cue::deflectionOf (0.04, false) == 0.0);
    CHECK (cue::deflectionOf (-0.04, false) == 0.0);
    CHECK (cue::deflectionOf (1.0, false) == doctest::Approx (1.0));
    CHECK (cue::deflectionOf (0.525, false) == doctest::Approx (0.5));
    CHECK (cue::deflectionOf (0.525, true) == doctest::Approx (-0.5));
    CHECK (cue::deflectionOf (-1.0, false) == doctest::Approx (-1.0));

    //  Full push at two units a second, for a fiftieth: four hundredths.
    CHECK (cue::rateStep (1.0, 1.0, 2.0, 0.02, std::nullopt) == doctest::Approx (1.04));

    //  Held inside its bounds.
    CHECK (cue::rateStep (9.99, 1.0, 2.0, 0.02, cue::RateBounds { -10.0, 10.0 }) == doctest::Approx (10.0));
    CHECK (cue::rateStep (-9.99, -1.0, 2.0, 0.02, cue::RateBounds { -10.0, 10.0 }) == doctest::Approx (-10.0));

    CHECK (cue::puckAxisIndex ("tx") == 0);
    CHECK (cue::puckAxisIndex ("rz") == 5);
    CHECK (cue::puckAxisIndex ("none") == -1);
}

TEST_CASE ("spacemouse: a curve is made with the puck's movement guessed from its message")
{
    doc::ShowDocument document;
    const auto list = document.createList ("Sound").id;

    const auto osc = [&document, &list] (const std::string& address, const std::string& value)
    {
        const auto id = document.createCue (list, 0, "osc", "Move").id;
        REQUIRE (document.setAttribute ("/godot/cue/" + id + "/address", address).ok);
        REQUIRE (document.setAttribute ("/godot/cue/" + id + "/value", value).ok);
        return id;
    };

    const auto row = [&document] (const std::string& curve, const char* name)
    {
        return document.getAttribute ("/godot/curve/" + curve + "/" + name).value_or (std::string {});
    };

    //  Three numbers are a position: the first, second and third push.
    const auto adm = osc ("/adm/obj/1/xyz", "f:0 f:0 f:0");
    const auto ys = document.createCurve (adm, 1);
    REQUIRE (ys.ok);
    CHECK (row (ys.id, "axis") == "ty");
    CHECK (row (ys.id, "invert") == "true");
    CHECK (row (ys.id, "speed") == "2");

    //  An address ending in X names its push, not inverted.
    const auto wfs = osc ("/wfs/input/1/positionX", "f:0");
    const auto xs = document.createCurve (wfs, 0);
    REQUIRE (xs.ok);
    CHECK (row (xs.id, "axis") == "tx");
    CHECK (row (xs.id, "invert") == "false");

    //  Anything else moves with nothing.
    const auto fader = osc ("/desk/fader", "f:0");
    const auto plain = document.createCurve (fader, 0);
    REQUIRE (plain.ok);
    CHECK (row (plain.id, "axis") == "none");
    CHECK (row (plain.id, "speed") == "1");
}

TEST_CASE ("spacemouse: the puck moves an armed curve with a movement during a pass, and nothing else")
{
    doc::ShowDocument document;
    const auto list = document.createList ("Sound").id;
    const auto cue = document.createCue (list, 0, "osc", "Move").id;
    REQUIRE (document.setAttribute ("/godot/cue/" + cue + "/address", "/wfs/input/1/positionX").ok);
    REQUIRE (document.setAttribute ("/godot/cue/" + cue + "/value", "f:0").ok);

    const auto curve = document.createCurve (cue, 0);
    REQUIRE (curve.ok);
    REQUIRE (document.setAttribute ("/godot/curve/" + curve.id + "/range", "-10 10").ok);

    cue::CurveTable curves;
    const std::array<double, 6> pushed { 1.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    const std::array<double, 6> still {};

    //  Not armed: not wanted, and nothing moved.
    CHECK_FALSE (cue::puckWanted (document, curves));

    curves.arm (cue);
    curves.setArmed (curve.id, true);
    CHECK (cue::puckWanted (document, curves));

    //  Armed but no pass: the puck does nothing.
    CHECK (cue::puckRides (document, nullptr, curves, pushed, 0.02).empty());

    curves.startPass ("RUN00001");
    curves.rideOf (curve.id).value = 9.99;

    //  A push steps it - held to the range - and a still puck writes nothing.
    const auto pairs = cue::puckRides (document, nullptr, curves, pushed, 0.02);
    REQUIRE (pairs.size() == 2u);
    CHECK (pairs[0].getString() == curve.id);
    CHECK (pairs[1].getFloat64() == doctest::Approx (10.0));
    CHECK (cue::puckRides (document, nullptr, curves, still, 0.02).empty());

    //  A curve with no movement is not the puck's.
    REQUIRE (document.setAttribute ("/godot/curve/" + curve.id + "/axis", "none").ok);
    CHECK_FALSE (cue::puckWanted (document, curves));
    CHECK (cue::puckRides (document, nullptr, curves, pushed, 0.02).empty());
}

TEST_CASE ("spacemouse: unwanted, the reader holds nothing; wanted with no puck, it searches")
{
    surface::SpaceMouse puck;
    REQUIRE (puck.start());
    CHECK (puck.status() == surface::spaceMouseStatus::off);
    CHECK_FALSE (puck.read().live);

    /*  Wanted: searching, or connected on a desk that has one plugged in, or
        `driver` where 3Dconnexion's holds it - never off. */
    puck.want (true);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 5 };

    while (puck.status() == surface::spaceMouseStatus::off && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds { 10 });

    CHECK (puck.status() != surface::spaceMouseStatus::off);

    puck.want (false);

    while (puck.status() != surface::spaceMouseStatus::off && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds { 10 });

    CHECK (puck.status() == surface::spaceMouseStatus::off);
    CHECK_FALSE (puck.read().live);
    puck.stop();
}
