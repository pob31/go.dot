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
    WHERE EVERY CUE SITS, AND WHEN - ONE ANSWER, ASKED BY EVERYTHING.

    Two things in this engine reason about when a cue is live by reading the
    document rather than by running it: PRD §3.9c's edit-time slot analysis,
    which asks whether two claims can overlap, and §3.13's state solver, which
    asks what is playing at a given position. They are the same question, and a
    second implementation of it would be a second thing to keep correct - with
    the failure arriving as a slot warning that disagrees with a load-to-time
    about the same show.

    So this is the walk, and both of them ask it.

    WHAT IT ANSWERS. Every cue in every list gets a ROW - its position in
    document order, header then members then footer - and, where the document
    makes it knowable, a pair of SECONDS from the entry of the chain it is in.
    Groups get an EXTENT, the rows they span, and whether they end on their own.

    THE TWO COORDINATES, AND WHY THERE ARE TWO. A manual cue list has no time in
    it: between one GO and the next there is a person, and a person is not a
    duration. So across a manual boundary the honest coordinate is the row.
    Inside a timeline group or an automatic sequence there is no person, so
    offsets and durations are exact and the walk computes seconds.

    A CHAIN IS TIMED ONLY WHEN NOTHING IN THE WAY OF THE ARITHMETIC IS UNKNOWN:
    a timeline group or an automatic sequence, one round, every member played,
    in the written order, and no header - a header runs before the members and
    blocks, so its length would shift every member after it, and a header is
    exactly where an operator-paced cue turns up. A footer is not in the way: it
    runs after every member.

    AND A LENGTH THAT IS NOT KNOWN MAKES ITS CUE UNTIMED rather than
    instantaneous. An interval with one end is not an interval: compared as
    empty it would prove that nothing ever overlaps it, which is the one thing
    neither caller is allowed to conclude.

    THREADING: none of its own. Built on the tick thread by whoever owns the
    document, or on any thread at all by `wfg validate`, which has no engine.
