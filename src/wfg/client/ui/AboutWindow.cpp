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

#include <wfg/client/ui/AboutWindow.h>
#include <wfg/client/ui/Look.h>

#include <utility>

namespace wfg::client::ui
{
    class AboutWindow::Content final : public juce::Component
    {
    public:
        Content (const model::Theme& theme, const Facts& facts)
        {
            const auto type = static_cast<float> (theme.type);

            auto dress = [&] (juce::Label& label, const juce::String& words, float height, const char* ink)
            {
                label.setText (words, juce::dontSendNotification);
                label.setFont (Look::font (theme, height * type));
                label.setColour (juce::Label::textColourId, Look::colour (theme, ink));
                label.setJustificationType (juce::Justification::centredLeft);
                addAndMakeVisible (label);
            };

            dress (name, "Go.dot " + juce::String::fromUTF8 (facts.version.c_str()), 22.0f, "ink");
            dress (licence, "Free software under the GNU General Public License, version 3 or later.", 13.0f, "ink");
            dress (built, juce::String::fromUTF8 (facts.builtWith.c_str()), 12.0f, "ink-faint");
            built.setJustificationType (juce::Justification::topLeft);

            site.setButtonText ("github.com/pob31/go.dot");
            site.setURL (juce::URL ("https://github.com/pob31/go.dot"));
            site.setFont (Look::font (theme, 13.0f * type), false, juce::Justification::centredLeft);
            site.setColour (juce::HyperlinkButton::textColourId, Look::colour (theme, "picked"));
            addAndMakeVisible (site);

            setSize (juce::roundToInt (460 * type), juce::roundToInt (200 * type));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (getWidth() / 20);
            const auto line = area.getHeight() / 7;

            name.setBounds (area.removeFromTop (line * 2));
            licence.setBounds (area.removeFromTop (line));
            site.setBounds (area.removeFromTop (line));
            area.removeFromTop (line / 2);
            built.setBounds (area);
        }

    private:
        juce::Label name, licence, built;
        juce::HyperlinkButton site;
    };

    AboutWindow::AboutWindow (const model::Theme& theme, Facts facts, std::function<void()> panicToUse)
        : DocumentWindow ("About Go.dot", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          panic (std::move (panicToUse))
    {
        content = std::make_unique<Content> (theme, facts);
        setUsingNativeTitleBar (true);
        setResizable (false, false);
        setContentNonOwned (content.get(), true);
        centreWithSize (content->getWidth(), content->getHeight());
    }

    AboutWindow::~AboutWindow()
    {
        clearContentComponent();
    }

    void AboutWindow::open()
    {
        setVisible (true);
        toFront (true);
    }

    void AboutWindow::closeButtonPressed()
    {
        setVisible (false);
    }

    bool AboutWindow::keyPressed (const juce::KeyPress& key)
    {
        //  ESC IS PANIC HERE TOO, whichever window has the focus (§4.4).
        if (key != juce::KeyPress (juce::KeyPress::escapeKey))
            return false;

        if (panic)
            panic();

        return true;
    }
}
