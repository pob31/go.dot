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
    A PATCH AS THE CANVAS DRAWS IT, AND THE EDITS IT MAKES (namespace draft 51,
    ACM; PC.5, PC.6).

    The canvas at the foot of the window draws a process cue's patch where Pd
    would: each box at its place, as wide and as tall as Pd makes it - Pd's own
    font table and margins, so a patch made in Pd lands here as it looks there -
    its inlets and outlets spread along its edges, the lines from an outlet's
    foot to an inlet's head. Pd's coordinates are the canvas's; the window
    scales them.

    AN EDIT IS A NEW TEXT. Every gesture - boxes moved, boxes and lines deleted,
    a box typed, placed or joined - is a function from the patch's text to the
    text it becomes, and the window sends that whole text as one `node.set` of
    the cue's `patch`. Records the gesture did not touch are kept byte for byte
    (PatchText), so a patch from Pd that was only moved here differs from Pd's
    own file by the lines that moved.

    HOW MANY INLETS AND OUTLETS. Pd knows from the object; the window has no Pd
    to ask. A table holds the vanilla objects a show uses, by name and
    arguments; a subpatch has as many as its [inlet]s and [outlet]s; anything
    else has as many as its lines need, and at least one of each. Pd's own
    window is exact, and is the door for what the table guesses wrong.

    Standard library only (namespace draft 14.16).
*/