*/

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/osc/OscValue.h>

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    inline const juce::Identifier idProperty { "id" };

    inline bool isCueElement (const juce::String& element) noexcept
    {
        return element == "Cue"  || element == "Group" || element == "Media"
            || element == "Fade" || element == "Stop"  || element == "Osc"
            || element == "Midi";
    }

    //======================================================================
    /*  A value out of the document, or the schema's default where the
        attribute is absent.

        NOT `ShowDocument::getAttribute`, although that gives the same
        answer: it addresses by identifier, and finding an identifier is a
        depth-first walk of the whole show. This analysis is already
        standing on the node, and it reads a dozen attributes per cue over
        hundreds of cues - which through the address door would be a
        quadratic walk of the show every time somebody moved a cue.

        The defaults still come from the parameter table, so a row that
        changes its default there changes it here, and there is no second
        list of what a group's `mode` is when nobody wrote one down. */
    class Reader
    {
    public:
        Reader()
        {
            for (const auto* owner : { "cue", "group", "media", "range", "fade",
                                       "stop", "feed", "insert" })
                for (const auto* row : doc::Schema::rowsForOwner (owner))
                    defaults[std::string (owner) + "/" + std::string (row->name)]
                        = std::string (row->defaultText);
        }

        std::string text (const juce::ValueTree& node,
                          std::string_view owner, const char* name) const
        {
            const juce::Identifier property { name };

            if (node.hasProperty (property))
                return node[property].toString().toStdString();

            const auto found = defaults.find (std::string (owner) + "/" + name);
            return found != defaults.end() ? found->second : std::string {};
        }

        double number (const juce::ValueTree& node,
                       std::string_view owner, const char* name) const
        {
            const juce::Identifier property { name };

            /*  Straight off the `var` when it is there, because the write
                choke point guarantees the stored type - and a round trip
                through text would be a locale question this project has
                two ctest entries about. */
            if (node.hasProperty (property))
                return static_cast<double> (node[property]);

            return osc::parseDouble (text (node, owner, name)).value_or (0.0);
        }

        int integer (const juce::ValueTree& node,
                     std::string_view owner, const char* name) const
        {
            return static_cast<int> (number (node, owner, name));
        }

        bool flag (const juce::ValueTree& node,
                   std::string_view owner, const char* name) const
        {
            const juce::Identifier property { name };

            if (node.hasProperty (property))
                return static_cast<bool> (node[property]);

            return text (node, owner, name) == "true";
        }

    private:
        std::map<std::string, std::string> defaults;
    };

    //======================================================================
    /** Where in the list a cue sits, and when - if when is knowable. */
    struct Placed
    {
        juce::ValueTree node;
        std::string id;
        std::string list;
        std::string element;

        int row = 0;

        /*  The group identifiers containing it, outermost first. A cue at
            the top level of a list has none. */
        std::vector<std::string> ancestors;

        bool timed = false;
        std::string chain;
        double from = 0.0;
        double to = 0.0;

        /*  WHETHER THE STANDBY POINTER MAY STAND HERE.

            §3.5, and PR 3.4 widened it: the pointer may sit at the top of a
            list or inside a MANUAL SEQUENCE group, because those are the ones
            whose members start on GO. It may not sit inside a timeline or an
            automatic group - nobody presses anything there - nor in a header or
            a footer, which are a group's own preparation and release rather
            than places an operator steps through.

            The solver needs it to say where a jump leaves the pointer: "the
            next row" is the wrong answer when the next row is a member of the
            timeline scene the jump has just landed in. */
        bool onManualPath = false;
    };

    /** The rows a group spans, so that "the group has ended" is a row. */
    struct Extent
    {
        int first = 0;
        int last = 0;
        bool endsOnItsOwn = false;
    };

    //======================================================================
    /*  Reading the show once: rows for every cue, extents for every group,
        and seconds wherever a chain makes them knowable.
    */
    class Walk
    {
    public:
        Walk (const Reader& readerToUse,
              const std::map<std::string, double>* durationsToUse)
            : reader (readerToUse), durations (durationsToUse)
        {
        }

        void visitList (const juce::ValueTree& list)
        {
            const auto id = list[idProperty].toString().toStdString();

            if (id.empty())
                return;

            currentList = id;
            nextRow = 0;

            for (const auto& child : list)
                if (isCueElement (child.getType().toString()))
                    place (child, {}, Timing {});

            listExtent[id] = nextRow - 1;
        }

        std::vector<Placed> placed;
        std::map<std::string, Extent> extents;
        std::map<std::string, int> listExtent;

        /*  Which cues never reach an end on their own, by identifier.

            A bed whose range plays for ever, a group looping for ever, a group
            containing either. The slot analysis asks it of GROUPS, to know
            where a claim is given back; the state solver asks it of every cue,
            because a cue that ends on its own is over by the time a later
            manual step is reached and one that does not is still going. One
            answer, two questions. */
        std::map<std::string, bool> unbounded;

    private:
        /*  Where a container's contents begin, when that is known at all.
            `origin` names the group the seconds are counted from. */
        struct Timing
        {
            bool known = false;
            std::string origin;
            double at = 0.0;
        };

        void place (const juce::ValueTree& node,
                    const std::vector<std::string>& ancestors,
                    const Timing& timing,
                    bool insideARole = false)
        {
            const auto element = node.getType().toString().toStdString();
            const auto id = node[idProperty].toString().toStdString();

            const auto length = lengthOf (node);

            Placed entry;
            entry.node = node;
            entry.id = id;
            entry.list = currentList;
            entry.element = element;
            entry.row = nextRow++;
            entry.ancestors = ancestors;

            /*  TIMED MEANS BOTH ENDS ARE KNOWN. A member of an automatic
                sequence whose file this build cannot read the length of
                starts at a known second and ends at an unknown one, and an
                interval with one end is not an interval - it would compare
                as empty and prove that nothing ever overlaps it, which is
                the one direction this analysis is not allowed to be wrong
                in. So it falls back to rows. */
            /*  Every ancestor a manual sequence, and not inside a header or a
                footer. See `Placed::onManualPath`. */
            entry.onManualPath = ! insideARole;

            for (const auto& groupId : ancestors)
            {
                const auto group = findGroup (groupId);

                if (! group.isValid()
                     || reader.text (group, "group", "mode") == "timeline"
                     || reader.text (group, "group", "advance") == "auto")
                    entry.onManualPath = false;
            }

            entry.timed = timing.known && length.has_value();
            entry.chain = entry.timed ? timing.origin : std::string {};
            entry.from = timing.at;
            entry.to = timing.at + length.value_or (0.0);
            placed.push_back (entry);

            unbounded[id] = ! endsOnItsOwn (node);

            if (element != "Group")
                return;

            auto inner = ancestors;
            inner.push_back (id);

            /*  HEADER, MEMBERS, FOOTER, which is the order they run in
                (§3.6) and therefore the order their rows are in. A header
                is an ordinary cue list that runs before the members
                whatever the group's own mode is; a footer runs at exit and
                blocks. */
            const auto memberTiming = timingInside (node, entry);

            const auto header = node.getChildWithName (juce::Identifier ("Header"));

            if (header.isValid())
                for (const auto& child : header)
                    if (isCueElement (child.getType().toString()))
                        place (child, inner, Timing {}, true);

            double running = memberTiming.known ? memberTiming.at : 0.0;
            bool chainAlive = memberTiming.known;
            const auto timeline = reader.text (node, "group", "mode") == "timeline";

            for (const auto& child : node)
            {
                if (! isCueElement (child.getType().toString()))
                    continue;

                Timing here;

                if (chainAlive)
                {
                    here.known = true;
                    here.origin = memberTiming.origin;
                    here.at = (timeline ? memberTiming.at : running)
                                + reader.number (child, "cue", "preWait");
                }

                place (child, inner, here, insideARole);

                if (chainAlive && ! timeline)
                {
                    /*  AN AUTOMATIC SEQUENCE IS A SUM, so one unknown
                        length ends the arithmetic for everything after it.
                        The members before it keep their seconds - they were
                        computed before the unknown - and the rest fall back
                        to rows, which is the conservative direction. */
                    const auto memberLength = lengthOf (child);

                    if (! memberLength.has_value())
                    {
                        chainAlive = false;
                        continue;
                    }

                    running = here.at + *memberLength
                                + reader.number (child, "cue", "postWait");
                }
            }

            const auto footer = node.getChildWithName (juce::Identifier ("Footer"));

            if (footer.isValid())
                for (const auto& child : footer)
                    if (isCueElement (child.getType().toString()))
                        place (child, inner, Timing {}, true);

            Extent extent;
            extent.first = entry.row;
            extent.last = nextRow - 1;
            extent.endsOnItsOwn = endsOnItsOwn (node);
            extents[id] = extent;
        }

        /*  The timing a group's MEMBERS run under, given the group's own
            placement.

            A chain is a timeline group or an automatic sequence, and it is
            computable only when nothing in the way of the arithmetic is
            unknown: one round (`loops` of 1), every member played
            (`play` of nought), in the written order (`sequential`), and no
            header - a header runs before the members and blocks, so its
            length would shift every member after it, and a header is
            exactly the place where an operator-paced cue turns up. A footer
            is not in the way: it runs after every member. */
        Timing timingInside (const juce::ValueTree& group, const Placed& entry) const
        {
            Timing none;

            const auto mode = reader.text (group, "group", "mode");
            const auto advance = reader.text (group, "group", "advance");

            if (mode != "timeline" && advance != "auto")
                return none;

            if (reader.integer (group, "group", "loops") != 1
                 || reader.integer (group, "group", "play") != 0
                 || reader.text (group, "group", "selection") != "sequential")
                return none;

            if (hasCuesIn (group, "Header"))
                return none;

            Timing inside;
            inside.known = true;

            /*  The outermost chain is the origin; a nested one keeps its
                parent's, so two cues at different depths of one chain are
                still comparable. */
            inside.origin = entry.timed ? entry.chain : entry.id;

            /*  ITS OWN PRE-WAIT IS ALREADY IN `entry.from` when a parent
                chain placed it - a cue's placed time is when it STARTS -
                and is not when it is the origin, where the seconds are
                counted from the moment the group was entered. §3.6: a
                group's pre-wait runs before its members begin their own. */
            inside.at = entry.timed ? entry.from
                                    : reader.number (group, "cue", "preWait");
            return inside;
        }

        /*  A group already placed, by identifier. A group is placed before it
            recurses into its members, so this never looks forward. */
        juce::ValueTree findGroup (const std::string& groupId) const
        {
            for (const auto& entry : placed)
                if (entry.id == groupId)
                    return entry.node;

            return {};
        }

        static bool hasCuesIn (const juce::ValueTree& group, const char* role)
        {
            const auto section = group.getChildWithName (juce::Identifier (role));

            if (! section.isValid())
                return false;

            for (const auto& child : section)
                if (isCueElement (child.getType().toString()))
                    return true;

            return false;
        }

        /*  How long a cue takes, in seconds, or nothing when the document
            does not say.

            A DISABLED CUE IS NOUGHT AND NOT UNKNOWN: it is skipped rather
            than deleted, so it takes no time and claims nothing. */
        std::optional<double> lengthOf (const juce::ValueTree& node) const
        {
            if (! reader.flag (node, "cue", "enabled"))
                return 0.0;

            const auto element = node.getType().toString();

            if (element == "Cue" || element == "Osc" || element == "Midi"
                 || element == "Stop")
                return 0.0;

            if (element == "Fade")
                return reader.number (node, "fade", "duration");

            if (element == "Media")
                return mediaLength (node);

            if (element == "Group")
                return groupLength (node);

            return std::nullopt;
        }

        std::optional<double> mediaLength (const juce::ValueTree& node) const
        {
            double total = 0.0;
            bool anyRange = false;

            for (const auto& child : node)
            {
                if (child.getType().toString() != "Range")
                    continue;

                anyRange = true;

                const auto passes = reader.integer (child, "range", "loops");

                /*  Nought passes is for ever, which is what an ambience bed
                    is. There is no number for that. */
                if (passes <= 0)
                    return std::nullopt;

                const auto span = reader.number (child, "range", "out")
                                    - reader.number (child, "range", "in");

                if (! (span > 0.0))
                    return std::nullopt;

                total += span * static_cast<double> (passes);
            }

            if (anyRange)
                return total;

            if (durations == nullptr)
                return std::nullopt;

            const auto file = reader.text (node, "media", "file");
            const auto found = durations->find (file);

            /*  Nought is "this build could not read it", which the side
                table is explicit about, and it is exactly the case the
                solver's confused list exists for. Not a length. */
            if (found == durations->end() || ! (found->second > 0.0))
                return std::nullopt;

            const auto span = found->second - reader.number (node, "media", "startOffset");
            return span > 0.0 ? std::optional<double> (span) : std::nullopt;
        }

        std::optional<double> groupLength (const juce::ValueTree& node) const
        {
            if (hasCuesIn (node, "Header") || hasCuesIn (node, "Footer"))
                return std::nullopt;

            const auto mode = reader.text (node, "group", "mode");
            const auto advance = reader.text (node, "group", "advance");

            if (mode != "timeline" && advance != "auto")
                return std::nullopt;

            const auto rounds = reader.integer (node, "group", "loops");

            if (rounds < 1 || reader.integer (node, "group", "play") != 0
                 || reader.text (node, "group", "selection") != "sequential")
                return std::nullopt;

            const auto timeline = mode == "timeline";
            double running = 0.0;
            double furthest = 0.0;

            for (const auto& child : node)
            {
                if (! isCueElement (child.getType().toString()))
                    continue;

                const auto length = lengthOf (child);

                if (! length.has_value())
                    return std::nullopt;

                const auto begins = (timeline ? 0.0 : running)
                                      + reader.number (child, "cue", "preWait");

                furthest = std::max (furthest, begins + *length);

                if (! timeline)
                    running = begins + *length + reader.number (child, "cue", "postWait");
            }

            return (reader.number (node, "cue", "preWait") + furthest)
                     * static_cast<double> (rounds);
        }

        /*  Whether this cue reaches an end without anybody stopping it.

            A GROUP IS ONLY AS BOUNDED AS ITS MEMBERS. A group whose
            `loops` is nought runs for ever by construction; one containing
            an ambience bed never advances past it, so it runs for ever
            too, and the fact that the group itself says `loops` of one
            changes nothing. */
        bool endsOnItsOwn (const juce::ValueTree& node) const
        {
            if (! reader.flag (node, "cue", "enabled"))
                return true;

            const auto element = node.getType().toString();

            if (element == "Media")
            {
                for (const auto& child : node)
                    if (child.getType().toString() == "Range"
                         && reader.integer (child, "range", "loops") <= 0)
                        return false;

                return true;
            }

            if (element != "Group")
                return true;

            if (reader.integer (node, "group", "loops") <= 0)
                return false;

            for (const auto& child : node)
            {
                if (isCueElement (child.getType().toString()))
                {
                    if (! endsOnItsOwn (child))
                        return false;

                    continue;
                }

                const auto role = child.getType().toString();

                if (role != "Header" && role != "Footer")
                    continue;

                for (const auto& inner : child)
                    if (isCueElement (inner.getType().toString())
                         && ! endsOnItsOwn (inner))
                        return false;
            }

            return true;
        }

        const Reader& reader;
        const std::map<std::string, double>* durations;
        std::string currentList;
        int nextRow = 0;
    };
}
