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

#pragma once

/*
    A group's members laid out in time, so they can be arranged by dragging.

    WHAT A BAR'S POSITION IS MADE OF, and it is one number: in a TIMELINE group
    (PRD §3.6) every member is scheduled at the group's entry and its `preWait`
    is its OFFSET from that moment. So a bar's left edge IS its pre-wait, moving
    it writes one `node.set`, and nothing else on the timeline moves - which is
    the whole reason raising a group's own pre-wait defers a scene without
    disturbing relative timing somebody spent an afternoon getting right.

    A SEQUENCE IS A CONSEQUENCE AND NOT A LAYOUT. There a member starts when the
    one before it has finished, so a bar's position is arithmetic over everything
    above it and dragging one would silently move all the rest. The bars are
    still drawn - seeing the shape is useful - and they do not move under the
    hand. `draggable` says which a reading is.

    A LENGTH IS NOT ALWAYS KNOWN, and pretending otherwise is the one thing this
    may not do. A media cue's `duration` is published; a fade's and a stop's are
    what somebody decided; a memo, an osc or a midi cue is an instant. An unknown
    length is drawn as a start with no end rather than as a bar of no width,
    because "I do not know how long this is" and "this takes no time" are
    different facts and a designer reads the difference.

    A NESTED GROUP IS RESOLVED BY WALKING DOWN INTO IT (author, 2026-09-22: *"can
    you resolve nested groups?"*). No group's length is published - the engine
    has it in `ShowWalk::groupLength` and it stays inside the static walk - but
    everything the sum is MADE of is published a level at a time, so the client
    does the same arithmetic itself, under the same guards: a header or a footer,
    a manual sequence, a `loops` other than one, a `play` N of M, a shuffle, or
    any member whose own length is unknown, and the answer is unknown.

    ONE DIFFERENCE FROM THE ENGINE'S VERSION, deliberate and worth stating.
    `groupLength` answers `(the group's own preWait + furthest) * rounds`; this
    answers `furthest * rounds`, leaving the pre-wait out. A bar's left edge IS
    its pre-wait here, so including it in the length as well would draw every
    nested group that much too long. The engine's own `place` adds a member's
    pre-wait to its start AND then takes the length that already contains it,
    which looks like the same number being counted twice - raised with the
    author, 2026-09-22, and not changed here: it moves the solver's answers and
    the slot analysis's seconds, which is not a client's decision to take.

    SNAPPING IS ASKED FOR, NOT ASSUMED (author, 2026-09-22: *"you can snap starts
    together, start and end, end and end with drag+shift modifier"*). A plain
    drag is free to the pixel; shift held offers the three relations that mean
    something between two bars, and says in words which one it took.
*/

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /** One member of the group, as a rectangle in time. */
    struct Bar
    {
        std::string id;
        std::string name;
        std::string kind;       ///< media, fade, group, memo...
        std::string colour;     ///< "#RRGGBB" as the cue declares it, or empty

        /** Seconds from the group's entry to this member's start. */
        double at = 0.0;

        /** How long it sounds. Meaningless unless `lengthKnown`. */
        double length = 0.0;
        bool lengthKnown = false;

        /** What the document holds, which in a timeline group is `at`. */
        double preWait = 0.0;
        double postWait = 0.0;

        bool isGroup = false;

        double ends() const noexcept { return at + (lengthKnown ? length : 0.0); }
    };

    struct TimelineReading
    {
        std::string groupId;
        std::string groupName;

        /** `timeline` or `sequence`, as the group says. */
        std::string mode;

        /** Whether a bar may be moved by dragging it - only in a timeline. */
        bool draggable = false;

        std::vector<Bar> bars;

        /** The furthest second anything reaches, for the axis. Never nought. */
        double span = 0.0;

        /*  The group this one is a member of, when it is a member of one.
            What the panel climbs to when somebody asks to go back up. */
        std::string parent;

        /** Empty when there is something to draw; a sentence when there is not. */
        std::string notice;
    };

    TimelineReading readTimeline (const tree::TreeSnapshot&, const std::string& groupId);

    /*  WHERE A DRAGGED BAR WOULD LAND, as a pre-wait: never before the group's
        own entry, because a member cannot start before the group it is in. */
    double preWaitFor (double wantedStart);

    /** One place a dragged edge could settle, and what it would be lining up with. */
    struct SnapTarget
    {
        double seconds = 0.0;

        /** The bar it belongs to, for the sentence. Empty for the group's entry. */
        std::string name;

        /** "start" or "end" - which edge of that bar. */
        std::string edge;
    };

    /*  EVERY INSTANT WORTH LINING UP WITH: the group's own entry, and both
        edges of every OTHER bar. The dragged bar is left out - a bar cannot
        snap to itself - and so is any bar whose length is unknown, at its end
        only: an end nobody knows is not a place.
    */
    std::vector<SnapTarget> snapTargets (const std::vector<Bar>& bars,
                                         const std::string& draggedId);

    /*  WHAT A DRAG SETTLES ON when shift is held: the dragged bar's START and
        its END are both offered to every target, so all three of the author's
        relations fall out of one comparison - start to start, start to end,
        end to end (and end to start, which is the same gesture read the other
        way and costs nothing to allow).

        `tolerance` is in SECONDS, so the caller turns a pixel distance into one
        through its own view: a snap that felt right at one zoom and not at
        another would be a snap nobody trusts. `nullopt` when nothing is near.
    */
    struct Snapped
    {
        double at = 0.0;        ///< the pre-wait to write
        std::string said;       ///< what it lined up with, in words
    };

    std::optional<Snapped> snapTo (double wantedStart, double length, bool lengthKnown,
                                   const std::vector<SnapTarget>& targets,
                                   double tolerance);
}
