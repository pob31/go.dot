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

#include <cstddef>
#include <string>
#include <vector>

#include <wfg/client/model/Curve.h>
#include <wfg/client/model/Eq.h>
#include <wfg/client/model/FadeMix.h>
#include <wfg/client/model/Fx.h>
#include <wfg/client/model/Lane.h>
#include <wfg/client/model/OscCurves.h>
#include <wfg/client/model/OscMessages.h>
#include <wfg/client/model/Ranges.h>
#include <wfg/client/model/Sends.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Take.h>
#include <wfg/client/model/Timeline.h>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct Subject
    {
        /*  One per editor. `none` is the panel shut; every other value is an
            editor that exists, so a kind is added here when its editor is. */
        enum class Kind { none, waveform, sends, timeline, curve, eq, fx, take, fade, messages };

        Kind kind = Kind::none;
        std::string objectId;

        bool operator== (const Subject&) const = default;
        bool isOpen() const noexcept { return kind != Kind::none; }
    };

    /** Whether a subject of this kind re-points itself when the pick moves. */
    bool followsPick (Subject::Kind);

    /*  WHETHER A PANEL OF THIS KIND EDITS EVERY PICKED CUE AT ONCE (namespace
        draft §30.11): the sends and the EQ do, over the media and mic cues
        picked. Every other panel is one cue's - a waveform, a chain, a take, a
        timeline, a fade - and stays on the one it opened on. */
    bool servesMany (Subject::Kind);

    /*  THE CUES SUCH A PANEL ACTS ON: of `picked`, the media and mic cues,
        `anchor` first when it is one of them, then the others in the order
        they were picked. Empty when the panel is one cue's, or when fewer than
        two cues are picked. */
    std::vector<std::string> footCues (const tree::TreeSnapshot&, Subject::Kind, const std::string& anchor,
                                       const std::vector<std::string>& picked);

    /*  WHAT THE HEAD SAYS IN PLACE OF ONE CUE'S NAME: "6 cues" when the panel
        acts on every cue picked, "6 of 8 cues" when some were passed over. */
    std::string manyCuesWords (std::size_t acting, std::size_t picked);

    /*  COPY AND PASTE ON THE FOOT (namespace draft §38, the author's request
        of 2026-10-07): the part of a cue a panel shows - "time" for the
        waveform (start offset, speed, the Ranges), "sends", "eq", "fx" - or
        empty for a panel whose content is not a part of a cue. */
    std::string partForPanel (Subject::Kind);

    /*  WHAT A PART ON A CLIPBOARD IS: its part words, the element of the cue it
        came from ("Media", "Mic", "Video") and that cue. Empty `parts` for
        text that is not part of a cue - a fragment of whole cues among it. */
    struct PartClip
    {
        std::string parts;
        std::string element;
        std::string sourceId;

        bool isPart() const noexcept { return ! parts.empty(); }
    };

    PartClip readPartClip (const std::string& text);

    /*  WHETHER A CUE OF THIS KIND ("media", "mic", "video") TAKES THE CLIP: every
        part fits its kind, and an effects chain only between cues of one kind
        (WU). The engine asks the same and refuses what does not; this is what
        keeps a paste from being sent to be refused. */
    bool takesPart (const PartClip&, const std::string& kind);

    /*  THE CUES A PASTE GOES ONTO: every picked cue that takes the clip when the
        panel's cue is among those picked, else the panel's cue alone - the one
        a person is looking at - and never the cue the part came from, which
        already is what it would become. */
    std::vector<std::string> pasteTargets (const tree::TreeSnapshot&, const PartClip&,
                                           const std::string& panelCue,
                                           const std::vector<std::string>& picked);

    /*  THE WORD A SUBJECT GOES BY - "waveform", "sends", "eq" - which is what
        an inspector's panel button carries and what `model::iconForPanel`
        reads, and the way back from it. Empty and `none` for each other. */
    std::string wordFor (Subject::Kind);
    Subject::Kind subjectKindFor (const std::string& word);

    /*  ONE SEND'S LANE, as the waveform's picker offers it (namespace draft
        §28, QB): the send it rides, the mix it feeds by name - which is the
        word the picker shows - and its points. A send of the cue, so a mix
        the cue does not send into has none to draw. */
    struct SendLaneReading
    {
        std::string sendId;
        std::string busName;
        std::vector<LanePoint> points;

        /*  The mix, by identifier: the key of its lane on the flipped faders
            (namespace draft §34). */
        std::string busId;

        /*  The send's written level, which its lane is an offset on: what a
            flipped fader's ride, the number as heard, is drawn less of. */
        double writtenDb = 0.0;
    };

    /*  Everything the foot needs for one pass, read while the window has its
        snapshot open. Only the fields the OPEN subject uses are filled: a shut
        panel costs one comparison, and a waveform does not pay for a reading
        the sends will want later.  */
    struct FootReading
    {
        Subject subject;

        /*  SEVERAL CUES AT ONCE (namespace draft §30.11): the cues a send mixer
            or an EQ panel acts on, the lead first - the anchor, when it is one
            of them - whose values are the ones drawn; `subject` then names the
            lead. Empty when the panel is on one cue, which is every other
            panel, and these two when fewer than two cues are picked. `picked`
            is how many cues are picked in all, for "6 of 8". */
        std::vector<std::string> cues;
        std::size_t picked = 0;

        bool many() const noexcept { return cues.size() > 1; }

        std::string cueName;     ///< what the title says it is showing
        std::string cueKind;
        std::string file;        ///< the media the cue names, for the analyser's table
        double fileLength = 0.0; ///< the cue's `duration`, or nought when unknown
        double startOffset = 0.0;

        /*  THE SPEED THE CUE PLAYS AT AND ITS MODE (namespace draft §22.7), as
            the document says them: the head row carries both, since the clock
            beside them counts the file's seconds and not the room's. */
        double rate = 1.0;
        std::string rateMode;

        std::vector<RangeRow> ranges;

        /*  THE CUE'S LEVEL LANE, drawn over the waveform (namespace draft
            §20.5), and whether the show is locked - which is when nothing on
            the lane may be grabbed, the lane being a decision the lock keeps. */
        std::vector<LanePoint> lane;
        bool locked = false;

        /*  AND ITS SENDS' LANES (namespace draft §28), one per send the cue
            has, in the order the show declares its mixes - the picker's
            entries after *Level*. Filled with the waveform. */
        std::vector<SendLaneReading> sendLanes;

        /*  AND A LANE BEING RECORDED FROM A FADER (namespace draft §20.9) -
            whichever cue it is for, so the waveform can say it is another's. */
        LaneRecordReading laneRecord;

        /*  THE MIX CHANNELS AND WHAT THIS CUE SENDS INTO THEM, filled only
            when the sends are what is open. `cueLevel` is the cue's own
            `media/level`, which the mixer draws as its master strip: it is not
            a fourth kind of number, it is the same one the inspector shows and
            the same one a fade drives, and putting it at the left of the mixer
            is what makes the picture true. */
        std::vector<SendStrip> sends;
        double cueLevel = 0.0;

        /*  And every picked cue's own level, in `cues`' order, over several
            (§30.11): the master strip's band, and where each starts a drag. */
        std::vector<double> cueLevels;

        /*  A GROUP'S MEMBERS IN TIME, filled only when the timeline is what is
            open. It carries its own notice, which the panel shows in place of
            the reading's - a group with no members and a cue that is not a
            group are different sentences. */
        TimelineReading timeline;

        /*  THE SHAPE A FADE TAKES, filled only when the curve is what is open.
            It carries its own notice, as the timeline does: a cue that is not a
            fade and a fade nobody has drawn on are different sentences. */
        CurveReading curve;

        /*  WHAT A FADE MOVES, filled only when its mixer is what is open
            (namespace draft §26): a strip for each slider, a row for each EQ
            number and plugin value, a door for each of the target's inserts. */
        FadeMixReading fadeMix;

        /*  AN OSC CUE'S MESSAGES, filled only when they are what is open
            (namespace draft 45, O.5): the cue's own message first, each value
            with its type and the curve on it. */
        OscMessagesReading oscMessages;

        /*  AND ITS CURVES, beside the table (namespace draft 45, O.7): each on
            the cue's own time, against an axis of its own. */
        OscCurvesReading oscCurves;

        /*  THE CUE'S EQ, filled only when the EQ is what is open (Phase 9a):
            the twenty-three rows as one value, the same value the voice is
            given, with its own notice for a cue that has none. */
        EqReading eq;

        /*  And every picked cue's, in `cues`' order, over several (§30.11):
            the lead's is `eq`, drawn; the rest are what a gain moved by hand
            moves each from. */
        std::vector<EqReading> eqs;

        /*  THE CUE'S SIGNAL CHAIN, filled only when the FX panel is what is
            open (author, 2026-09-25): the show's plugins in the order the
            sound goes through them, and what this cue does with each. The
            chain starts with the EQ, so `eq` above is filled with it - the
            first box is the EQ's, with its own switch. */
        FxReading fx;

        /*  THE TAKE OF THE CHANNEL A MIC CUE PLAYS THROUGH, filled only when
            the take panel is what is open (Phase 9c): its state, length,
            layers, points and playhead, with its own notice for a cue that has
            none. The peaks come through their own door, beside this. */
        TakeReading take;

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

    /*  `picked` is the list's selection, for the panels that serve several
        cues at once (`servesMany`); a panel on one cue ignores it. */
    FootReading readFoot (const tree::TreeSnapshot&, const Subject&,
                          const std::vector<std::string>& picked = {});

    /*  WHERE THE FOOT GOES WHILE A SURFACE SHOWS A PAGE OF A CUE (author,
        2026-09-25: "When adjusting either EQ or send levels display the
        footer on screen"): the aimed cue's EQ panel for an EQ page, its send
        mixer for a Send page, its chain for an FX page and its take for a
        Loop page (Phase 9c). Nothing otherwise, and the foot is the window's
        own again.

        FROM THE PRESS THAT PUTS THE PAGE UP (author, 2026-10-05: "Pressing
        the Eq toggle on the controller does switch the rotaries to EQ, but it
        should open also the EQ footer for the selected channel for
        visualisation"; namespace draft §30.5, RN). Until then it waited for the
        first turn, on the reading that a page only up had adjusted nothing;
        but the page is where the hand is about to adjust, and the panel is
        what it adjusts, drawn whole. `page` is `readSurfacePage`'s answer,
        passed by its parts so this file needs no surface model. */
    Subject footForSurface (bool pageUp, const std::string& pageWord, const std::string& aim);
}
