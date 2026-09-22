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
    WHAT THE PANEL AT THE FOOT OF THE WINDOW IS SHOWING.

    The author's shape for it (2026-09-21): *"the panel at the bottom opens on
    specific things not tabbed or cramming all at once"*. So it is not a tab
    bar and there is no "everything about this cue" view. A gesture names a
    SUBJECT — the waveform of this cue, the send levels of that one, a fade's
    curve — and the panel opens on it, titled with what it is and which object
    it belongs to.

    THE HOST IS THE POINT, and the editors are interchangeable. Adding a kind
    below is adding an editor and a way to ask for it, and nothing else moves.
    That is deliberate, because the author has named what is coming: a VST
    interface for processing media files, the slots of the effects channels,
    state-machine processing channels, *"and so on"*. None of them is built;
    all of them are this shape.

    A SUBJECT SURVIVES A PICK WHERE THAT MAKES SENSE. The waveform of the cue
    somebody just clicked is what they want to see next; a fade's curve is not,
    when the thing picked is a group. `followsPick` is that rule, in one place,
    so the panel does not have to guess per kind.

    std only: what the panel needs from the tree is gathered here so the window
    keeps its one snapshot read (§14.16 rule 2, and the boundary check that
    enforces it).
*/

#include <string>
#include <vector>

#include <wfg/client/model/Ranges.h>
#include <wfg/client/model/Sends.h>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct Subject
    {
        /*  One per editor. `none` is the panel shut; every other value is an
            editor that exists, so a kind is added here when its editor is. */
        enum class Kind { none, waveform, sends };

        Kind kind = Kind::none;
        std::string objectId;

        bool operator== (const Subject&) const = default;
        bool isOpen() const noexcept { return kind != Kind::none; }
    };

    /** Whether a subject of this kind re-points itself when the pick moves. */
    bool followsPick (Subject::Kind);

    /*  Everything the foot needs for one pass, read while the window has its
        snapshot open. Only the fields the OPEN subject uses are filled: a shut
        panel costs one comparison, and a waveform does not pay for a reading
        the sends will want later.  */
    struct FootReading
    {
        Subject subject;

        std::string cueName;     ///< what the title says it is showing
        std::string cueKind;
        std::string file;        ///< the media the cue names, for the analyser's table
        double fileLength = 0.0; ///< the cue's `duration`, or nought when unknown
        double startOffset = 0.0;

        std::vector<RangeRow> ranges;

        /*  THE MIX CHANNELS AND WHAT THIS CUE SENDS INTO THEM, filled only
            when the sends are what is open. `cueLevel` is the cue's own
            `media/level`, which the mixer draws as its master strip: it is not
            a fourth kind of number, it is the same one the inspector shows and
            the same one a fade drives, and putting it at the left of the mixer
            is what makes the picture true. */
        std::vector<SendStrip> sends;
        double cueLevel = 0.0;

        /*  Where the playhead is, when a run of this cue is sounding, and
            whether there is one at all. A cue with no run has no playhead, and
            drawing one at nought would be the panel inventing a reading.

            `runId` is WHICH run, because the panel can now talk to it: a drag
            in the ruler seeks it and the pause button stops it, and both are
            addressed to the run rather than to the cue. Empty when nothing of
            this cue is sounding. */
        bool running = false;
        double position = 0.0;
        std::string runId;

        /** Empty when there is nothing to say; a sentence when there is. */
        std::string notice;
    };

    FootReading readFoot (const tree::TreeSnapshot&, const Subject&);
}
