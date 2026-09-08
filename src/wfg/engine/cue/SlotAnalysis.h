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
    Which cues can be holding the same slot at the same time, worked out by
    reading the show rather than by running it.

    PRD §3.9c: each binding has a live range over show time, overlapping ranges
    cannot share a resource, and re-analysis on edit is what turns a reorder
    into a warning. This is that analysis. It runs over the document alone - no
    engine, no audio, no run table - so `wfg validate` gives the same answer as
    a running session, and a designer moving a cue at eleven in the morning
    finds out then rather than at the performance.

    THE COORDINATE IS A ROW, AND SOMETIMES A SECOND.

    A manual cue list has no time in it. Between one GO and the next there is a
    person, and a person is not a duration; so the honest coordinate across a
    manual boundary is the ROW - a claim is live from the row of the cue that
    takes it to the row where the structure guarantees it is given back. Inside
    a timeline or an automatic chain there is no person, so offsets and
    durations are exact and the analysis uses seconds instead. §13.5 says
    exactly this, and it is the difference between an analysis that warns about
    two members of an automatic sequence (which cannot overlap, and would be
    noise) and one that does not.

    CONSERVATIVE IN ONE DIRECTION ONLY. Some ranges are genuinely indefinite: a
    bed that loops for ever, a manual group whose members are operator-paced, a
    media file this build cannot read the length of. So the analysis can prove
    POSSIBLE overlap and can never prove impossible, and every answer it gives
    leans that way. Which is why an overlap is a warning and never a refusal
    (§3.9c: *warn, don't refuse*), why it never changes `wfg validate`'s exit
    code, and why `shared` on either cue's `Feed` or `Insert` silences the pair
    - that is a designer saying, permanently and in the document, that they
    considered it.

    WHERE A CLAIM ENDS, in rows, in the order the answers are looked for:

      1. A `Stop` cue after it in the same list aimed at the cue itself or at a
         group containing it. That is a release somebody wrote down.
      2. The innermost enclosing group that ends on its own - one whose `loops`
         is not nought and every one of whose members ends on its own. A group
         ending means its members' runs have ended, so the claim is back.
      3. Otherwise the end of the list. A finite media cue at the top level of a
         manual list is the ordinary case here: it may well have finished long
         before the next GO, and it may equally still be sounding, and nothing
         in the document says which.

    ACROSS LISTS IT IS ALWAYS AN OVERLAP. Two lists can be live at once and
    nothing orders their rows against each other, so a pair on one slot in two
    lists is the same warning. §3.9c's *(proposed)* cross-list refusal is not
    built; `shared` is the override it asks for and one attribute already gives.

    A CACHE ASKED RATHER THAN TOLD, which is PR 3.2's shape and the reason
    `ShowDocument` grew a revision counter: a `markStale` call is a line every
    future write path has to remember, and the one that forgets produces a stale
    reading indistinguishable from a correct one. `/godot/engine/analysisRebuilds`
    publishes the count so the guarantee is asserted by counting rather than by
    timing (M18).

    IT CARRIES THE DOCUMENT'S OWN REFERENCE WARNINGS TOO, which is not scope
    creep but the same cache. `ShowDocument::warnings()` is a full walk of the
    show, `/godot/document/warnings` publishes it beside the overlaps, and the
    runtime half of the tree is rebuilt every tick - so asking the document
    directly from there would be that walk fifty times a second for an answer
    that changes when somebody edits a cue. Both are functions of the document
    at one revision. One revision, one cache.

    THREADING: none of its own. The tick thread builds it and reads it, exactly
    as it owns the document. `wfg validate` builds one on its own thread with no
    engine anywhere.
*/

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wfg::doc { class ShowDocument; }

namespace wfg::cue
{
    /** One claim, and how long it is live for. */
    struct SlotUse
    {
        /** The slot claimed, by identifier. */
        std::string slot;

        /** The cue that claims it. */
        std::string cue;

        /** The list the cue is in. Two uses in different lists never order. */
        std::string list;

        /*  The cue whose row the claim is released at, which may be the
            claiming cue itself when a stop cue is aimed at it, and is the
            last cue of the list when nothing ends it. */
        std::string until;

        /** Rows within the list, in document order, header then members then
            footer. Inclusive at both ends. */
        int firstRow = 0;
        int lastRow = 0;

        /*  Either the `Feed` or the `Insert` carrying this claim is marked
            `shared`: the designer says the sharing is meant. Silences every
            pair this use is half of, and nothing else. */
        bool shared = false;

        /*  Whether `from` and `to` mean anything. True only inside a timeline
            or automatic chain where every offset and duration between the
            chain's entry and this cue is known. */
        bool timed = false;

        /** The group whose entry `from` and `to` are counted from. */
        std::string chain;

        /** Seconds from that entry. Meaningless unless `timed`. */
        double from = 0.0;
        double to = 0.0;
    };

    /** Two claims on one slot whose live ranges intersect. */
    struct SlotClash
    {
        std::string slot;
        std::string first;
        std::string second;
    };

    //==============================================================================
    class SlotAnalysis
    {
    public:
        /*  Rebuilds if the document has moved since the last build, and does
            nothing at all if it has not.

            `durations` is the media side table, by the bundle-relative path the
            document writes, or nullptr where none was read - a replay, a test,
            a `wfg tree` of a bundle with no media folder. Absent, every media
            cue's length is unknown, every chain containing one is untimed, and
            the analysis falls back to rows: more warnings rather than fewer,
            which is the direction it is allowed to be wrong in. */
        void ensureBuilt (const doc::ShowDocument& document,
                          const std::map<std::string, double>* durations);

        /** Every live range, in document order. */
        const std::vector<SlotUse>& uses() const noexcept { return liveRanges; }

        /** Every intersecting pair, in document order. */
        const std::vector<SlotClash>& clashes() const noexcept { return intersections; }

        /** `<first> <last>` pairs for one slot, space-separated, for
            `/godot/slot/<id>/usage`. */
        std::string usageOf (const std::string& slotId) const;

        /** `<a> <b>` pairs for one slot, space-separated, for
            `/godot/slot/<id>/overlaps`. */
        std::string overlapsOf (const std::string& slotId) const;

        /** One sentence per intersecting pair. Printed by `wfg validate`, and
            it does NOT change that verb's exit code. */
        const std::vector<std::string>& overlapWarnings() const noexcept { return overlapText; }

        /** The document's own dangling references, cached at the same revision.
            These DO carry exit 1: a reference is a mistake somebody can fix by
            fixing it. */
        const std::vector<std::string>& referenceWarnings() const noexcept { return referenceText; }

        /** Both of the above, newline-separated, for `/godot/document/warnings`. */
        std::string warningText() const;

        /** How many times this has actually been rebuilt. M18 counts it. */
        std::size_t rebuilds() const noexcept { return rebuildCount; }

    private:
        std::vector<SlotUse> liveRanges;
        std::vector<SlotClash> intersections;
        std::vector<std::string> overlapText;
        std::vector<std::string> referenceText;

        /*  Nought means "never built", which is why `ShowDocument::revision()`
            starts at one. */
        std::uint64_t builtAt = 0;

        /*  The side table the last build read. Compared by ADDRESS: the map is
            filled once when the show is opened and handed over by pointer, so a
            different pointer is a different show's media and the same pointer
            is the same numbers. */
        const std::map<std::string, double>* builtWith = nullptr;

        std::size_t rebuildCount = 0;
    };
}
