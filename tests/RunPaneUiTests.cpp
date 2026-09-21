/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/client/ui/RunPaneComponent.h>

using namespace wfg::client;

TEST_CASE ("active cue errors: collapsed drawer retains failures and respects edit mode")
{
    std::string inspected;
    ui::RunPaneComponent::Actions actions;
    actions.inspectError = [&] (const std::string& id) { inspected = id; };
    ui::RunPaneComponent pane (model::Theme {}, actions);
    pane.setSize (450, 500);
    juce::TextButton* toggle = nullptr;
    juce::TextButton* clear = nullptr;
    juce::ListBox* errors = nullptr;
    for (auto* child : pane.getChildren())
    {
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            (button->getButtonText() == "Clear" ? clear : toggle) = button;
        if (auto* list = dynamic_cast<juce::ListBox*> (child)) errors = list;
    }
    REQUIRE (toggle != nullptr); REQUIRE (clear != nullptr); REQUIRE (errors != nullptr);
    CHECK_FALSE (errors->isVisible());
    CHECK_FALSE (clear->isEnabled());
    model::RunRow failed;
    failed.id = "run-1"; failed.cueId = "cue-1"; failed.cueName = "Thunder";
    failed.error = "missing-media"; failed.state = "failed";
    pane.show ({ failed }, {});
    pane.show ({ failed }, {});
    CHECK (toggle->getButtonText() == "> Errors (1)");
    CHECK_FALSE (errors->isVisible());
    toggle->setToggleState (true, juce::dontSendNotification); toggle->onClick();
    CHECK (errors->isVisible());
    CHECK (errors->getListBoxModel()->getNameForRow (0).contains ("missing-media"));
    pane.show ({}, {});
    CHECK (errors->getListBoxModel()->getNumRows() == 1);
    errors->getListBoxModel()->returnKeyPressed (0);
    CHECK (inspected == "cue-1");
    inspected.clear();
    pane.setEditing (false);
    errors->getListBoxModel()->returnKeyPressed (0);
    CHECK (inspected.empty());
    failed.id = "run-2";
    pane.show ({ failed }, {});
    REQUIRE (errors->getListBoxModel()->getNumRows() == 2);
    auto* row = errors->getComponentForRowNumber (0);
    REQUIRE (row != nullptr);
    auto* dismiss = dynamic_cast<juce::TextButton*> (row->getChildComponent (0));
    REQUIRE (dismiss != nullptr);
    dismiss->onClick();
    CHECK (inspected.empty());
    pane.show ({ failed }, {});
    CHECK (errors->getListBoxModel()->getNumRows() == 1);
    CHECK (errors->getListBoxModel()->getNameForRow (0).contains ("Thunder"));
    clear->onClick();
    pane.show ({ failed }, {});
    CHECK (errors->getListBoxModel()->getNumRows() == 0);
    CHECK_FALSE (clear->isEnabled());
    CHECK (toggle->getButtonText() == "v Errors (0)");
}
