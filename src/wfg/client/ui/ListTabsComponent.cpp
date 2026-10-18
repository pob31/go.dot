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

#include <wfg/client/ui/ListTabsComponent.h>

#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <utility>

namespace wfg::client::ui
{
    ListTabsComponent::ListTabsComponent (const model::Theme& themeToUse)
        : theme (themeToUse)
    {
        plus.setWantsKeyboardFocus (false);
        plus.setTooltip ("A new cue list, or a cart - a list GO never meets, its cues fired by a key, a pad or a"
                         " message");
        plus.onClick = [this]
        {
            juce::PopupMenu menu;
            menu.addItem (1, "New cue list");
            menu.addItem (2, "New cart");

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&plus),
                                [safe = juce::Component::SafePointer<ListTabsComponent> (this)] (int chosen)
                                {
                                    if (safe != nullptr && chosen > 0)
                                        safe->pressNew (chosen == 2);
                                });
        };

        addChildComponent (plus);
        flattenPlus();
    }

    void ListTabsComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        flattenPlus();
        resized();
        repaint();
    }

    //  The + is a word beside the tabs, as they are: no fill, no outline, the tabs' grey.
    void ListTabsComponent::flattenPlus()
    {
        plus.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        plus.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        plus.setColour (juce::TextButton::textColourOffId, Look::colour (theme, "ink-dim"));
    }

    int ListTabsComponent::rowHeight() const noexcept
    {
        return juce::roundToInt (theme.row * theme.type);
    }

    int ListTabsComponent::preferredHeight() const noexcept
    {
        return rowHeight() + rowHeight() / 4;
    }

    void ListTabsComponent::setTabs (const std::vector<model::ListTab>& tabs, const std::string& drawnId,
                                     const std::string& goId, bool editable)
    {
        const auto changed = tabs != drawnTabs;
        const auto relit = drawnId != drawn || goId != go;
        const auto editChanged = editable != mayEdit;

        drawn = drawnId;
        go = goId;
        mayEdit = editable;

        if (changed)
        {
            drawnTabs = tabs;

            for (auto& button : buttons)
                removeChildComponent (button.get());

            buttons.clear();

            for (const auto& tab : drawnTabs)
            {
                auto button = std::make_unique<Tab> (theme);
                button->setWantsKeyboardFocus (false);
                button->setClickingTogglesState (false);
                button->setTooltip (tab.cart ? "A cart: GO never meets it. Double click to rename."
                                             : "A cue list. Pressed, GO acts on it. Double click to rename.");

                const auto id = tab.id;
                button->onClick = [this, id] { pressTab (id); };
                button->addMouseListener (this, false);

                addAndMakeVisible (*button);
                buttons.push_back (std::move (button));
            }
        }

        if (changed || relit)
            relight();

        plus.setVisible (mayEdit);

        if (changed || editChanged)
            resized();
    }

    void ListTabsComponent::relight()
    {
        for (std::size_t at = 0; at < buttons.size() && at < drawnTabs.size(); ++at)
        {
            const auto& tab = drawnTabs[at];
            buttons[at]->setButtonText (juce::String (juce::CharPointer_UTF8 (model::tabWords (tab, tab.id == go).c_str())));
            buttons[at]->setToggleState (tab.id == drawn, juce::dontSendNotification);
        }
    }

    juce::String ListTabsComponent::wordsOf (const std::string& listId) const
    {
        for (std::size_t at = 0; at < buttons.size() && at < drawnTabs.size(); ++at)
            if (drawnTabs[at].id == listId)
                return buttons[at]->getButtonText();

        return {};
    }

    void ListTabsComponent::pressTab (const std::string& listId)
    {
        if (actions.show)
            actions.show (listId);
    }

    void ListTabsComponent::pressNew (bool cart)
    {
        if (actions.create)
            actions.create (cart);
    }

    void ListTabsComponent::mouseDoubleClick (const juce::MouseEvent& event)
    {
        for (std::size_t at = 0; at < buttons.size() && at < drawnTabs.size(); ++at)
            if (event.eventComponent == buttons[at].get())
            {
                if (actions.rename && mayEdit)
                    actions.rename (drawnTabs[at].id);

                return;
            }
    }

    juce::PopupMenu ListTabsComponent::menuFor (const std::string& listId)
    {
        juce::PopupMenu menu;
        const auto found = std::find_if (drawnTabs.begin(), drawnTabs.end(),
                                         [&listId] (const model::ListTab& tab) { return tab.id == listId; });

        if (found == drawnTabs.end())
            return menu;

        const auto cart = found->cart;

        menu.addItem ("Rename...", mayEdit, false, [this, listId] { if (actions.rename) actions.rename (listId); });

        /*  NEVER THE LAST CUE LIST, and said so in the line itself rather than
            only greyed (PRD §4.8): a show keeps one for GO. */
        const auto removable = mayEdit && model::mayRemoveList (drawnTabs, listId);
        menu.addItem (cart ? "Remove this cart"
                           : removable || ! mayEdit ? "Remove this cue list"
                                                    : "Remove this cue list - the show keeps one for GO",
                      removable, false, [this, listId] { if (actions.remove) actions.remove (listId); });
        return menu;
    }

    void ListTabsComponent::mouseUp (const juce::MouseEvent& event)
    {
        if (! event.mods.isPopupMenu())
            return;

        for (std::size_t at = 0; at < buttons.size() && at < drawnTabs.size(); ++at)
            if (event.eventComponent == buttons[at].get())
            {
                menuFor (drawnTabs[at].id).showMenuAsync (juce::PopupMenu::Options().withMousePosition());
                return;
            }
    }

    void ListTabsComponent::Tab::paintButton (juce::Graphics& g, bool over, bool down)
    {
        const auto lit = getToggleState();
        const auto yellow = Look::colour (look, "go");
        const auto ink = lit ? yellow : Look::colour (look, over || down ? "ink" : "ink-dim");
        const auto font = Look::font (look, 13.0f);

        auto area = getLocalBounds();

        if (lit)
        {
            const auto bar = juce::jmax (2, juce::roundToInt (2.0 * look.type));
            g.setColour (yellow);
            g.fillRect (area.removeFromBottom (bar).reduced (juce::roundToInt (6.0 * look.type), 0));
        }

        g.setColour (ink);
        g.setFont (lit ? font.boldened() : font);
        g.drawFittedText (getButtonText(), area.reduced (4, 0), juce::Justification::centred, 1, 0.8f);
    }

    void ListTabsComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));
        g.setColour (Look::colour (theme, "rule"));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);
    }

    void ListTabsComponent::resized()
    {
        auto area = getLocalBounds().reduced (2, 1);
        const auto row = rowHeight();

        if (plus.isVisible())
        {
            plus.setBounds (area.removeFromRight (row + row / 2));
            area.removeFromRight (2);
        }

        /*  EACH TAB AS WIDE AS ITS WORDS, up to an even share of the row, so a
            show of two lists reads as two tabs and one of twelve still fits. */
        const auto count = static_cast<int> (buttons.size());

        if (count == 0)
            return;

        const auto share = area.getWidth() / count;
        const auto font = Look::font (theme, 13.0f);

        for (auto& button : buttons)
        {
            const auto wanted = juce::GlyphArrangement::getStringWidthInt (font, button->getButtonText()) + row;
            button->setBounds (area.removeFromLeft (juce::jmin (share, juce::jmax (row * 3, wanted))));
            area.removeFromLeft (2);
        }
    }
}
