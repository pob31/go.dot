/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/command/Event.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <string>
#include <vector>

namespace wfg::tree { class TreeSnapshot; }
namespace wfg::client::model { struct Theme; struct PageProblems; }
namespace wfg::client::ui
{
    /*  KEPT ON TOP OF THE SHOW'S WINDOW (author, 2026-09-30: "selecting the
        cue list window can easily be on top when selecting. This defeats the
        purpose. Can we keep the settings window on top?"). On top only while
        Go.dot is the application in front, so it never covers another
        program; the dialogs it opens - a folder to scan, a preset - go on top
        of it, JUCE seeing to that. Linux cannot change a window's level in
        place (JUCE would rebuild the window), so there it is on top while open. */
    class ShowSettingsWindow final : public juce::DocumentWindow,
                                     private juce::Timer
    {
    public:
        ShowSettingsWindow (const model::Theme&, const tree::TreeSnapshot&,
                             std::function<void (Event)> send, std::function<void()> panic = {});
        ~ShowSettingsWindow() override;
        void refresh (const tree::TreeSnapshot&);

        /** Shows the page of that name (model/SettingsPages.h); a name it has not is ignored. */
        void showPage (const std::string& name);

        /** Marks the pages, and their sections, that have a problem (model/Problems.h, namespace draft §64). */
        void showProblems (const std::vector<model::PageProblems>& pages);
        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress&) override;
    private:
        void timerCallback() override;
        class Panel;
        std::unique_ptr<Panel> panel;
        std::function<void()> panic;
    };
}
