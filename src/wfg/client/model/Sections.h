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
    A sound cue's sections, as the waveform panel draws and drags them
    (namespace draft §55).

    THE SAME ARITHMETIC THE ENGINE WORKS BY, restated because the boundary
    forbids reaching for it - `model/Lane`'s arrangement: `doc::MediaEdit` is
    the one judge (the timeline, the clamp on a crossfade), `doc::` is a token
    this half of the program may not name, and the test that keeps the two
    honest asserts these against the real ones.

    WHAT A SECTION IS: a piece of the file, its in and out points in seconds
    of the FILE, a trim in dB, a fade at each end and a gap of silence before
    it (55.9) - and, until 55.9, the crossfade at the join into it from the
    section before, centred on the join. The sections in their order are the
    EDITED TIMELINE: section k begins where section k-1 ends, and the edit is
    as long as the sections put together - which is the cue's file time while
    the edit is open, the time its lane, its ranges and its playhead are on.

    NOTHING HERE WRITES. Each gesture is one command on release; the carry of
    what sat on the timeline is the engine's, whichever verb asked.
*/

#include <wfg/client/model/View.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    struct SectionRow
    {
        std::string id;
        int index = 0;            ///< its place in the edited timeline
        double in = 0.0;          ///< seconds of the file
        double out = 0.0;
        double trimDb = 0.0;
        double fadeIn = 0.01;     ///< seconds: centred on a join, inside the section at a free edge (55.9)
        double fadeOut = 0.01;
        double fadeInCurve = 0.0; ///< -1..1
        double fadeOutCurve = 0.0;
        double gap = 0.0;         ///< seconds of silence before it

        double length() const noexcept { return out - in; }
    };

    /** Every section of this cue, in the order of its edited timeline; none for a cue with no edit. */
    std::vector<SectionRow> readSections (const tree::TreeSnapshot&, const std::string& cueId);

    /** Where each section begins on the edited timeline: its gap after where the one before ends. */
    std::vector<double> sectionStarts (const std::vector<SectionRow>&);

    /** The edit's length: where the last section ends. */
    double editedLength (const std::vector<SectionRow>&);

    /** Section `index` touches the one before it: no gap, or one under a millisecond. */
    bool isJoin (const std::vector<SectionRow>&, std::size_t index) noexcept;

    /** A join still one in the file, as a split leaves it: plays plain, whatever its fades. */
    bool continuousJoin (const SectionRow& before, const SectionRow& after) noexcept;

    /*  THE FADES, as the engine hears them (55.9, AEA): held to the material
        - twice the in point for a fade in centred on a join, what the
        section's length leaves beside its other fade - and nought at a join
        still one in the file. */
    struct SectionFades
    {
        double in = 0.0;
        double out = 0.0;
        bool inCentred = false;
        bool outCentred = false;
        bool plainIn = false;
        bool plainOut = false;
        double inCurve = 0.0;
        double outCurve = 0.0;
    };

    std::vector<SectionRow> clampFades (std::vector<SectionRow>);
    std::vector<SectionFades> heardFades (const std::vector<SectionRow>&);

    /** A fade in's gain at a progress through it: equal power for a sound, straight for a picture, to 2^-curve (AEB). */
    double fadeGain (double progress, double curve, bool picture) noexcept;

    //==============================================================================
    /** One section's span on the bar, in pixels, for the view looked through. */
    struct SectionLayout
    {
        std::size_t index = 0;
        double x0 = 0.0;
        double x1 = 0.0;
        bool join = false;      ///< it touches the one before: its left edge is a join
    };

    /** The sections that meet the view, each with its pixel span; a section outside it is left out. */
    std::vector<SectionLayout> layoutSections (const std::vector<SectionRow>&, const View&, int width);

    enum class SectionHit { none, block, join };

    struct SectionHitResult
    {
        SectionHit hit = SectionHit::none;
        std::size_t index = 0;   ///< the section, or the section a join leads INTO
    };

    /** A join within `joinGrab` pixels of a section's left edge wins over the block; the first section has no join. */
    SectionHitResult hitSection (const std::vector<SectionLayout>&, double x, double joinGrab) noexcept;

    /*  Where a dragged block lands: the place among the sections, counting
        from nought, that a release at `x` means - after every block whose
        middle is left of it - or -1 when that is where it already is. Over
        the whole list, so a section outside the view counts too. */
    int dropSlotFor (const std::vector<SectionRow>&, const View&, int width, std::size_t dragged, double x);

    /** A join handle dragged to `pointerSeconds`: the crossfade is twice the distance from the join. */
    double crossfadeFromDrag (double joinSeconds, double pointerSeconds) noexcept;

    //==============================================================================
    /*  THE GRIPS ON THE BAR (namespace draft 55.9, ADY): in each section's
        middle the volume, at the height of its trim on the lane's scale; at
        each end the edge, at the bar's foot, and the fade's length, at its
        top, where the fade has reached full level. */
    enum class Grip { none, volume, fadeIn, fadeOut, edgeIn, edgeOut };

    struct GripHit
    {
        Grip grip = Grip::none;
        std::size_t index = 0;

        bool operator== (const GripHit&) const = default;
    };

    /** Where a fade's top handle sits on the edited timeline: its full-level end, the fade as the door holds it. */
    double fadeHandleSeconds (const std::vector<SectionRow>&, std::size_t index, bool inSide);

    /*  The grip under a point of the bar - `x` and `y` in pixels from its top
        left, `height` its height - within `radius` pixels: the volume first,
        then a fade's handle in the top band, an edge in the foot band. At a
        join both edges are one, the incoming section's; two fade handles on
        one spot go by which side of the join the pointer is. */
    GripHit hitGrip (const std::vector<SectionRow>&, const View&, int width, int height, double x, double y,
                     double radius, bool volumeShown);

    /** The height of the bands at the top and the foot of a bar `height` pixels tall that the handles live in. */
    double gripBand (int height) noexcept;

    /** A fade's top handle dragged to `seconds` of the edited timeline: the fade's length that means. */
    double fadeFromHandle (const std::vector<SectionRow>&, std::size_t index, bool inSide, double seconds);

    /*  An edge's foot handle dragged to `seconds` of the edited timeline: the
        second of the file that means, held where section.edge takes it - a
        roll that leaves both sections something, a trim that stays inside
        the silence beside it and the file (`fileLength`, nought unknown). */
    double edgeFromHandle (const std::vector<SectionRow>&, std::size_t index, bool inSide, double seconds,
                           double fileLength);

    /*  THE EDIT AS A DRAG WOULD LEAVE IT, for the bar to draw before the one
        write on release - section.edge, section.fade and section.curve
        restated: a roll or a trim into the silence; a fade or a curve with its
        partner across a join by as much unless alone; the fades held. */
    std::vector<SectionRow> withEdge (std::vector<SectionRow>, std::size_t index, bool inSide, double fileSeconds);
    std::vector<SectionRow> withFade (std::vector<SectionRow>, std::size_t index, bool inSide, double seconds, bool alone);
    std::vector<SectionRow> withCurve (std::vector<SectionRow>, std::size_t index, bool inSide, double curve, bool alone);

    /*  THE FADE UNDER THE POINTER, for the wheel (55.9): the section and side
        whose fade spans `seconds`, with `slack` seconds of grace either side
        of a short one; at a join, the side the pointer is on. */
    std::optional<std::pair<std::size_t, bool>> fadeAt (const std::vector<SectionRow>&, double seconds, double slack);

    /** The section an edited second falls in; nothing in a gap or past the end. */
    std::optional<std::size_t> sectionAt (const std::vector<SectionRow>&, double seconds);

    //==============================================================================
    /*  A MOVIE'S STRIP THROUGH ITS EDIT (namespace draft §55.5, ADV): the
        strip's thumbnails and cuts are the FILE's, read once, and the bar
        draws the edited timeline - so each slot of the bar asks which second
        of the file an edited second is, and each cut of the file is laid where
        the section holding it puts it. */

    /** Which section an edited second falls in, and the file's second there. */
    struct SectionPlace
    {
        std::size_t index = 0;
        double fileSecond = 0.0;
    };

    /*  The place of an edited second: nothing before the top or past the end
        (the end itself is the last section's). The engine's `placeOf`. */
    std::optional<SectionPlace> placeOf (const std::vector<SectionRow>&, double editedSecond) noexcept;

    /*  The file's cuts on the edited timeline: for each section, every cut
        strictly inside it, moved to where the section begins; in order. The
        cuts themselves with no sections. */
    std::vector<double> cutsOnTimeline (const std::vector<SectionRow>&, const std::vector<double>& fileCuts);

    //==============================================================================
    /** "-6 dB", "0 dB", "+3 dB". */
    std::string trimText (double dB);

    /** A trim typed back, by the level box's rules (`levelFrom`). */
    std::optional<double> trimFrom (const std::string& typed);

    /** The words on a block: its number, then its in and out as the ruler writes them. */
    std::string sectionLabel (const SectionRow&, std::size_t index);

    //==============================================================================
    /*  THE RENDER OF A CUE'S OPEN EDIT, as `/godot/engine/editRender` says it:
        one line a cue - cue, state, percent, problem - a tab between each. */
    struct EditRenderRow
    {
        std::string cue;
        std::string state;      ///< rendering, done or failed; empty when the cue has no render
        std::string problem;
        int percent = 0;
    };

    EditRenderRow readEditRender (const tree::TreeSnapshot&, const std::string& cueId);

    /** What the panel says of it: "rendering the edit, 42 %", "the edit is rendered", why it failed; nothing with no render. */
    std::string renderWords (const EditRenderRow&);
}
