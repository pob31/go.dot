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

#include <wfg/client/ui/Icons.h>

#include <cmath>
#include <map>
#include <utility>

namespace wfg::client::ui
{
    namespace
    {
        constexpr float grid = 24.0f;
        constexpr float weight = 1.75f;     // the line, in grid units
        constexpr float pi = 3.14159265358979f;

        /*  WHAT AN ICON IS MADE OF: lines to stroke and shapes to fill, both
            in grid units. Two paths and not one, because a stop is a filled
            square and a loop is a line, and one icon may want both (a fade
            is a curve with a dot at each end). */
        struct Shape
        {
            juce::Path lines;
            juce::Path solid;
        };

        void line (juce::Path& path, float x1, float y1, float x2, float y2)
        {
            path.startNewSubPath (x1, y1);
            path.lineTo (x2, y2);
        }

        void ring (juce::Path& path, float cx, float cy, float r)
        {
            path.addEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f);
        }

        /*  AN ARC by angles measured as JUCE measures them - clockwise from
            twelve o'clock, in degrees here because the icons were drawn in
            degrees. */
        void arc (juce::Path& path, float cx, float cy, float r, float fromDegrees, float toDegrees)
        {
            path.addCentredArc (cx, cy, r, r, 0.0f, fromDegrees * pi / 180.0f, toDegrees * pi / 180.0f, true);
        }

        /*  AN ARROWHEAD whose tip is at (x, y), pointing along (dx, dy). */
        void head (juce::Path& solid, float x, float y, float dx, float dy, float size = 3.2f)
        {
            const auto length = std::sqrt (dx * dx + dy * dy);

            if (! (length > 0.0f))
                return;

            const auto ux = dx / length, uy = dy / length;
            const auto bx = x - ux * size, by = y - uy * size;
            const auto half = size * 0.72f;

            solid.addTriangle (x, y, bx - uy * half, by + ux * half, bx + uy * half, by - ux * half);
        }

        /*  THE HEAD AT THE END OF AN ARC drawn clockwise, so it points the way
            the arc was travelling. */
        void headOnArc (juce::Path& solid, float cx, float cy, float r, float atDegrees)
        {
            const auto a = atDegrees * pi / 180.0f;
            const auto x = cx + r * std::sin (a), y = cy - r * std::cos (a);
            const auto dx = std::cos (a), dy = std::sin (a);

            head (solid, x + dx * 1.6f, y + dy * 1.6f, dx, dy);
        }

