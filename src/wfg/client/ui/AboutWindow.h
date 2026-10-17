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
    ABOUT GO.DOT (namespace draft §78, ASQ): which version this is, under what
    licence, built on what, and where it lives - opened from the Help menu.

    A WINDOW OF ITS OWN AND NEVER MODAL, as the network monitor is: a modal box
    would take the keys, and Space is GO and Esc is PANIC in every window of the
    show (§4.1, §4.4). Here too Esc is PANIC.

    PRD §4.7 names About among the places where idle eyes wander; nothing hides
    here yet.
*/

#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

namespace wfg::client::ui
{
    class AboutWindow final : public juce::DocumentWindow
    {
    public:
        struct Facts
        {
            std::string version;        ///< Go.dot's, `1.2.3`
            std::string builtWith;      ///< JUCE's and Tracktion's, one line each
        };

        AboutWindow (const model::Theme& theme, Facts facts, std::function<void()> panic);
        ~AboutWindow() override;

        void open();
        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress& key) override;

    private:
        class Content;
        std::unique_ptr<Content> content;
        std::function<void()> panic;
    };
}
