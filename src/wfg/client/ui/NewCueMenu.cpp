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

#include <wfg/client/ui/NewCueMenu.h>

#include <utility>

namespace wfg::client::ui
{
    /*  ONE IDENTIFIER PER (choice, wrap), never nought - which is JUCE's
        answer for "closed without a click". */
    namespace
    {
        int menuItemIdFor (int choice, bool wrap) noexcept
        {
            return 1 + choice * 2 + (wrap ? 1 : 0);
        }
    }

    std::pair<int, bool> choiceOfMenuItem (int menuItemId)
    {
        if (menuItemId <= 0)
            return { -1, false };

        return { (menuItemId - 1) / 2, ((menuItemId - 1) % 2) == 1 };
    }

    juce::PopupMenu newCueMenu (const std::vector<model::MenuLine>& lines)
    {
        juce::PopupMenu menu;

        for (const auto& line : lines)
        {
            switch (line.kind)
            {
                case model::MenuLine::Kind::header:
                    menu.addSectionHeader (juce::String (line.text));
                    break;

                case model::MenuLine::Kind::item:
                    menu.addItem (menuItemIdFor (line.choice, line.wrap), juce::String (line.text));
                    break;

                /*  A SENTENCE, NOT A CHOICE: greyed, and never answered, which
                    a disabled item with an identifier nobody reads is. */
                case model::MenuLine::Kind::note:
                    menu.addItem (-1, juce::String (line.text), false);
                    break;

                case model::MenuLine::Kind::separator:
                    menu.addSeparator();
                    break;
            }
        }

        return menu;
    }

    void showNewCueMenu (juce::Component& anchor, const std::vector<model::MenuLine>& lines,
                         std::function<void (int choice, bool wrap)> chosen)
    {
        newCueMenu (lines).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor),
                                          [safe = juce::Component::SafePointer<juce::Component> (&anchor),
                                           answer = std::move (chosen)] (int menuItemId)
                                          {
                                              const auto [choice, wrap] = choiceOfMenuItem (menuItemId);

                                              if (safe == nullptr || choice < 0 || ! answer)
                                                  return;

                                              answer (choice, wrap);
                                          });
    }
}