        Shape build (model::Icon icon)
        {
            Shape s;
            auto& l = s.lines;
            auto& f = s.solid;

            switch (icon)
            {
                case model::Icon::none:
                    break;

                //  WHAT A CUE IS --------------------------------------------------
                case model::Icon::memo:
                    l.addRoundedRectangle (5.0f, 3.0f, 14.0f, 18.0f, 2.0f);
                    line (l, 8.5f, 8.5f, 15.5f, 8.5f);
                    line (l, 8.5f, 12.0f, 15.5f, 12.0f);
                    line (l, 8.5f, 15.5f, 13.0f, 15.5f);
                    break;

                case model::Icon::media:
                    //  A sound file, as bars of a level: the shape every editor draws one as.
                    line (l, 4.0f, 10.0f, 4.0f, 14.0f);
                    line (l, 8.0f, 7.0f, 8.0f, 17.0f);
                    line (l, 12.0f, 4.0f, 12.0f, 20.0f);
                    line (l, 16.0f, 8.0f, 16.0f, 16.0f);
                    line (l, 20.0f, 10.5f, 20.0f, 13.5f);
                    break;

                case model::Icon::mic:
                    l.addRoundedRectangle (9.0f, 3.0f, 6.0f, 11.0f, 3.0f);
                    l.startNewSubPath (6.0f, 11.0f);
                    l.cubicTo (6.0f, 15.0f, 8.5f, 17.5f, 12.0f, 17.5f);
                    l.cubicTo (15.5f, 17.5f, 18.0f, 15.0f, 18.0f, 11.0f);
                    line (l, 12.0f, 17.5f, 12.0f, 21.0f);
                    line (l, 8.5f, 21.0f, 15.5f, 21.0f);
                    break;

                case model::Icon::video:
                    //  A picture in its frame: a hill and the sun over it (Phase 8a).
                    l.addRoundedRectangle (3.0f, 5.0f, 18.0f, 14.0f, 1.5f);
                    l.startNewSubPath (5.0f, 17.0f);
                    l.lineTo (10.0f, 11.0f);
                    l.lineTo (13.5f, 14.5f);
                    l.lineTo (15.5f, 12.5f);
                    l.lineTo (19.0f, 17.0f);
                    ring (f, 16.0f, 8.5f, 1.6f);
                    break;

                case model::Icon::fade:
                    //  A level going somewhere else, with a dot where it starts and where it lands.
                    l.startNewSubPath (4.0f, 6.0f);
                    l.cubicTo (10.0f, 6.0f, 13.0f, 18.0f, 20.0f, 18.0f);
                    ring (f, 4.0f, 6.0f, 2.0f);
                    ring (f, 20.0f, 18.0f, 2.0f);
                    break;

                case model::Icon::start:
                    l.startNewSubPath (7.0f, 5.0f);
                    l.lineTo (19.0f, 12.0f);
                    l.lineTo (7.0f, 19.0f);
                    l.closeSubPath();
                    break;

                case model::Icon::osc:
                    //  A message sent out over the network: a source and what spreads from it.
                    ring (f, 6.0f, 12.0f, 2.2f);
                    arc (l, 6.0f, 12.0f, 7.0f, 45.0f, 135.0f);
                    arc (l, 6.0f, 12.0f, 12.5f, 55.0f, 125.0f);
                    break;

                case model::Icon::midi:
                {
                    //  The five-pin socket, pins along its upper half and the key at its foot.
                    ring (l, 12.0f, 12.0f, 9.0f);

                    for (const auto degrees : { 180.0f, 225.0f, 270.0f, 315.0f, 360.0f })
                    {
                        const auto a = degrees * pi / 180.0f;
                        ring (f, 12.0f + 5.2f * std::cos (a), 12.5f + 5.2f * std::sin (a), 1.35f);
                    }

                    f.addRoundedRectangle (10.8f, 17.2f, 2.4f, 2.2f, 0.6f);
                    break;
                }

                case model::Icon::process:
                    //  A patch (namespace draft §51): two boxes and the line between
                    //  them, an outlet at the foot of one and an inlet on the other.
                    l.addRectangle (3.0f, 3.5f, 11.0f, 6.5f);
                    l.addRectangle (10.0f, 14.0f, 11.0f, 6.5f);
                    f.addRectangle (3.0f, 8.5f, 3.5f, 1.5f);
                    f.addRectangle (10.0f, 14.0f, 3.5f, 1.5f);
                    line (l, 4.75f, 10.0f, 11.75f, 14.0f);
                    break;

                case model::Icon::group:
                    l.addRoundedRectangle (3.0f, 8.0f, 13.0f, 13.0f, 2.0f);
                    l.startNewSubPath (8.0f, 8.0f);
                    l.lineTo (8.0f, 3.5f);
                    l.lineTo (21.0f, 3.5f);
                    l.lineTo (21.0f, 16.0f);
                    l.lineTo (16.0f, 16.0f);
                    break;

                case model::Icon::range:
                    l.startNewSubPath (7.0f, 4.0f);
                    l.lineTo (3.5f, 4.0f);
                    l.lineTo (3.5f, 20.0f);
                    l.lineTo (7.0f, 20.0f);
                    l.startNewSubPath (17.0f, 4.0f);
                    l.lineTo (20.5f, 4.0f);
                    l.lineTo (20.5f, 20.0f);
                    l.lineTo (17.0f, 20.0f);
                    f.addRoundedRectangle (7.5f, 10.5f, 9.0f, 3.0f, 1.0f);
                    break;

                case model::Icon::trigger:
                    l.startNewSubPath (13.5f, 2.5f);
                    l.lineTo (5.0f, 13.5f);
                    l.lineTo (11.0f, 13.5f);
                    l.lineTo (10.0f, 21.5f);
                    l.lineTo (19.0f, 10.0f);
                    l.lineTo (13.0f, 10.0f);
                    l.closeSubPath();
                    break;

                //  WHAT A TRANSPORT CUE DOES --------------------------------------
                case model::Icon::stop:
                    f.addRoundedRectangle (6.0f, 6.0f, 12.0f, 12.0f, 2.0f);
                    break;

                case model::Icon::stopFade:
                    l.startNewSubPath (2.5f, 6.0f);
                    l.cubicTo (6.0f, 6.0f, 7.0f, 15.5f, 10.5f, 15.5f);
                    f.addRoundedRectangle (12.5f, 7.5f, 9.5f, 9.5f, 1.8f);
                    break;

                case model::Icon::stopAfter:
                    //  Plays to the end of what it is in, then stops.
                    line (l, 2.5f, 12.0f, 8.5f, 12.0f);
                    line (l, 10.0f, 7.0f, 10.0f, 17.0f);
                    f.addRoundedRectangle (12.5f, 7.5f, 9.5f, 9.5f, 1.8f);
                    break;

                case model::Icon::advance:
                    f.addTriangle (5.0f, 6.0f, 15.0f, 12.0f, 5.0f, 18.0f);
                    f.addRoundedRectangle (16.5f, 6.0f, 3.0f, 12.0f, 1.0f);
                    break;

                case model::Icon::record:
                    ring (f, 12.0f, 12.0f, 6.5f);
                    break;

                case model::Icon::loop:
                    arc (l, 12.0f, 12.0f, 7.5f, 50.0f, 320.0f);
                    headOnArc (f, 12.0f, 12.0f, 7.5f, 320.0f);
                    break;

                case model::Icon::overdub:
                    //  A second layer over the first: a take recorded on top.
                    ring (l, 9.0f, 12.0f, 5.5f);
                    ring (f, 15.0f, 12.0f, 5.5f);
                    break;

                case model::Icon::clear:
                    ring (l, 12.0f, 12.0f, 8.0f);
                    line (l, 9.0f, 9.0f, 15.0f, 15.0f);
                    line (l, 15.0f, 9.0f, 9.0f, 15.0f);
                    break;

                case model::Icon::enable:
                    //  The disabled ring, its slash turned into a tick: runs again, for this run.
                    ring (l, 12.0f, 12.0f, 8.5f);
                    line (l, 8.0f, 12.5f, 11.0f, 15.5f);
                    line (l, 11.0f, 15.5f, 16.5f, 9.0f);
                    break;

                case model::Icon::jump:
                    //  Over the rows in between, from here to there: where standby lands.
                    ring (f, 4.5f, 15.0f, 1.9f);
                    arc (l, 12.0f, 15.0f, 7.5f, -90.0f, 90.0f);
                    headOnArc (f, 12.0f, 15.0f, 7.5f, 90.0f);
                    break;

                //  HOW A GROUP RUNS ------------------------------------------------
                case model::Icon::sequence:
                    //  One after another: three blocks, one leading to the next.
                    l.addRoundedRectangle (1.5f, 8.0f, 5.5f, 8.0f, 1.3f);
                    l.addRoundedRectangle (9.25f, 8.0f, 5.5f, 8.0f, 1.3f);
                    f.addRoundedRectangle (17.0f, 8.0f, 5.5f, 8.0f, 1.3f);
                    line (l, 7.0f, 12.0f, 9.25f, 12.0f);
                    line (l, 14.75f, 12.0f, 17.0f, 12.0f);
                    break;

                case model::Icon::timeline:
                    //  Members laid out in time, overlapping: each starts at its own moment.
                    f.addRoundedRectangle (3.0f, 5.0f, 9.0f, 3.2f, 1.6f);
                    f.addRoundedRectangle (8.0f, 10.4f, 11.0f, 3.2f, 1.6f);
                    f.addRoundedRectangle (5.0f, 15.8f, 16.0f, 3.2f, 1.6f);
                    break;

                case model::Icon::sampler:
                    //  Pads waiting for a hand, one of them pressed.
                    l.addRoundedRectangle (4.0f, 4.0f, 7.0f, 7.0f, 1.5f);
                    l.addRoundedRectangle (13.0f, 4.0f, 7.0f, 7.0f, 1.5f);
                    l.addRoundedRectangle (4.0f, 13.0f, 7.0f, 7.0f, 1.5f);
                    f.addRoundedRectangle (13.0f, 13.0f, 7.0f, 7.0f, 1.5f);
                    break;

                //  WHAT A ROW SAYS ABOUT ITSELF ------------------------------------
                case model::Icon::forever:
                    l.startNewSubPath (12.0f, 12.0f);
                    l.cubicTo (9.5f, 7.5f, 3.0f, 7.5f, 3.0f, 12.0f);
                    l.cubicTo (3.0f, 16.5f, 9.5f, 16.5f, 12.0f, 12.0f);
                    l.cubicTo (14.5f, 7.5f, 21.0f, 7.5f, 21.0f, 12.0f);
                    l.cubicTo (21.0f, 16.5f, 14.5f, 16.5f, 12.0f, 12.0f);
                    break;

                case model::Icon::shuffle:
                    l.startNewSubPath (3.0f, 7.0f);
                    l.lineTo (6.5f, 7.0f);
                    l.cubicTo (11.5f, 7.0f, 12.5f, 17.0f, 17.5f, 17.0f);
                    l.lineTo (18.5f, 17.0f);
                    l.startNewSubPath (3.0f, 17.0f);
                    l.lineTo (6.5f, 17.0f);
                    l.cubicTo (11.5f, 17.0f, 12.5f, 7.0f, 17.5f, 7.0f);
                    l.lineTo (18.5f, 7.0f);
                    head (f, 22.0f, 7.0f, 1.0f, 0.0f);
                    head (f, 22.0f, 17.0f, 1.0f, 0.0f);
                    break;

                case model::Icon::follow:
                    l.startNewSubPath (5.0f, 6.0f);
                    l.lineTo (11.0f, 12.0f);
                    l.lineTo (5.0f, 18.0f);
                    l.startNewSubPath (12.5f, 6.0f);
                    l.lineTo (18.5f, 12.0f);
                    l.lineTo (12.5f, 18.0f);
                    break;

                case model::Icon::subset:
                    //  Some of them: two taken, one left.
                    ring (f, 5.0f, 12.0f, 2.6f);
                    ring (f, 12.0f, 12.0f, 2.6f);
                    ring (l, 19.0f, 12.0f, 2.4f);
                    break;

                case model::Icon::preset:
                    //  A pin: this cue is fixed in place by a header further out.
                    ring (l, 12.0f, 8.5f, 4.5f);
                    line (l, 12.0f, 13.0f, 12.0f, 21.0f);
                    ring (f, 12.0f, 8.5f, 1.5f);
                    break;

                case model::Icon::disabled:
                    ring (l, 12.0f, 12.0f, 8.5f);
                    line (l, 6.0f, 6.0f, 18.0f, 18.0f);
                    break;

                case model::Icon::speed:
                    arc (l, 12.0f, 14.5f, 8.5f, -90.0f, 90.0f);
                    line (l, 12.0f, 14.5f, 16.0f, 9.5f);
                    ring (f, 12.0f, 14.5f, 1.9f);
                    line (l, 3.5f, 14.5f, 3.5f, 18.0f);
                    line (l, 20.5f, 14.5f, 20.5f, 18.0f);
                    break;

                case model::Icon::stretch:
                    //  Longer or shorter, the pitch kept: the length moves and nothing else.
                    line (l, 3.0f, 6.0f, 3.0f, 18.0f);
                    line (l, 21.0f, 6.0f, 21.0f, 18.0f);
                    line (l, 8.5f, 12.0f, 15.5f, 12.0f);
                    head (f, 6.0f, 12.0f, -1.0f, 0.0f);
                    head (f, 18.0f, 12.0f, 1.0f, 0.0f);
                    break;

                case model::Icon::dca:
                    //  A fader: what a DCA is on a desk.
                    line (l, 12.0f, 3.0f, 12.0f, 21.0f);
                    line (l, 7.0f, 6.0f, 9.0f, 6.0f);
                    line (l, 15.0f, 6.0f, 17.0f, 6.0f);
                    line (l, 7.0f, 18.0f, 9.0f, 18.0f);
                    line (l, 15.0f, 18.0f, 17.0f, 18.0f);
                    f.addRoundedRectangle (6.5f, 9.5f, 11.0f, 5.0f, 1.3f);
                    break;

                case model::Icon::lane:
                    l.startNewSubPath (3.0f, 17.0f);
                    l.lineTo (9.0f, 8.0f);
                    l.lineTo (15.0f, 13.0f);
                    l.lineTo (21.0f, 6.0f);

                    for (const auto& [x, y] : { std::pair { 3.0f, 17.0f }, std::pair { 9.0f, 8.0f },
                                                std::pair { 15.0f, 13.0f }, std::pair { 21.0f, 6.0f } })
                        ring (f, x, y, 1.9f);
                    break;

                //  THE PANELS AT THE FOOT ------------------------------------------
                case model::Icon::waveform:
                    l.startNewSubPath (1.5f, 12.0f);
                    l.lineTo (4.5f, 12.0f);
                    l.lineTo (6.0f, 8.0f);
                    l.lineTo (8.0f, 16.0f);
                    l.lineTo (10.0f, 4.5f);
                    l.lineTo (12.0f, 19.5f);
                    l.lineTo (14.0f, 7.0f);
                    l.lineTo (16.0f, 15.5f);
                    l.lineTo (18.0f, 10.0f);
                    l.lineTo (19.5f, 12.0f);
                    l.lineTo (22.5f, 12.0f);
                    break;

                case model::Icon::sends:
                    line (l, 6.0f, 3.5f, 6.0f, 20.5f);
                    line (l, 12.0f, 3.5f, 12.0f, 20.5f);
                    line (l, 18.0f, 3.5f, 18.0f, 20.5f);
                    f.addRoundedRectangle (3.0f, 13.0f, 6.0f, 3.5f, 1.0f);
                    f.addRoundedRectangle (9.0f, 6.5f, 6.0f, 3.5f, 1.0f);
                    f.addRoundedRectangle (15.0f, 10.0f, 6.0f, 3.5f, 1.0f);
                    break;

                case model::Icon::curve:
                    l.startNewSubPath (3.0f, 3.0f);
                    l.lineTo (3.0f, 21.0f);
                    l.lineTo (21.0f, 21.0f);
                    l.startNewSubPath (3.0f, 6.0f);
                    l.cubicTo (12.0f, 6.0f, 11.0f, 17.0f, 21.0f, 17.0f);
                    ring (f, 12.0f, 11.6f, 1.8f);
                    break;

                case model::Icon::take:
                    //  A tape's two reels and the tape between them: something recorded.
                    ring (l, 7.0f, 10.5f, 4.2f);
                    ring (l, 17.0f, 10.5f, 4.2f);
                    line (l, 7.0f, 14.7f, 17.0f, 14.7f);
                    ring (f, 7.0f, 10.5f, 1.3f);
                    ring (f, 17.0f, 10.5f, 1.3f);
                    line (l, 3.0f, 19.5f, 21.0f, 19.5f);
                    break;

                //  THE INSPECTOR'S DRAWERS -----------------------------------------
                case model::Icon::identity:
                    l.startNewSubPath (2.5f, 12.0f);
                    l.lineTo (8.5f, 5.5f);
                    l.lineTo (21.0f, 5.5f);
                    l.lineTo (21.0f, 18.5f);
                    l.lineTo (8.5f, 18.5f);
                    l.closeSubPath();
                    ring (f, 9.0f, 12.0f, 1.7f);
                    break;

                case model::Icon::clock:
                    ring (l, 12.0f, 12.0f, 9.0f);
                    l.startNewSubPath (12.0f, 6.5f);
                    l.lineTo (12.0f, 12.0f);
                    l.lineTo (16.0f, 14.5f);
                    break;

                case model::Icon::list:
                    ring (f, 5.0f, 7.0f, 1.5f);
                    ring (f, 5.0f, 12.0f, 1.5f);
                    ring (f, 5.0f, 17.0f, 1.5f);
                    line (l, 9.0f, 7.0f, 20.5f, 7.0f);
                    line (l, 9.0f, 12.0f, 20.5f, 12.0f);
                    line (l, 9.0f, 17.0f, 20.5f, 17.0f);
                    break;

                case model::Icon::info:
                    ring (l, 12.0f, 12.0f, 9.0f);
                    line (l, 12.0f, 11.0f, 12.0f, 17.0f);
                    ring (f, 12.0f, 7.5f, 1.4f);
                    break;

                case model::Icon::sound:
                    l.startNewSubPath (3.0f, 9.5f);
                    l.lineTo (7.0f, 9.5f);
                    l.lineTo (12.0f, 5.0f);
                    l.lineTo (12.0f, 19.0f);
                    l.lineTo (7.0f, 14.5f);
                    l.lineTo (3.0f, 14.5f);
                    l.closeSubPath();
                    arc (l, 12.0f, 12.0f, 4.5f, 50.0f, 130.0f);
                    arc (l, 12.0f, 12.0f, 8.5f, 45.0f, 135.0f);
                    break;

                //  THE FOOT'S HEAD -------------------------------------------------
                case model::Icon::copy:
                    //  Two sheets, one over the other: the same thing twice.
                    l.addRoundedRectangle (8.5f, 3.5f, 11.0f, 13.5f, 1.6f);
                    l.startNewSubPath (5.5f, 7.0f);
                    l.lineTo (4.5f, 7.0f);
                    l.lineTo (4.5f, 20.5f);
                    l.lineTo (15.5f, 20.5f);
                    l.lineTo (15.5f, 19.5f);
                    break;

                case model::Icon::paste:
                    //  A clipboard, its clip at the top and a sheet on it.
                    l.addRoundedRectangle (4.5f, 5.0f, 15.0f, 16.5f, 1.6f);
                    f.addRoundedRectangle (8.5f, 2.5f, 7.0f, 4.0f, 1.2f);
                    line (l, 8.5f, 11.0f, 15.5f, 11.0f);
                    line (l, 8.5f, 15.0f, 13.5f, 15.0f);
                    break;

                /*  HOW READY A ROW'S CUE IS (namespace draft §48, AAS): one ring
                    told apart by what is inside it, so the four read without
                    their colours (PRD §4.8). */
                case model::Icon::loading:
                    //  A ring not yet closed: still being read.
                    arc (l, 12.0f, 12.0f, 7.0f, 40.0f, 320.0f);
                    break;

                case model::Icon::ready:
                    //  A ring and its centre: held, GO shows it at once.
                    ring (l, 12.0f, 12.0f, 7.0f);
                    ring (f, 12.0f, 12.0f, 3.5f);
                    break;

                case model::Icon::partly:
                    //  A ring half full: some of it got ready, the rest at GO.
                    ring (l, 12.0f, 12.0f, 7.0f);
                    f.startNewSubPath (12.0f, 5.0f);
                    f.addCentredArc (12.0f, 12.0f, 7.0f, 7.0f, 0.0f, 0.0f, pi, false);
                    f.closeSubPath();
                    break;

                case model::Icon::missing:
                    //  A ring struck through: nothing to read.
                    ring (l, 12.0f, 12.0f, 7.0f);
                    line (l, 7.0f, 17.0f, 17.0f, 7.0f);
                    break;
            }

            return s;
        }

