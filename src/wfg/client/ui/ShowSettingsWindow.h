/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <wfg/engine/audio/AudioSettings.h>
#include <wfg/engine/command/Event.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::tree { class TreeSnapshot; }
namespace wfg::client::model { struct Theme; }
namespace wfg::client::ui
{
    class ShowSettingsWindow final : public juce::DocumentWindow
    {
    public:
        ShowSettingsWindow (const model::Theme&, const tree::TreeSnapshot&,
                             std::function<void (Event)> send, std::function<void()> panic = {});
        ~ShowSettingsWindow() override;
        void refresh (const tree::TreeSnapshot&);
        void closeButtonPressed() override;
        bool keyPressed (const juce::KeyPress&) override;
    private:
        class Panel;
        std::unique_ptr<Panel> panel;
        std::function<void()> panic;
    };
}
