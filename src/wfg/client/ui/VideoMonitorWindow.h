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
    THE VIDEO MONITOR: every canvas as it is now, a tile each (author,
    2026-10-07: "Can we have a video monitor window for the canvases? This
    would be opened by an item in the show menu"; ten frames a second, his
    pick). The renderer draws each canvas small on its own CPU while this is
    open and the client's timer hands the pictures here - the fifth door
    (Console.h, `canvasPictures`) - so the window costs the show nothing while
    it is shut, and nothing on the graphics card while it is open.

    WHAT IS UP, NOT HOW GOOD IT LOOKS: the tiles are at most 256 by 144, through
    the reference compositor, before any output's warp or calibration. Each says
    its canvas's name, its size and how long ago its picture was drawn, in words
    (§4.8) - "no picture" when the renderer has drawn none.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/engine/Console.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class VideoMonitorWindow final : public juce::DocumentWindow
    {
    public:
        struct Actions
        {
            /** Whether the renderer should draw the canvases: on while this is open. */
            std::function<void (bool)> monitor;

            /** Esc, which is PANIC in every window of the show. */
            std::function<void()> panic;
        };

        struct Tile
        {
            std::string canvasId;
            std::string name;
            int canvasWidth = 0, canvasHeight = 0;
        };

        VideoMonitorWindow (const model::Theme& theme, Actions actions);
        ~VideoMonitorWindow() override;

        /*  ONE PASS: the canvases the show declares, by name, and the latest
            pictures the renderer drew of them. */
        void show (std::vector<Tile> tiles, const std::vector<ClientHost::CanvasPicture>& pictures);

        void open();
        bool watching() const noexcept;

        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        class Content;
        std::unique_ptr<Content> content;
        Actions actions;
    };
}
