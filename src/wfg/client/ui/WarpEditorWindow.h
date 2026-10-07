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
    AN OUTPUT'S WARPS, DRAWN AND DRAGGED (namespace draft 40, WY): the author's
    "the square with the 4 corners and eventual extra vertical and horizontal
    splits allows to draw various zones in the output from the full coverage
    and map for each a different canvas", edited live - the projector follows
    while a point is dragged (his pick).

    THE PICTURE is the output, the shape of its display; over it, each warp the
    output draws: its own canvas first, then each zone, bottom first. The one
    picked is drawn bright with its control points; the others are outlines. A
    point is dragged with the mouse, or picked and moved by the arrow keys (a
    thousandth of the display, a hundredth with Shift), or typed. The splits
    are added and taken away by column and by row, and the warp keeps its shape
    as they are (`model::regridded`). "Whole output" puts a warp back.

    THE LIST beside it says each warp's canvas, blend and opacity in words; a
    zone is added on top, taken away, and its canvas, blend and opacity chosen.

    EVERY CHANGE IS A COMMAND (§3.2): a drag is a run of `node.set` on one
    address, which the document folds into one undo step; a split is one
    `node.setMany` of the grid and its points.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Video.h>
#include <wfg/engine/command/Event.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

namespace wfg::tree { class TreeSnapshot; }

namespace wfg::client::ui
{
    class WarpEditorWindow final : public juce::DocumentWindow
    {
    public:
        WarpEditorWindow (const model::Theme& theme, std::function<void (Event)> send);
        ~WarpEditorWindow() override;

        /** Opens it on one output, from the snapshot it is drawn from. */
        void open (const std::string& outputId, const tree::TreeSnapshot& snapshot);

        /** Every pass while it is open: the output's warps as the show says them now. */
        void refresh (const tree::TreeSnapshot& snapshot);

        void closeButtonPressed() override;

        class Editor;

    private:
        std::unique_ptr<Editor> editor;
    };
}
