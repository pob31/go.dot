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

#include <algorithm>
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
                 std::function<void (const std::vector<int>&, const juce::File&, const std::vector<std::string>&)> onImport,
                 std::function<void()> onCancel,
                 std::function<void (const std::vector<std::string>&, std::function<void (const ImportFiles&)>)> findFilesToUse)
            : theme (themeToUse), parent (parentToUse), findFiles (std::move (findFilesToUse))
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

            /*  THE FILES (namespace draft §100, AXX): how many were found, the
                ones that were not, and a folder to look in - as many as it
                takes, each looked through once. */
            if (findFiles)
            {
                filesSaid.setFont (Look::font (theme, 14.0f));
                filesSaid.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
                addAndMakeVisible (filesSaid);

                missingList.setMultiLine (true, false);
                missingList.setReadOnly (true);
                missingList.setScrollbarsShown (true);
                missingList.setCaretVisible (false);
                missingList.setFont (Look::font (theme, 13.0f));
                addChildComponent (missingList);

                addFolderButton.setButtonText ("Add a folder to search...");
                addFolderButton.setTooltip ("Look for the files not found in a folder of this computer, and every folder "
                                            "inside it - by their names. Add as many as it takes.");
                addFolderButton.onClick = [this] { chooseSearchFolder(); };
                addAndMakeVisible (addFolderButton);

                lookAgain();
            }

            importButton.setButtonText ("Import");
            importButton.onClick = [this, onImport]
            {
                std::vector<int> ticked;

                for (const auto& entry : boxes)
                    if (entry.box->getToggleState())
                        ticked.push_back (entry.index);

                const auto named = juce::File::createLegalFileName (nameBox.getText().trim());

                if (onImport && ! named.isEmpty())
                    onImport (ticked, parent.getChildFile (named), searchFolders);
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

            if (findFiles)
            {
                if (missingList.isVisible())
                {
                    missingList.setBounds (area.removeFromBottom (juce::jmin (110, area.getHeight() / 3)));
                    area.removeFromBottom (6);
                }

                auto files = area.removeFromBottom (28);
                addFolderButton.setBounds (files.removeFromRight (200));
                files.removeFromRight (8);
                filesSaid.setBounds (files);
                area.removeFromBottom (10);
            }

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
        /*  THE FILES LOOKED FOR AGAIN, in every folder added so far; what is
            said meanwhile, and the answer when it comes. */
        void lookAgain()
        {
            filesSaid.setText (searchFolders.empty() ? "Looking for the workspace's sounds and pictures..."
                                                     : "Looking in " + juce::String (searchFolders.size())
                                                       + (searchFolders.size() == 1 ? " folder..." : " folders..."),
                               juce::dontSendNotification);
            addFolderButton.setEnabled (false);

            findFiles (searchFolders, [safe = juce::Component::SafePointer<Content> (this)] (const ImportFiles& files)
            {
                if (safe != nullptr)
                    safe->showFiles (files);
            });
        }

        void showFiles (const ImportFiles& files)
        {
            addFolderButton.setEnabled (true);

            if (! files.error.empty())
            {
                filesSaid.setText (juce::String::fromUTF8 (files.error.c_str()), juce::dontSendNotification);
                return;
            }

            const auto missing = static_cast<int> (files.missing.size());

            if (files.total == 0)
                filesSaid.setText ("The workspace plays no file.", juce::dontSendNotification);
            else if (missing == 0)
                filesSaid.setText (files.total == 1 ? "Its one file was found." : "All " + juce::String (files.total)
                                                                                    + " of its files were found.",
                                   juce::dontSendNotification);
            else
                filesSaid.setText (juce::String (missing) + " of its " + juce::String (files.total) + " files "
                                   + (missing == 1 ? "was" : "were") + " not found - imported, they play nothing:",
                                   juce::dontSendNotification);

            filesSaid.setColour (juce::Label::textColourId, Look::colour (theme, missing > 0 ? "standby" : "ink"));

            juce::String lines;

            for (const auto& name : files.missing)
                lines << juce::String::fromUTF8 (name.c_str()) << "\n";

            for (const auto& folder : searchFolders)
                lines << "\n" << "Also looked in " << juce::String::fromUTF8 (folder.c_str());

            missingList.setText (lines.trimEnd(), false);
            missingList.setVisible (missing > 0 || ! searchFolders.empty());
            resized();
        }

        void chooseSearchFolder()
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a folder the missing files may be in", parent);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe = juce::Component::SafePointer<Content> (this)] (const juce::FileChooser& answered)
                                  {
                                      if (safe == nullptr || answered.getResult() == juce::File())
                                          return;

                                      const auto picked = answered.getResult().getFullPathName().toStdString();

                                      if (std::find (safe->searchFolders.begin(), safe->searchFolders.end(), picked)
                                            == safe->searchFolders.end())
                                          safe->searchFolders.push_back (picked);

                                      safe->lookAgain();
                                  });
        }

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

        std::function<void (const std::vector<std::string>&, std::function<void (const ImportFiles&)>)> findFiles;
        std::vector<std::string> searchFolders;
        juce::Label filesSaid;
        juce::TextEditor missingList;
        juce::TextButton addFolderButton;
    };

    //==========================================================================
    ImportWindow::ImportWindow (const model::Theme& theme, const juce::StringArray& sets, const ImportScenes& scenes,
                                const juce::File& parent, const juce::String& name, Actions actionsToUse)
        : DocumentWindow (scenes.title.empty() ? juce::String ("Import Ableton Live set") : juce::String::fromUTF8 (scenes.title.c_str()),
                          Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          actions (std::move (actionsToUse))
    {
        content = std::make_unique<Content> (theme, sets, scenes, parent, name,
                                             [this] (const std::vector<int>& ticked, const juce::File& into,
                                                     const std::vector<std::string>& folders)
                                             { if (actions.import) actions.import (ticked, into, folders); },
                                             [this] { if (actions.cancel) actions.cancel(); },
                                             actions.findFiles);
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
