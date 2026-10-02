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
    WHAT GOES BACK INTO THE SHOW'S TEMPLATE, ticked by hand (namespace draft
    §25): the performance's changes in four groups - added, changed, removed,
    show settings - each with a tick, and under a changed cue each field that
    differs with its own. The ticks are model::TemplateReview's; this draws
    them and hands them back.

    Its own window, as the network monitor is, so the cue list stays in view
    beside it.
*/

#include <wfg/client/model/TemplateReview.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace wfg::client::ui
{
    class TemplateReviewWindow final : public juce::DocumentWindow
    {
    public:
        struct Actions
        {
            /** "Update the template": the ticks as they stand. */
            std::function<void (const model::TemplateReview&)> update;

            /** Cancel, or the close button: nothing is brought back. */
            std::function<void()> cancel;
        };

        TemplateReviewWindow (const model::Theme& theme, model::TemplateReview review,
                              const juce::String& showName, Actions actions);
        ~TemplateReviewWindow() override;

        void closeButtonPressed() override;

    private:
        class Content;
        std::unique_ptr<Content> content;
        Actions actions;
    };
}
