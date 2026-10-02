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

#include <wfg/client/ui/TemplateReviewWindow.h>

#include <wfg/client/ui/Look.h>

#include <utility>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int rowHeight = 26;
        constexpr int indent = 28;

        const char* headingFor (TemplateChange::Kind kind)
        {
            switch (kind)
            {
                case TemplateChange::Kind::added:    return "Added in this performance";
                case TemplateChange::Kind::changed:  return "Changed in this performance";
                case TemplateChange::Kind::removed:  return "Deleted in this performance";
                case TemplateChange::Kind::settings: return "Show settings";
            }

            return "";
        }
    }

    //==========================================================================
    class TemplateReviewWindow::Content final : public juce::Component
    {
    public:
        Content (const model::Theme& themeToUse, model::TemplateReview reviewToShow, const juce::String& showName,
                 std::function<void (const model::TemplateReview&)> onUpdate, std::function<void()> onCancel)
            : theme (themeToUse), review (std::move (reviewToShow))
        {
            intro.setText ("This performance differs from " + showName + "'s template. Tick what to bring "
                           "into the template - every performance made from it afterwards starts with it.",
                           juce::dontSendNotification);
            intro.setFont (Look::font (theme, 15.0f));
            intro.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            addAndMakeVisible (intro);

            viewport.setViewedComponent (&rows, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            build();

            updateButton.setButtonText ("Update the template");
            updateButton.onClick = [this, onUpdate] { if (onUpdate) onUpdate (review); };
            cancelButton.setButtonText ("Cancel");
            cancelButton.onClick = [onCancel] { if (onCancel) onCancel(); };
            addAndMakeVisible (updateButton);
            addAndMakeVisible (cancelButton);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (theme, "ground"));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (14);
            intro.setBounds (area.removeFromTop (44));
            auto buttons = area.removeFromBottom (32);
            cancelButton.setBounds (buttons.removeFromRight (110));
            buttons.removeFromRight (10);
            updateButton.setBounds (buttons.removeFromRight (180));
            area.removeFromBottom (10);
            viewport.setBounds (area);
            rows.setSize (viewport.getMaximumVisibleWidth(), rows.getHeight());
            layoutRows();
        }

    private:
        //  Group by kind, in the order the window reads best: added, changed, removed, settings.
        void build()
        {
            const auto& changes = review.changes();

            for (const auto kind : { TemplateChange::Kind::added, TemplateChange::Kind::changed,
                                     TemplateChange::Kind::removed, TemplateChange::Kind::settings })
            {
                bool any = false;

                for (std::size_t i = 0; i < changes.size(); ++i)
                {
                    if (changes[i].kind != kind)
                        continue;

                    if (! any)
                    {
                        auto heading = std::make_unique<juce::Label>();
                        heading->setText (headingFor (kind), juce::dontSendNotification);
                        heading->setFont (Look::font (theme, 15.0f).boldened());
                        heading->setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
                        lines.push_back ({ std::move (heading), nullptr, 0 });
                        any = true;
                    }

                    juce::String words = changes[i].label;

                    if (! changes[i].localSounds.empty())
                        words << "   (a sound only this performance has)";

                    addTick (words, 1, review.isTicked (i), [this, i] (bool on) { review.tick (i, on); refresh(); });

                    for (std::size_t f = 0; f < changes[i].fields.size(); ++f)
                        addTick (changes[i].fields[f].label, 2, review.isFieldTicked (i, f),
                                 [this, i, f] (bool on) { review.tickField (i, f, on); refresh(); });
                }
            }

            rows.setSize (rows.getWidth(), static_cast<int> (lines.size()) * rowHeight + 8);
        }

        void addTick (const juce::String& words, int depth, bool ticked, std::function<void (bool)> changed)
        {
            auto box = std::make_unique<juce::ToggleButton> (words);
            box->setToggleState (ticked, juce::dontSendNotification);
            box->setColour (juce::ToggleButton::textColourId, Look::colour (theme, "ink"));
            box->setColour (juce::ToggleButton::tickColourId, Look::colour (theme, "picked"));
            auto* raw = box.get();
            box->onClick = [raw, changed] { changed (raw->getToggleState()); };
            lines.push_back ({ nullptr, std::move (box), depth });
        }

        //  A cue's tick follows its fields and the fields their cue: redraw every box from the model.
        void refresh()
        {
            const auto& changes = review.changes();
            std::size_t line = 0;

            for (const auto kind : { TemplateChange::Kind::added, TemplateChange::Kind::changed,
                                     TemplateChange::Kind::removed, TemplateChange::Kind::settings })
            {
                bool any = false;

                for (std::size_t i = 0; i < changes.size(); ++i)
                {
                    if (changes[i].kind != kind)
                        continue;

                    if (! any) { ++line; any = true; }

                    lines[line++].box->setToggleState (review.isTicked (i), juce::dontSendNotification);

                    for (std::size_t f = 0; f < changes[i].fields.size(); ++f)
                        lines[line++].box->setToggleState (review.isFieldTicked (i, f), juce::dontSendNotification);
                }
            }
        }

        void layoutRows()
        {
            int y = 4;

            for (auto& entry : lines)
            {
                juce::Component* item = entry.heading != nullptr ? static_cast<juce::Component*> (entry.heading.get())
                                                                 : entry.box.get();
                rows.addAndMakeVisible (item);
                const auto x = entry.depth > 0 ? (entry.depth - 1) * indent + 6 : 0;
                item->setBounds (x, y, rows.getWidth() - x - 4, rowHeight);
                y += rowHeight;
            }
        }

        struct Line
        {
            std::unique_ptr<juce::Label> heading;
            std::unique_ptr<juce::ToggleButton> box;
            int depth = 0;
        };

        const model::Theme& theme;
        model::TemplateReview review;
        juce::Label intro;
        juce::Viewport viewport;
        juce::Component rows;
        std::vector<Line> lines;
        juce::TextButton updateButton, cancelButton;
    };

    //==========================================================================
    TemplateReviewWindow::TemplateReviewWindow (const model::Theme& theme, model::TemplateReview review,
                                                const juce::String& showName, Actions actionsToUse)
        : DocumentWindow ("Update the show's template", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          actions (std::move (actionsToUse))
    {
        content = std::make_unique<Content> (theme, std::move (review), showName,
                                             [this] (const model::TemplateReview& ticked) { if (actions.update) actions.update (ticked); },
                                             [this] { if (actions.cancel) actions.cancel(); });
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (480, 320, 2000, 2000);
        setContentNonOwned (content.get(), true);
        centreWithSize (640, 520);
    }

    TemplateReviewWindow::~TemplateReviewWindow()
    {
        clearContentComponent();
    }

    void TemplateReviewWindow::closeButtonPressed()
    {
        if (actions.cancel)
            actions.cancel();
    }
}
