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
    THE SHOW'S LISTS AS TABS, over the cue list (namespace draft §74, AQM,
    AQS). One tab a list - a show list, or a cart GO never meets - the one
    drawn below lit, the one GO acts on marked, a cart saying so in a word;
    and a + that makes either, while the show may be edited. A double click
    on a tab renames it.

    It says which tab was pressed and nothing else: which list is drawn, and
    whether a press moves GO's focus, are the window's to decide.
*/

#include <wfg/client/model/ListTabs.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class ListTabsComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** A tab pressed: this list, to be drawn. */
            std::function<void (const std::string& listId)> show;

            /** The +, and which kind was chosen under it. */
            std::function<void (bool cart)> create;

            /** A tab double-clicked: its name to be asked for. */
            std::function<void (const std::string& listId)> rename;
        };

        explicit ListTabsComponent (const model::Theme& theme);

        void setActions (Actions actionsToUse) { actions = std::move (actionsToUse); }
        void applyTheme (const model::Theme& theme);

        /*  THE TABS AS THE SHOW HAS THEM: rebuilt only when the lists change,
            relit when the list drawn or the list GO acts on does. The + only
            while the show may be edited. */
        void setTabs (const std::vector<model::ListTab>& tabs, const std::string& drawnId,
                      const std::string& goId, bool editable);

        int preferredHeight() const noexcept;

        void paint (juce::Graphics& g) override;
        void resized() override;
        void mouseDoubleClick (const juce::MouseEvent& event) override;

        /*  WHAT A TEST PRESSES: a tab by its list, and the + with its choice. */
        void pressTab (const std::string& listId);
        void pressNew (bool cart);

        /** The words a list's tab says now, empty for no such tab. */
        juce::String wordsOf (const std::string& listId) const;

    private:
        model::Theme theme;
        Actions actions;

        std::vector<model::ListTab> drawnTabs;
        std::string drawn, go;
        bool mayEdit = false;

        std::vector<std::unique_ptr<juce::TextButton>> buttons;    ///< in step with `drawnTabs`
        juce::TextButton plus { "+" };

        int rowHeight() const noexcept;
        void relight();
    };
}
