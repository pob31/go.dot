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

#include <wfg/client/ui/VideoMonitorWindow.h>

#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace wfg::client::ui
{
    class VideoMonitorWindow::Content final : public juce::Component
    {
    public:
        explicit Content (const model::Theme& themeToUse) : theme (themeToUse)
        {
            setSize (820, 520);
        }

        void show (std::vector<Tile> tilesNow, const std::vector<ClientHost::CanvasPicture>& pictures)
        {
            tiles = std::move (tilesNow);

            for (const auto& picture : pictures)
            {
                auto& held = images[picture.canvasId];

                if (picture.sample == held.sample && held.image.isValid())
                    continue;

                if (! held.image.isValid() || held.image.getWidth() != picture.width || held.image.getHeight() != picture.height)
                    held.image = juce::Image (juce::Image::RGB, picture.width, picture.height, false, juce::SoftwareImageType());

                juce::Image::BitmapData pixels (held.image, juce::Image::BitmapData::writeOnly);

                for (int row = 0; row < picture.height; ++row)
                    for (int column = 0; column < picture.width; ++column)
                    {
                        const auto* rgb = picture.rgb.data() + 3 * (row * picture.width + column);
                        pixels.setPixelColour (column, row, juce::Colour (rgb[0], rgb[1], rgb[2]));
                    }

                held.sample = picture.sample;
                held.drawnAt = juce::Time::getMillisecondCounter();
            }

            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (theme, "ground"));

            if (tiles.empty())
            {
                g.setColour (Look::colour (theme, "ink-off"));
                g.setFont (Look::font (theme, 14.0f));
                g.drawFittedText ("This show has no canvas yet - Show settings, Video.", getLocalBounds().reduced (24),
                                  juce::Justification::centred, 2);
                return;
            }

            const auto count = static_cast<int> (tiles.size());
            const auto columns = std::max (1, static_cast<int> (std::ceil (std::sqrt (static_cast<double> (count)))));
            const auto rows = (count + columns - 1) / columns;
            const auto area = getLocalBounds().reduced (8);
            const auto cellWidth = area.getWidth() / columns;
            const auto cellHeight = area.getHeight() / std::max (1, rows);
            const auto now = juce::Time::getMillisecondCounter();

            for (int n = 0; n < count; ++n)
            {
                const auto& tile = tiles[static_cast<std::size_t> (n)];
                auto cell = juce::Rectangle<int> (area.getX() + (n % columns) * cellWidth,
                                                  area.getY() + (n / columns) * cellHeight,
                                                  cellWidth, cellHeight).reduced (6);

                auto caption = cell.removeFromBottom (20);
                const auto aspect = tile.canvasWidth > 0 && tile.canvasHeight > 0
                                      ? static_cast<double> (tile.canvasWidth) / tile.canvasHeight : 16.0 / 9.0;
                auto picture = cell;

                if (picture.getWidth() > picture.getHeight() * aspect)
                    picture = picture.withSizeKeepingCentre (static_cast<int> (picture.getHeight() * aspect), picture.getHeight());
                else
                    picture = picture.withSizeKeepingCentre (picture.getWidth(), static_cast<int> (picture.getWidth() / aspect));

                g.setColour (juce::Colours::black);
                g.fillRect (picture);

                const auto found = images.find (tile.canvasId);
                const auto drawn = found != images.end() && found->second.image.isValid();

                if (drawn)
                {
                    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
                    g.drawImage (found->second.image, picture.toFloat());
                }

                g.setColour (Look::colour (theme, "rule"));
                g.drawRect (picture);

                /*  WHAT AND HOW OLD, in words: a picture the renderer stopped
                    drawing - it quit, or it is busy - says so rather than
                    passing for live. */
                juce::String said = juce::String (tile.name.empty() ? tile.canvasId : tile.name)
                                    + "  " + juce::String (tile.canvasWidth) + juce::String::fromUTF8 (" \xc3\x97 ")
                                    + juce::String (tile.canvasHeight);

                if (! drawn)
                    said << "  - no picture";
                else if (now - found->second.drawnAt > 1500)
                    said << "  - not updating";

                g.setColour (Look::colour (theme, drawn ? "ink" : "ink-off"));
                g.setFont (Look::font (theme, 12.0f));
                g.drawText (said, caption, juce::Justification::centredLeft, true);
            }
        }

    private:
        struct Held
        {
            juce::Image image;
            long long sample = -1;
            juce::uint32 drawnAt = 0;
        };

        model::Theme theme;
        std::vector<Tile> tiles;
        std::map<std::string, Held> images;
    };

    //==============================================================================
    VideoMonitorWindow::VideoMonitorWindow (const model::Theme& theme, Actions actionsToUse)
        : DocumentWindow ("Video monitor", Look::colour (theme, "ground"), juce::DocumentWindow::closeButton),
          content (std::make_unique<Content> (theme)), actions (std::move (actionsToUse))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (320, 220, 4000, 3000);
        setContentNonOwned (content.get(), true);
        centreWithSize (content->getWidth(), content->getHeight());
    }

    VideoMonitorWindow::~VideoMonitorWindow()
    {
        if (actions.monitor)
            actions.monitor (false);

        clearContentComponent();
    }

    void VideoMonitorWindow::show (std::vector<Tile> tiles, const std::vector<ClientHost::CanvasPicture>& pictures)
    {
        content->show (std::move (tiles), pictures);
    }

    void VideoMonitorWindow::open()
    {
        setVisible (true);
        toFront (true);

        if (actions.monitor)
            actions.monitor (true);
    }

    bool VideoMonitorWindow::watching() const noexcept
    {
        return isVisible();
    }

    void VideoMonitorWindow::closeButtonPressed()
    {
        setVisible (false);

        if (actions.monitor)
            actions.monitor (false);
    }

    bool VideoMonitorWindow::keyPressed (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress (juce::KeyPress::escapeKey))
            return false;

        if (actions.panic)
            actions.panic();

        return true;
    }
}
