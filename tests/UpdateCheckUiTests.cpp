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

/*
    THE ASK FOR A NEWER GO.DOT, READ FROM A FILE (namespace draft §78): what
    ui/UpdateCheck does with what it is pointed at. How an answer is read is the
    model's, tested on every system in ClientTests; this is the reading itself,
    which lives with the window and so with the window's tests. No network: a
    `file:` URL is read as a file, which is what `--update-url` gives a test.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/client/model/Updates.h>
#include <wfg/client/model/Problems.h>
#include <wfg/client/model/SettingsPages.h>
#include <wfg/client/ui/TransportComponent.h>
#include <wfg/client/ui/UpdateCheck.h>

#include <juce_core/juce_core.h>

#include <string>

using namespace wfg::client;

namespace
{
    std::string urlOf (const juce::File& file)
    {
        return juce::URL (file).toString (false).toStdString();
    }
}

TEST_CASE ("updates: an ask reads what it is pointed at, and what is not there is no answer")
{
    const juce::TemporaryFile written (".json");
    REQUIRE (written.getFile().replaceWithText ("{\"tag_name\": \"v0.1.2\", "
                                                "\"html_url\": \"https://github.com/pob31/go.dot/releases/tag/v0.1.2\"}"));

    const auto found = ui::UpdateCheck::askNow (urlOf (written.getFile()), "0.1.1");
    CHECK (found.reached);
    REQUIRE (found.release.has_value());
    CHECK (model::updates::versionText (found.release->version) == "0.1.2");

    //  A file that is not there is no answer, so nothing found before is forgotten.
    const auto gone = written.getFile().getSiblingFile ("not-there-"
                                                          + juce::String (juce::Random::getSystemRandom().nextInt64())
                                                          + ".json");
    const auto missing = ui::UpdateCheck::askNow (urlOf (gone), "0.1.1");
    CHECK (! missing.reached);
    CHECK (! missing.release.has_value());
}

/*  THE MARK ON THE TITLE LINE (ASQ): there while a newer one is out, its version
    in words, left of the warnings so that what is wrong stays nearest the clock,
    a press its own and nothing else's, and gone when it is taken away. A picture
    of the line goes to WFG_SNAPSHOT_DIR when it is set, for the author to look at. */
TEST_CASE ("updates: the mark sits on the title line left of the warnings, says its version, and a press is its own")
{
    auto goes = 0, panics = 0, pressed = 0;

    ui::TransportComponent::Actions actions;
    actions.go = [&goes] { ++goes; };
    actions.panic = [&panics] { ++panics; };

    const model::Theme theme;
    ui::TransportComponent transport (theme, actions);
    transport.setSize (1200, transport.preferredHeight());

    const auto child = [&transport] (const juce::String& id) -> juce::Component*
    {
        for (auto* c : transport.getChildren())
            if (c->getComponentID() == id)
                return c;

        return nullptr;
    };

    CHECK (child ("update") == nullptr);

    model::PageProblems network;
    network.page = model::settingsPages::network;
    network.icon = model::iconForPage (network.page);
    network.sentences = { "Desk: no answer at 10.0.0.12:10023" };
    transport.showProblems ({ network });
    transport.showUpdate ("Go.dot 0.1.2 is out", [&pressed] { ++pressed; });

    auto* mark = dynamic_cast<juce::Button*> (child ("update"));
    auto* warning = child ("problem:" + juce::String (model::settingsPages::network));
    REQUIRE (mark != nullptr);
    REQUIRE (warning != nullptr);

    CHECK (mark->getButtonText() == "Go.dot 0.1.2 is out");
    CHECK (mark->getTooltip().startsWith ("Go.dot 0.1.2 is out"));
    CHECK (mark->isVisible());
    CHECK (mark->getWidth() > 0);
    CHECK (mark->getRight() <= warning->getX());
    CHECK (mark->getBounds().getCentreY() == warning->getBounds().getCentreY());

    if (mark->onClick)
        mark->onClick();

    CHECK (pressed == 1);
    CHECK (goes == 0);
    CHECK (panics == 0);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("WFG_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        juce::Image canvas (juce::Image::ARGB, transport.getWidth(), transport.getHeight(), true,
                            juce::SoftwareImageType());
        juce::Graphics g (canvas);
        transport.paintEntireComponent (g, false);

        const juce::File file { juce::File (dir).getChildFile ("title-line-update.png") };
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        juce::FileOutputStream out { file };
        REQUIRE (out.openedOk());
        CHECK (juce::PNGImageFormat().writeImageToStream (canvas, out));
        MESSAGE ("wrote " << file.getFullPathName().toStdString());
    }

    //  Taken away, it is gone, and the warning stays where it was.
    const auto warningAt = warning->getBounds();
    transport.showUpdate ({}, {});
    CHECK (child ("update") == nullptr);
    CHECK (warning->getBounds() == warningAt);
}