#include <wfg/engine/process/PatchText.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::model
{
    /*  WHAT THE CANVAS AT THE FOOT SHOWS (PC.5): the cue, its patch's text,
        whether the show lets it be edited, and while it runs what its run says -
        its state, its last printed line, and the last value on each of its named
        ports (PC.8 draws those on the lines). */
    struct PatchReading
    {
        std::string cueId;
        std::string text;
        bool locked = false;
        std::string runId;
        std::string state;
        std::string said;
        std::string ports;

        /*  WHAT IT OPENS IN (PC.7): plugdata, Pd, or empty when this machine has
            neither; and Pd's download as the engine says it. */
        std::string editor;
        std::string editorInstall;
    };

    PatchReading readPatchFoot (const tree::TreeSnapshot& snapshot, const std::string& cueId);

    /*  Pd's font metrics for a canvas's font size: a character's width and a
        line's height, in Pd's pixels, from Pd's own table. */
    struct PdFont
    {
        int size = 12;
        double charWidth = 7.0;
        double lineHeight = 16.0;
    };

    PdFont pdFontFor (int size);

    struct PatchBoxView
    {
        std::size_t box = 0;                 // its index in Patch::boxes
        process::BoxKind kind = process::BoxKind::object;
        double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
        std::vector<std::string> lines;      // as Pd shows it: escapes undone, wrapped
        int inlets = 0;
        int outlets = 0;
    };

    struct PatchLineView
    {
        std::size_t line = 0;                // its index in Patch::lines
        std::size_t fromView = 0, toView = 0;   // indexes into PatchView::boxes
        int outlet = 0, inlet = 0;
        double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0;
    };

    struct PatchView
    {
        std::string problem;
        PdFont font;
        std::size_t canvas = 0;
        std::vector<PatchBoxView> boxes;
        std::vector<PatchLineView> lines;
        double right = 0.0, bottom = 0.0;    // how far its boxes reach

        /*  The view of the box at index `box` of Patch::boxes, or none. */
        const PatchBoxView* viewOf (std::size_t box) const;
    };

    /*  The patch, or one of its subpatches, as the canvas draws it. */
    PatchView viewPatch (const process::Patch& patch, std::size_t canvas = 0);

    /*  An inlet's or an outlet's place: the left edge of its small rectangle. */
    double portX (const PatchBoxView& box, int index, int count);
    inline constexpr double portWidth = 7.0;
    inline constexpr double portHeight = 3.0;

    /*  WHAT IS UNDER A POINT, the topmost first: an outlet or an inlet before
        the box it is on - the hand that grabs one is starting a line - then a
        box, then a line within three pixels. */
    struct PatchHit
    {
        enum class What { nothing, box, inlet, outlet, line } what = What::nothing;
        std::size_t item = 0;    // a view box's index, or a view line's
        int port = 0;
    };

    PatchHit hitPatch (const PatchView& view, double x, double y);

    /*  The view boxes a rubber band from (x1, y1) to (x2, y2) touches. */
    std::vector<std::size_t> boxesTouched (const PatchView& view, double x1, double y1, double x2, double y2);

    //==========================================================================
    /*  THE EDITS: each takes the patch's text and gives the text it becomes -
        the same text when there was nothing to do. Boxes and lines are named
        by their index in Patch::boxes and Patch::lines. */

    /*  Boxes moved by (dx, dy) Pd pixels, kept on the canvas's positive side. */
    std::string patchMoved (const std::string& text, const std::vector<std::size_t>& boxes, int dx, int dy);

    /*  Boxes deleted with every line to or from them, and lines deleted; the
        lines that are left renumbered, as Pd numbers what is left. A subpatch
        goes whole. */
    std::string patchDeleted (const std::string& text, const std::vector<std::size_t>& boxes,
                              const std::vector<std::size_t>& lines);

    /*  A BOX'S WORDS TYPED AGAIN (PC.6): as a person types them, written as Pd
        writes them - a semicolon, a comma and a dollar sign escaped - its place
        and its width kept. An object or a message typed empty goes, as in Pd. */
    std::string patchTyped (const std::string& text, std::size_t box, const std::string& typed);

    /*  A BOX PLACED (PC.6) at (x, y) on the patch's own canvas: an object, a
        message, a number, a symbol or a comment, Pd's Ctrl+1 to Ctrl+5 - with
        the words typed, or none yet. An empty patch is given its canvas first.
        Its index in Patch::boxes is the last. */
    enum class Placed { object, message, number, symbol, comment };
    std::string patchPlaced (const std::string& text, Placed what, int x, int y, const std::string& typed = {});

    /*  A LINE DRAWN (PC.6) from one box's outlet to another's inlet, both on
        one canvas: the same text when it would join a box to itself, already
        exists, or names a port the boxes have not got. */
    std::string patchConnected (const PatchView& view, const std::string& text,
                                std::size_t fromView, int outlet, std::size_t toView, int inlet);

    /*  THE PICKED BOXES AS A PIECE OF PATCH (PC.6): their records, a subpatch
        whole, and the lines between them numbered from nought - what a copy
        puts on the clipboard, in Pd's own text, so it pastes into Pd too. */
    std::string patchCopied (const std::string& text, const std::vector<std::size_t>& boxes);

    /*  A PIECE OF PATCH PASTED (PC.6) onto the patch's own canvas, moved by
        (dx, dy), its lines numbered after the boxes already there. Answers the
        text and the indexes in Patch::boxes of what it put there. */
    struct Pasted
    {
        std::string text;
        std::vector<std::size_t> boxes;
    };
    Pasted patchPasted (const std::string& text, const std::string& piece, int dx, int dy);

    //==========================================================================
    /*  THE PATCH LIVE (PC.8): the run's `ports` readout - "name value..." a
        line - as a map, and what a GUI box is and says. */
    std::map<std::string, std::string> portValues (const std::string& ports);

    struct GuiBox
    {
        std::string kind;            // tgl, bng, hsl, vsl, nbx, hradio, vradio
        std::string send;            // empty when it has none
        std::string receive;
        double low = 0.0;            // a slider's or a number box's range
        double high = 127.0;
        int cells = 8;               // a radio's
        double nonzero = 1.0;        // what a toggle sends when on
    };

    std::optional<GuiBox> guiOf (const process::Patch& patch, std::size_t box);

    /*  What a hand on a GUI box sends to its receive name, from where on it the
        hand is - (fx, fy) from its top-left, nought to one each - and what the
        box shows now: a toggle flips, a bang bangs, a slider takes the place, a
        radio its cell, a number box what was dragged to (`dragged`, in its
        units). Empty for a box that sends nothing to anyone. */
    std::optional<process::Atoms> guiPress (const GuiBox& gui, double fx, double fy, std::optional<double> shown,
                                            std::optional<double> dragged = std::nullopt);
}
