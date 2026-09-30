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
    THE NETWORK MONITOR: every OSC and MIDI message the show received or sent
    while it was open, a line each (author, 2026-09-30: "Should we add a
    network monitor similar to the one in WFS-DIY? this can be access via a
    menu item in the show menu").

    WFS-DIY's shape, kept: a window of its own that can stand on a second
    screen, a table with the time, which way, the road, the peer, the address
    and the values, filters, a pause, clear and a CSV. What differs is where
    the lines come from - the engine keeps nothing while nobody watches, and
    this window is what switches the listening on (`Actions::listen`) - and
    that the window never reads the tap itself: the client's one timer drains
    it and hands the captures here, which keeps the door count honest.

    IN AND OUT ARE WORDS AND ARROWS AS WELL AS COLOURS (§4.8), and a message
    the engine could not read says why in words, in the refusal's red.
*/

#include <wfg/client/model/Theme.h>
#include <wfg/client/model/Traffic.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace wfg::client::ui
{
    class NetworkMonitorWindow final : public juce::DocumentWindow
    {
    public:
        struct Actions
        {
            /** Whether the engine should record: on while this is open and not paused. */
            std::function<void (bool)> listen;

            /** Esc, which is PANIC in every window of the show. */
            std::function<void()> panic;
        };

        NetworkMonitorWindow (const model::Theme& theme, Actions actions);
        ~NetworkMonitorWindow() override;

        /*  WHAT ARRIVED SINCE THE LAST PASS, and how many captures the engine
            has had to drop since it started - shown as the difference since
            this window last said nothing was missed. */
        void add (const std::vector<monitor::Capture>& captures, std::uint64_t droppedSoFar);

        /** Opens it, listening; the window stays as it was left - filters, lines, pause. */
        void open();

        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress& key) override;

        /** Whether the engine should be draining into this now. */
        bool listening() const noexcept;

        /*  What it holds and shows, for a test: every line kept, the ones the
            filters let through. */
        std::size_t lineCount() const noexcept;
        std::size_t shownCount() const noexcept;
        const model::TrafficRow& shownRow (std::size_t index) const;
        void setFilter (const model::TrafficFilter& filter);

    private:
        class Content;
        std::unique_ptr<Content> content;
        Actions actions;
    };
}
