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
    Several fingers on one surface, as rules (namespace draft §125, MT.1): the
    pointer's key, the turns the mouse and the fingers take, the sweep of a
    lift that never came, and a finger's fader gearing. Pure: no window.
*/

#include <wfg/client/model/FingerDrag.h>
#include <wfg/client/model/Pointers.h>

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <cmath>
#include <vector>

using namespace wfg::client::model;

TEST_CASE ("client: a pointer is its kind and its number, so the mouse is never the first finger")
{
    CHECK (pointerKey (PointerKind::mouse, 0) == mousePointer);
    CHECK (pointerKey (PointerKind::touch, 0) == firstFinger);
    CHECK (pointerKey (PointerKind::touch, 0) != pointerKey (PointerKind::mouse, 0));
    CHECK (pointerKey (PointerKind::touch, 3) == firstFinger + 3);
    CHECK (pointerKey (PointerKind::pen, 0) == 2000);

    CHECK (kindOfPointer (pointerKey (PointerKind::mouse, 0)) == PointerKind::mouse);
    CHECK (kindOfPointer (pointerKey (PointerKind::touch, 9)) == PointerKind::touch);
    CHECK (kindOfPointer (pointerKey (PointerKind::pen, 1)) == PointerKind::pen);

    CHECK (isFinger (firstFinger + 1));
    CHECK_FALSE (isFinger (mousePointer));
    CHECK_FALSE (isFinger (pointerKey (PointerKind::pen, 0)));

    //  An index past the thousand stays its kind's.
    CHECK (kindOfPointer (pointerKey (PointerKind::touch, 5000)) == PointerKind::touch);
}

TEST_CASE ("client: fingers act together, and the mouse and the fingers take turns")
{
    PointerTurns turns;

    SUBCASE ("two fingers both act")
    {
        CHECK (turns.down (firstFinger));
        CHECK (turns.down (firstFinger + 1));
        CHECK (turns.owns (firstFinger));
        CHECK (turns.owns (firstFinger + 1));
        CHECK (turns.held().size() == 2);
    }

    SUBCASE ("the mouse does nothing while a finger is down")
    {
        CHECK (turns.down (firstFinger));
        CHECK_FALSE (turns.down (mousePointer));
        CHECK_FALSE (turns.owns (mousePointer));

        //  The finger lifts; the mouse, refused at its start, stays refused until it lifts.
        CHECK (turns.up (firstFinger));
        CHECK_FALSE (turns.down (mousePointer));
        CHECK_FALSE (turns.up (mousePointer));

        //  A fresh press acts.
        CHECK (turns.down (mousePointer));
    }

    SUBCASE ("a landing finger is ignored while the mouse is down")
    {
        CHECK (turns.down (mousePointer));
        CHECK_FALSE (turns.down (firstFinger));
        CHECK (turns.up (mousePointer));

        //  Still the same press: ignored to its end, and its lift lets go of nothing.
        CHECK_FALSE (turns.owns (firstFinger));
        CHECK_FALSE (turns.up (firstFinger));
        CHECK (turns.down (firstFinger));
    }

    SUBCASE ("a pen takes the mouse's turn")
    {
        CHECK (turns.down (pointerKey (PointerKind::pen, 0)));
        CHECK_FALSE (turns.down (firstFinger));
        CHECK (turns.mouseDown());
        CHECK_FALSE (turns.fingerDown());
    }

    SUBCASE ("a repeated press answers as before")
    {
        CHECK (turns.down (firstFinger));
        CHECK (turns.down (firstFinger));
        CHECK (turns.held().size() == 1);
    }
}

TEST_CASE ("client: a lift that never came is swept at the next lone press, and what it held is named")
{
    PointerTurns turns;

    CHECK (turns.down (firstFinger));
    CHECK (turns.down (firstFinger + 1));

    //  Two pointers really down with the new one: nothing is stale.
    CHECK (turns.sweep (3).empty());
    CHECK (turns.held().size() == 2);

    //  The new press is the only one JUCE knows: the two left are stale and named.
    const auto stale = turns.sweep (1);
    CHECK (stale == std::vector<int> { firstFinger, firstFinger + 1 });
    CHECK (turns.held().empty());

    //  A refused press is forgotten too, and not named: it held nothing.
    CHECK (turns.down (mousePointer));
    CHECK_FALSE (turns.down (firstFinger + 4));
    CHECK (turns.sweep (0) == std::vector<int> { mousePointer });
    CHECK (turns.down (firstFinger + 4));
}

TEST_CASE ("client: a finger on a fader moves from where it landed, finer the further off the strip")
{
    CHECK (outsideBand (5.0, 0.0, 10.0) == doctest::Approx (0.0));
    CHECK (outsideBand (-3.0, 0.0, 10.0) == doctest::Approx (3.0));
    CHECK (outsideBand (14.0, 0.0, 10.0) == doctest::Approx (4.0));

    SUBCASE ("on the strip, a pixel is a pixel of the throw, and the grab moves nothing")
    {
        FingerDrag drag (0.5, 100.0);
        CHECK (drag.place() == doctest::Approx (0.5));
        CHECK (drag.moveTo (100.0, 0.0, 200.0) == doctest::Approx (0.5));
        CHECK (drag.moveTo (150.0, 0.0, 200.0) == doctest::Approx (0.75));
        CHECK (drag.rate() == doctest::Approx (1.0));
    }

    SUBCASE ("forty-eight pixels off halves it, ninety-six quarters it - the page's numbers")
    {
        FingerDrag drag (0.5, 0.0);
        CHECK (drag.moveTo (40.0, fingerUnitPx, 200.0) == doctest::Approx (0.6));
        CHECK (drag.rate() == doctest::Approx (0.5));
        CHECK (drag.moveTo (80.0, 2.0 * fingerUnitPx, 200.0) == doctest::Approx (0.65));
        CHECK (drag.rate() == doctest::Approx (0.25));
    }

    SUBCASE ("never finer than a 256th, however far off")
    {
        FingerDrag drag (0.0, 0.0);
        drag.moveTo (256.0, 10000.0, 256.0);
        CHECK (drag.rate() == doctest::Approx (1.0 / 256.0));
        CHECK (drag.place() == doctest::Approx (1.0 / 256.0));
    }

    SUBCASE ("the rate changes mid-gesture without a jump")
    {
        FingerDrag drag (0.5, 0.0);
        drag.moveTo (20.0, 0.0, 200.0);
        const auto before = drag.place();

        //  The finger goes far off the strip without moving along it: the place stays.
        CHECK (drag.moveTo (20.0, 300.0, 200.0) == doctest::Approx (before));
    }

    SUBCASE ("held between the ends")
    {
        FingerDrag drag (0.9, 0.0);
        CHECK (drag.moveTo (500.0, 0.0, 200.0) == doctest::Approx (1.0));
        CHECK (drag.moveTo (-1000.0, 0.0, 200.0) == doctest::Approx (0.0));
    }

    SUBCASE ("a throw with no length moves nothing")
    {
        FingerDrag drag (0.3, 0.0);
        CHECK (drag.moveTo (50.0, 0.0, 0.0) == doctest::Approx (0.3));
    }
}