        /*  BUILT ONCE PER ICON AND KEPT: every icon is drawn on the message
            thread, so a plain map needs no guard, and a list of five hundred
            rows paints the same thirty shapes rather than building them again
            per row per pass. */
        const Shape& shapeOf (model::Icon icon)
        {
            static std::map<model::Icon, Shape> built;

            auto found = built.find (icon);

            if (found == built.end())
                found = built.emplace (icon, build (icon)).first;

            return found->second;
        }
    }

    namespace icons
    {
        void draw (juce::Graphics& g, model::Icon icon, juce::Rectangle<float> area, juce::Colour colour)
        {
            if (icon == model::Icon::none || area.isEmpty())
                return;

            const auto side = juce::jmin (area.getWidth(), area.getHeight());
            const auto square = area.withSizeKeepingCentre (side, side);
            const auto scale = side / grid;

            const auto place = juce::AffineTransform::scale (scale).translated (square.getX(), square.getY());
            const auto& shape = shapeOf (icon);

            g.setColour (colour);

            if (! shape.solid.isEmpty())
                g.fillPath (shape.solid, place);

            if (! shape.lines.isEmpty())
            {
                /*  NEVER THINNER THAN A PIXEL: under that a line is drawn as a
                    grey smear, and a row of them shimmers as the list scrolls. */
                const auto width = juce::jmax (1.0f, weight * scale);

                auto stroked = shape.lines;
                stroked.applyTransform (place);

                g.strokePath (stroked, juce::PathStrokeType (width, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            }
        }

        int markWidth (const model::Mark& mark, int height, const juce::Font& font)
        {
            auto width = height;

            if (! mark.text.empty())
                width += juce::roundToInt (static_cast<float> (height) * 0.2f)
                           + juce::GlyphArrangement::getStringWidthInt (font, juce::String::fromUTF8 (mark.text.c_str()));

            return width;
        }

        int drawMark (juce::Graphics& g, const model::Mark& mark, juce::Rectangle<int> area,
                      juce::Colour colour, const juce::Font& font)
        {
            const auto height = area.getHeight();
            const auto width = juce::jmin (area.getWidth(), markWidth (mark, height, font));

            auto space = area.withWidth (width);

            draw (g, mark.icon, space.removeFromLeft (height).toFloat(), colour);

            if (! mark.text.empty())
            {
                space.removeFromLeft (juce::roundToInt (static_cast<float> (height) * 0.2f));
                g.setColour (colour);
                g.setFont (font);
                g.drawText (juce::String::fromUTF8 (mark.text.c_str()), space,
                            juce::Justification::centredLeft, true);
            }

            return width;
        }
    }

    IconButton::IconButton (model::Icon iconToUse, const juce::String& word)
        : juce::Button (word), icon (iconToUse)
    {
        setWantsKeyboardFocus (false);
    }

    void IconButton::setIcon (model::Icon iconToUse)
    {
        if (icon != iconToUse)
        {
            icon = iconToUse;
            repaint();
        }
    }

    void IconButton::setColours (juce::Colour inkToUse, juce::Colour groundToUse, juce::Colour accentToUse)
    {
        ink = inkToUse;
        ground = groundToUse;
        accent = accentToUse;
        repaint();
    }

    void IconButton::setTextHeight (float height)
    {
        textHeight = height;
        repaint();
    }

    void IconButton::setWordShown (bool shown)
    {
        if (wordShown != shown)
        {
            wordShown = shown;
            repaint();
        }
    }

    int IconButton::idealWidth (int height) const
    {
        const auto font = juce::Font (juce::FontOptions {}.withHeight (textHeight));
        const auto word = getButtonText();

        //  A word standing for its picture: the word, and air either side.
        if (icon == model::Icon::none)
            return juce::GlyphArrangement::getStringWidthInt (font.boldened(), word) + height;

        auto width = height;   // the icon's square, which carries its own air

        if (word.isNotEmpty())
            width += juce::GlyphArrangement::getStringWidthInt (font, word) + height / 3;

        return width;
    }

    void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
    {
        auto area = getLocalBounds().toFloat().reduced (0.5f);
        const auto on = getToggleState();
        const auto corner = 4.0f;

        /*  LIT THREE WAYS WHEN ITS PANEL IS OPEN: the accent washes the button,
            a bar runs along its foot, and the word goes from the dim ink to the
            full one. Unlit, it is the raised panel every other button is. */
        g.setColour (on ? accent.withAlpha (0.28f) : ground);
        g.fillRoundedRectangle (area, corner);

        if (over || down)
        {
            g.setColour (ink.withAlpha (down ? 0.16f : 0.08f));
            g.fillRoundedRectangle (area, corner);
        }

        g.setColour (on ? accent : ink.withAlpha (0.22f));
        g.drawRoundedRectangle (area, corner, 1.0f);

        if (on)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (area.removeFromBottom (3.0f).reduced (corner, 0.0f), 1.5f);
            area = getLocalBounds().toFloat().reduced (0.5f);
        }

        const auto alpha = isEnabled() ? 1.0f : 0.4f;

        /*  A WORD THAT IS ITS OWN PICTURE - "EQ", "FX" - is drawn where the
            picture would be and as the picture would be, in the accent and a
            little heavier than a word, and is never dropped for want of room:
            it is no wider than a picture. */
        if (icon == model::Icon::none)
        {
            g.setColour ((on ? accent.brighter (0.25f) : accent).withMultipliedAlpha (alpha));
            g.setFont (juce::Font (juce::FontOptions {}.withHeight (textHeight)).boldened());
            g.drawText (getButtonText(), area, juce::Justification::centred, true);
            return;
        }

        const auto height = static_cast<float> (getHeight());
        const auto word = wordShown ? getButtonText() : juce::String();

        auto content = area.reduced (height * 0.18f, 0.0f);

        /*  THE ICON AND THE WORD CENTRED AS A PAIR, so a narrow button and a
            wide one read the same way. */
        const auto font = juce::Font (juce::FontOptions {}.withHeight (textHeight));
        const auto iconSide = height * 0.62f;
        const auto gap = word.isEmpty() ? 0.0f : height * 0.22f;
        const auto wordWidth = word.isEmpty() ? 0.0f
                                              : static_cast<float> (juce::GlyphArrangement::getStringWidthInt (font, word));
        const auto total = juce::jmin (content.getWidth(), iconSide + gap + wordWidth);

        auto pair = content.withSizeKeepingCentre (total, content.getHeight());

        icons::draw (g, icon, pair.removeFromLeft (iconSide), (on ? accent.brighter (0.25f) : accent).withMultipliedAlpha (alpha));

        if (word.isNotEmpty())
        {
            pair.removeFromLeft (gap);
            g.setColour ((on ? ink : ink.withAlpha (0.78f)).withMultipliedAlpha (alpha));
            g.setFont (font);
            g.drawText (word, pair, juce::Justification::centredLeft, true);
        }
    }
}
