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

#include <wfg/client/ui/ImportWindow.h>

#include <wfg/client/ui/Look.h>

#include <utility>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        constexpr int rowHeight = 26;
    }

    //==========================================================================
    class ImportWindow::Content final : public juce::Component
    {
    public:
        Content (const model::Theme& themeToUse, const juce::StringArray& sets, const ImportScenes& scenes,
                 const juce::File& parentToUse, const juce::String& name,
                 std::function<void (const std::vector<int>&, const juce::File&)> onImport,
                 std::function<void()> onCancel)
            : theme (themeToUse), parent (parentToUse)
        {
            const auto what = sets.size() == 1
                                ? "One set becomes one show: " + sets[0] + "."
                                : juce::String (sets.size()) + " sets become one show, a performance each, the newest "
                                  "the show's template. The scenes are the template's.";

            intro.setText (! scenes.intro.empty()
                             ? juce::String::fromUTF8 (scenes.intro.c_str())
                             : what + " Tick the scenes that become GOs - each becomes one, numbered in order. "
                                      "What the import cannot carry is written in a report beside the show.",
                           juce::dontSendNotification);
            intro.setFont (Look::font (theme, 15.0f));
            intro.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            addAndMakeVisible (intro);

            viewport.setViewedComponent (&rows, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            for (const auto& scene : scenes.scenes)
            {
                juce::String words;
                words << scene.index + 1 << "   " << (scene.name.empty() ? juce::String ("(no name)")
                                                                          : juce::String::fromUTF8 (scene.name.c_str()));

                if (! scene.firstLine.empty())
                    words << "  -  " << juce::String::fromUTF8 (scene.firstLine.c_str());

                if (! scene.doesSomething)
                    words << "   (does nothing)";

                auto box = std::make_unique<juce::ToggleButton> (words);
                box->setToggleState (scene.ticked, juce::dontSendNotification);
                box->setEnabled (scene.doesSomething);
                box->setColour (juce::ToggleButton::textColourId,
                                Look::colour (theme, scene.doesSomething ? "ink" : "ink-off"));
                box->setColour (juce::ToggleButton::tickColourId, Look::colour (theme, "picked"));
                rows.addAndMakeVisible (box.get());
                boxes.push_back ({ scene.index, std::move (box) });
            }

            rows.setSize (rows.getWidth(), static_cast<int> (boxes.size()) * rowHeight + 8);

            where.setText ("Into", juce::dontSendNotification);
            where.setFont (Look::font (theme, 14.0f));
            where.setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
            addAndMakeVisible (where);

            folderLabel.setFont (Look::font (theme, 14.0f));
            folderLabel.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
            addAndMakeVisible (folderLabel);
            sayWhere();

            nameBox.setText (name, false);
            nameBox.setTooltip ("The show's name: a new folder of that name is made in the folder on the left");
            addAndMakeVisible (nameBox);

            chooseButton.setButtonText ("Folder...");
            chooseButton.onClick = [this] { chooseFolder(); };
            addAndMakeVisible (chooseButton);

            importButton.setButtonText ("Import");
            importButton.onClick = [this, onImport]
            {
                std::vector<int> ticked;

                for (const auto& entry : boxes)
                    if (entry.box->getToggleState())
                        ticked.push_back (entry.index);

                const auto named = juce::File::createLegalFileName (nameBox.getText().trim());

                if (onImport && ! named.isEmpty())
                    onImport (ticked, parent.getChildFile (named));
            };
            cancelButton.setButtonText ("Cancel");
            cancelButton.onClick = [onCancel] { if (onCancel) onCancel(); };
            addAndMakeVisible (importButton);
            addAndMakeVisible (cancelButton);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (theme, "ground"));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (14);
            intro.setBounds (area.removeFromTop (64));

            auto buttons = area.removeFromBottom (32);
            cancelButton.setBounds (buttons.removeFromRight (110));
            buttons.removeFromRight (10);
            importButton.setBounds (buttons.removeFromRight (130));
            area.removeFromBottom (10);

            auto destination = area.removeFromBottom (28);
            where.setBounds (destination.removeFromLeft (44));
            chooseButton.setBounds (destination.removeFromRight (90));
            destination.removeFromRight (8);
            nameBox.setBounds (destination.removeFromRight (juce::jmin (220, destination.getWidth() / 2)));
            destination.removeFromRight (8);
            folderLabel.setBounds (destination);
            area.removeFromBottom (10);

            viewport.setBounds (area);
            rows.setSize (viewport.getMaximumVisibleWidth(), rows.getHeight());

            int y = 4;

            for (auto& entry : boxes)
            {
                entry.box->setBounds (6, y, rows.getWidth() - 10, rowHeight);
                y += rowHeight;
            }
        }

    private:
        void sayWhere()
        {
            folderLabel.setText (parent.getFullPathName() + juce::File::getSeparatorString(), juce::dontSendNotification);
        }

        void chooseFolder()
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose the folder the show is made in", parent);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe = juce::Component::SafePointer<Content> (this)] (const juce::FileChooser& answered)
                                  {
                                      if (safe == nullptr || answered.getResult() == juce::File())
                                          return;

                                      safe->parent = answered.getResult();
                                      safe->sayWhere();
                                  });
        }

        struct Entry
        {
            int index = 0;
            std::unique_ptr<juce::ToggleButton> box;
        };

        const model::Theme& theme;
        juce::File parent;
        juce::Label intro;
        juce::Viewport viewport;
        juce::Component rows;
        std::vector<Entry> boxes;
        juce::Label where, folderLabel;
        juce::TextEditor nameBox;
        juce::TextButton chooseButton, importButton, cancelButton;
        std::unique_ptr<juce::FileChooser> chooser;
    };

    //==========================================================================
    ImportWindow::ImportWindow (const model::Theme& theme, const juce::StringArray& sets, const ImportScenes& scenes,
                                const juce::File& parent, const juce::String& name, Actions actionsToUse)
        : DocumentWindow (scenes.title.empty() ? juce::String ("Import Ableton Live set") : juce::String::fromUTF8 (scenes.title.c_str()),
                          Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          actions (std::move (actionsToUse))
    {
        content = std::make_unique<Content> (theme, sets, scenes, parent, name,
                                             [this] (const std::vector<int>& ticked, const juce::File& into)
                                             { if (actions.import) actions.import (ticked, into); },
                                             [this] { if (actions.cancel) actions.cancel(); });
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (520, 360, 2400, 2000);
        setContentNonOwned (content.get(), true);
        centreWithSize (760, 620);
    }

    ImportWindow::~ImportWindow()
    {
        clearContentComponent();
    }

    void ImportWindow::closeButtonPressed()
    {
        if (actions.cancel)
            actions.cancel();
    }
}
