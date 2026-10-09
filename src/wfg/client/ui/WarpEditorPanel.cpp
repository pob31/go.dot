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

#include <wfg/client/ui/WarpEditorPanel.h>

#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>
#include <wfg/engine/video/Mapping.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        /*  ONE WARP THE OUTPUT DRAWS: its own canvas's (the first, `zoneId`
            empty) or a zone's - where its rows live, and what the list says. */
        struct Layer
        {
            std::string zoneId;
            std::string base;
            std::string canvas;
            std::string name;
            std::string blend = "normal";
            double opacity = 100.0;
            model::WarpPoints warp;
        };

        video::Mesh meshOf (const model::WarpPoints& warp)
        {
            video::Mesh mesh;
            mesh.columns = warp.columns;
            mesh.rows = warp.rows;

            for (std::size_t n = 0; n < warp.x.size(); ++n)
            {
                mesh.x.push_back (static_cast<float> (warp.x[n]));
                mesh.y.push_back (static_cast<float> (warp.y[n]));
            }

            return mesh;
        }

        const char* const blends[] { "normal", "add", "screen", "multiply" };
    }

    //==============================================================================
    class WarpEditorPanel::Editor final : public juce::Component
    {
    public:
        Editor (const model::Theme& themeToUse, std::function<void (Event)> dispatch)
            : theme (themeToUse), send (std::move (dispatch))
        {
            setWantsKeyboardFocus (true);

            for (auto* button : { &addZone, &removeZone, &moreColumns, &fewerColumns, &moreRows, &fewerRows, &whole, &copyTo })
            {
                button->setWantsKeyboardFocus (false);
                addAndMakeVisible (*button);
            }

            for (auto* box : { &canvasBox, &blendBox })
            {
                box->setWantsKeyboardFocus (false);
                addAndMakeVisible (*box);
            }

            for (auto* label : { &opacityBox, &pointX, &pointY })
            {
                label->setEditable (true, true, false);
                label->setColour (juce::Label::backgroundColourId, Look::colour (theme, "panel-in"));
                label->setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
                label->setJustificationType (juce::Justification::centred);
                addAndMakeVisible (*label);
            }

            for (auto* label : { &canvasLabel, &blendLabel, &opacityLabel, &pointLabel, &gridLabel, &hint })
            {
                label->setColour (juce::Label::textColourId, Look::colour (theme, "ink-dim"));
                label->setFont (Look::font (theme, 12.0f));
                addAndMakeVisible (*label);
            }

            canvasLabel.setText ("Canvas", juce::dontSendNotification);
            blendLabel.setText ("Blend", juce::dontSendNotification);
            opacityLabel.setText ("Opacity %", juce::dontSendNotification);
            pointLabel.setText ("Point x, y", juce::dontSendNotification);
            hint.setJustificationType (juce::Justification::topLeft);
            hint.setText ("Drag a point, or click one and move it with the arrow keys (Shift: ten times as far)."
                          " The projector follows.", juce::dontSendNotification);

            addZone.setTooltip ("A further canvas on this output, through a warp of its own, over what it shows");
            removeZone.setTooltip ("Take the picked zone away");
            moreColumns.setTooltip ("One more column of points: the warp keeps its shape");
            fewerColumns.setTooltip ("One column of points fewer");
            moreRows.setTooltip ("One more row of points");
            fewerRows.setTooltip ("One row of points fewer");
            whole.setTooltip ("Put the picked warp back over the whole output");
            copyTo.setTooltip ("Put the picked warp's shape - its points and its splits - on another warp of this"
                               " output or another; that warp keeps its canvas, blend and opacity");
            copyTo.onClick = [this] { chooseCopyTarget(); };

            for (int n = 0; n < 4; ++n)
                blendBox.addItem (blends[n], n + 1);

            addZone.onClick = [this] { if (! locked && send) send (gesture::createZone (outputId, firstCanvas())); };
            removeZone.onClick = [this]
            {
                if (const auto* layer = picked(); layer != nullptr && ! layer->zoneId.empty() && ! locked && send)
                    send (gesture::deleteObject (layer->zoneId));
            };

            moreColumns.onClick = [this] { regrid (1, 0); };
            fewerColumns.onClick = [this] { regrid (-1, 0); };
            moreRows.onClick = [this] { regrid (0, 1); };
            fewerRows.onClick = [this] { regrid (0, -1); };
            whole.onClick = [this]
            {
                if (const auto* layer = picked(); layer != nullptr)
                    writeWarp (model::WarpPoints::whole (layer->warp.columns, layer->warp.rows), true);
            };

            canvasBox.onChange = [this]
            {
                const auto* layer = picked();
                const auto at = canvasBox.getSelectedItemIndex();

                if (layer == nullptr || locked || ! send || at < 0 || static_cast<std::size_t> (at) >= canvasKeys.size())
                    return;

                if (canvasKeys[static_cast<std::size_t> (at)] != layer->canvas)
                    send (gesture::setNode (layer->base + "canvas", canvasKeys[static_cast<std::size_t> (at)]));
            };

            blendBox.onChange = [this]
            {
                const auto* layer = picked();
                const auto at = blendBox.getSelectedId() - 1;

                if (layer == nullptr || layer->zoneId.empty() || locked || ! send || at < 0 || at > 3)
                    return;

                if (layer->blend != blends[at])
                    send (gesture::setNode (layer->base + "blend", blends[at]));
            };

            opacityBox.onTextChange = [this]
            {
                const auto* layer = picked();

                if (layer == nullptr || layer->zoneId.empty() || locked || ! send)
                    return;

                if (const auto value = osc::parseDouble (opacityBox.getText().trim().toStdString()); value.has_value())
                    send (gesture::setNode (layer->base + "opacity", osc::formatDouble (std::clamp (*value, 0.0, 100.0))));
            };

            pointX.onTextChange = [this] { typedPoint(); };
            pointY.onTextChange = [this] { typedPoint(); };

            setSize (980, 600);
        }

        void open (const std::string& output, const tree::TreeSnapshot& snapshot)
        {
            /*  EACH OUTPUT'S PICK KEPT (§47.12): going to another output and
                back finds the warp that was being worked on. */
            if (output != outputId)
            {
                if (! outputId.empty())
                    pickedFor[outputId] = pickedLayer;

                outputId = output;
                const auto found = pickedFor.find (output);
                pickedLayer = found != pickedFor.end() ? found->second : 0;
                pickedPoint.reset();
                dragging = false;
            }

            refresh (snapshot);
        }

        const std::string& output() const noexcept { return outputId; }

        void refresh (const tree::TreeSnapshot& snapshot)
        {
            locked = model::isYes (model::flag (snapshot, "/godot/document/locked"));

            const auto base = "/godot/videoOutput/" + outputId + "/";
            outputName = model::text (snapshot, base + "name");

            std::vector<Layer> now;
            Layer own;
            own.base = base;
            own.canvas = model::text (snapshot, base + "canvas");
            own.warp = model::readWarp (snapshot, base);
            now.push_back (std::move (own));

            for (auto& zone : model::readZones (snapshot, outputId))
            {
                Layer layer;
                layer.zoneId = zone.id;
                layer.base = "/godot/zone/" + zone.id + "/";
                layer.canvas = zone.canvas;
                layer.name = zone.name;
                layer.blend = zone.blend;
                layer.opacity = zone.opacity;
                layer.warp = zone.warp;
                now.push_back (std::move (layer));
            }

            canvases = model::readCanvases (snapshot);

            //  Every warp of the show, for "Copy to...".
            targets = model::warpTargets (snapshot);

            /*  A DRAG IN HAND IS NOT UNDONE BY THE PICTURE CATCHING UP: the
                warp being dragged keeps the window's own points until the
                mouse lets go and the show has them. */
            if (dragging && pickedLayer < layers.size() && pickedLayer < now.size()
                  && now[pickedLayer].base == layers[pickedLayer].base)
                now[pickedLayer].warp = layers[pickedLayer].warp;

            layers = std::move (now);
            pickedLayer = std::min (pickedLayer, layers.size() - 1);
            fillControls();
            repaint();
        }

        //==========================================================================
        void paint (juce::Graphics& g) override
        {
            g.fillAll (Look::colour (theme, "panel"));

            //  THE LIST: each warp in words, the picked one lit.
            g.setFont (Look::font (theme, 13.0f));

            for (std::size_t n = 0; n < layers.size(); ++n)
            {
                const auto row = rowArea (n);

                if (n == pickedLayer)
                {
                    g.setColour (Look::colour (theme, "picked").withAlpha (0.25f));
                    g.fillRect (row);
                }

                g.setColour (Look::colour (theme, n == pickedLayer ? "ink" : "ink-dim"));
                g.drawText (juce::String (describe (layers[n], n)), row.reduced (8, 0), juce::Justification::centredLeft, true);
            }

            //  THE OUTPUT, the shape of a 16 by 9 display, and every warp over it.
            const auto picture = pictureArea();
            g.setColour (juce::Colours::black);
            g.fillRect (picture);
            g.setColour (Look::colour (theme, "rule"));
            g.drawRect (picture);

            for (std::size_t n = 0; n < layers.size(); ++n)
                if (n != pickedLayer)
                    drawWarp (g, picture, layers[n].warp, false);

            if (pickedLayer < layers.size())
                drawWarp (g, picture, layers[pickedLayer].warp, true);

            if (locked)
            {
                g.setColour (Look::colour (theme, "ink-off"));
                g.drawText ("The show is locked: unlock it to change the warps.", picture.withTop (picture.getBottom() - 24),
                            juce::Justification::centred);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (10);
            auto side = area.removeFromLeft (300);
            area.removeFromLeft (10);
            pictureBounds = area;

            listBounds = side.removeFromTop (std::max (120, side.getHeight() / 3));
            side.removeFromTop (6);

            auto buttons = side.removeFromTop (28);
            addZone.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2, 0));
            removeZone.setBounds (buttons.reduced (2, 0));
            side.removeFromTop (8);

            const auto pair = [&side] (juce::Component& label, juce::Component& control)
            {
                auto line = side.removeFromTop (28);
                label.setBounds (line.removeFromLeft (90));
                control.setBounds (line.reduced (0, 2));
                side.removeFromTop (4);
            };

            pair (canvasLabel, canvasBox);
            pair (blendLabel, blendBox);
            pair (opacityLabel, opacityBox);

            auto points = side.removeFromTop (28);
            pointLabel.setBounds (points.removeFromLeft (90));
            pointX.setBounds (points.removeFromLeft (points.getWidth() / 2).reduced (2));
            pointY.setBounds (points.reduced (2));
            side.removeFromTop (6);

            auto grid = side.removeFromTop (28);
            gridLabel.setBounds (grid.removeFromLeft (90));
            const auto quarter = grid.getWidth() / 4;
            fewerColumns.setBounds (grid.removeFromLeft (quarter).reduced (2, 0));
            moreColumns.setBounds (grid.removeFromLeft (quarter).reduced (2, 0));
            fewerRows.setBounds (grid.removeFromLeft (quarter).reduced (2, 0));
            moreRows.setBounds (grid.reduced (2, 0));
            side.removeFromTop (6);

            auto lastRow = side.removeFromTop (28);
            whole.setBounds (lastRow.removeFromLeft (lastRow.getWidth() / 2).reduced (0, 2).withTrimmedRight (2));
            copyTo.setBounds (lastRow.reduced (0, 2).withTrimmedLeft (2));
            side.removeFromTop (8);
            hint.setBounds (side);
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            grabKeyboardFocus();

            for (std::size_t n = 0; n < layers.size(); ++n)
                if (rowArea (n).contains (event.getPosition()))
                {
                    pickedLayer = n;
                    pickedPoint.reset();
                    fillControls();
                    repaint();
                    return;
                }

            if (pickedLayer >= layers.size() || ! pictureArea().contains (event.getPosition()))
                return;

            pickedPoint = nearestPoint (event.position);
            dragging = pickedPoint.has_value() && ! locked;
            fillControls();
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (! dragging || ! pickedPoint.has_value() || pickedLayer >= layers.size())
                return;

            const auto picture = pictureArea().toFloat();
            auto warp = layers[pickedLayer].warp;
            const auto at = *pickedPoint;

            /*  A LITTLE PAST THE EDGES IS ALLOWED: a warp that takes only part
                of a canvas onto the display puts its points outside it. */
            warp.x[at] = std::clamp ((event.position.x - picture.getX()) / picture.getWidth(), -0.5f, 1.5f);
            warp.y[at] = std::clamp ((event.position.y - picture.getY()) / picture.getHeight(), -0.5f, 1.5f);
            writeWarp (warp, false);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            dragging = false;
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (! pickedPoint.has_value() || pickedLayer >= layers.size() || locked)
                return false;

            const auto step = key.getModifiers().isShiftDown() ? 0.01 : 0.001;
            auto warp = layers[pickedLayer].warp;
            const auto at = *pickedPoint;

            if (key.isKeyCode (juce::KeyPress::leftKey))       warp.x[at] -= step;
            else if (key.isKeyCode (juce::KeyPress::rightKey)) warp.x[at] += step;
            else if (key.isKeyCode (juce::KeyPress::upKey))    warp.y[at] -= step;
            else if (key.isKeyCode (juce::KeyPress::downKey))  warp.y[at] += step;
            else return false;

            writeWarp (warp, false);
            return true;
        }

        std::string outputName;

    private:
        //==========================================================================
        juce::Rectangle<int> rowArea (std::size_t n) const
        {
            return listBounds.withHeight (26).translated (0, static_cast<int> (n) * 26);
        }

        juce::Rectangle<int> pictureArea() const
        {
            constexpr double aspect = 16.0 / 9.0;
            auto area = pictureBounds.reduced (40);

            if (area.getWidth() > area.getHeight() * aspect)
                return area.withSizeKeepingCentre (static_cast<int> (area.getHeight() * aspect), area.getHeight());

            return area.withSizeKeepingCentre (area.getWidth(), static_cast<int> (area.getWidth() / aspect));
        }

        std::string canvasName (const std::string& id) const
        {
            for (const auto& canvas : canvases)
                if (canvas.id == id)
                    return canvas.label();

            return id.empty() ? std::string ("no canvas") : id;
        }

        std::string describe (const Layer& layer, std::size_t n) const
        {
            if (n == 0)
                return canvasName (layer.canvas) + " - the output's own";

            const auto name = layer.name.empty() ? canvasName (layer.canvas) : layer.name;
            return "Zone " + std::to_string (n) + ": " + name + ", " + layer.blend + ", "
                 + osc::formatDouble (layer.opacity) + " %";
        }

        std::string firstCanvas() const
        {
            return canvases.empty() ? std::string {} : canvases.front().id;
        }

        const Layer* picked() const
        {
            return pickedLayer < layers.size() ? &layers[pickedLayer] : nullptr;
        }

        std::optional<std::size_t> nearestPoint (juce::Point<float> at) const
        {
            const auto& warp = layers[pickedLayer].warp;
            const auto picture = pictureArea().toFloat();
            std::optional<std::size_t> best;
            auto bestDistance = 12.0f;

            for (std::size_t n = 0; n < warp.x.size(); ++n)
            {
                const juce::Point<float> point { picture.getX() + static_cast<float> (warp.x[n]) * picture.getWidth(),
                                                 picture.getY() + static_cast<float> (warp.y[n]) * picture.getHeight() };

                if (const auto distance = point.getDistanceFrom (at); distance < bestDistance)
                {
                    bestDistance = distance;
                    best = n;
                }
            }

            return best;
        }

        /*  THE WARP AS THE RENDERER WILL BEND IT: its grid lines through the
            curve the renderer draws (Mapping.h), sixteen steps a line. */
        void drawWarp (juce::Graphics& g, juce::Rectangle<int> picture, const model::WarpPoints& warp, bool bright) const
        {
            const auto mesh = meshOf (warp);

            if (! mesh.isValid())
                return;

            const auto area = picture.toFloat();
            const auto place = [&mesh, &area] (double s, double t)
            {
                double x = 0.0, y = 0.0;
                video::meshAt (mesh, s, t, x, y);
                return juce::Point<float> { area.getX() + static_cast<float> (x) * area.getWidth(),
                                            area.getY() + static_cast<float> (y) * area.getHeight() };
            };

            g.setColour (bright ? Look::colour (theme, "accent") : Look::colour (theme, "ink-faint"));

            constexpr int steps = 16;

            for (int column = 0; column < warp.columns; ++column)
            {
                juce::Path line;
                const auto s = static_cast<double> (column) / (warp.columns - 1);

                for (int step = 0; step <= steps; ++step)
                {
                    const auto point = place (s, static_cast<double> (step) / steps);
                    step == 0 ? line.startNewSubPath (point) : line.lineTo (point);
                }

                g.strokePath (line, juce::PathStrokeType (bright ? 1.5f : 1.0f));
            }

            for (int row = 0; row < warp.rows; ++row)
            {
                juce::Path line;
                const auto t = static_cast<double> (row) / (warp.rows - 1);

                for (int step = 0; step <= steps; ++step)
                {
                    const auto point = place (static_cast<double> (step) / steps, t);
                    step == 0 ? line.startNewSubPath (point) : line.lineTo (point);
                }

                g.strokePath (line, juce::PathStrokeType (bright ? 1.5f : 1.0f));
            }

            if (! bright)
                return;

            //  ITS POINTS: a dot each, the picked one ringed (a shape, not only a colour).
            for (std::size_t n = 0; n < warp.x.size(); ++n)
            {
                const juce::Point<float> point { area.getX() + static_cast<float> (warp.x[n]) * area.getWidth(),
                                                 area.getY() + static_cast<float> (warp.y[n]) * area.getHeight() };
                g.setColour (Look::colour (theme, "ink"));
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (point));

                if (pickedPoint.has_value() && *pickedPoint == n)
                {
                    g.setColour (Look::colour (theme, "picked"));
                    g.drawEllipse (juce::Rectangle<float> (16.0f, 16.0f).withCentre (point), 2.0f);
                }
            }
        }

        void fillControls()
        {
            const auto* layer = picked();
            const auto isZone = layer != nullptr && ! layer->zoneId.empty();

            canvasKeys.clear();
            canvasBox.clear (juce::dontSendNotification);

            for (const auto& [key, label] : model::canvasChoices (canvases))
            {
                canvasKeys.push_back (key);
                canvasBox.addItem (juce::String (label), static_cast<int> (canvasKeys.size()));
            }

            if (layer != nullptr)
                for (std::size_t n = 0; n < canvasKeys.size(); ++n)
                    if (canvasKeys[n] == layer->canvas)
                        canvasBox.setSelectedItemIndex (static_cast<int> (n), juce::dontSendNotification);

            blendBox.setSelectedId (1, juce::dontSendNotification);

            if (layer != nullptr)
                for (int n = 0; n < 4; ++n)
                    if (layer->blend == blends[n])
                        blendBox.setSelectedId (n + 1, juce::dontSendNotification);

            if (! opacityBox.isBeingEdited())
                opacityBox.setText (layer != nullptr ? juce::String (osc::formatDouble (layer->opacity)) : juce::String(),
                                    juce::dontSendNotification);

            const auto point = pickedPoint.has_value() && layer != nullptr && *pickedPoint < layer->warp.x.size();

            if (! pointX.isBeingEdited())
                pointX.setText (point ? juce::String (layer->warp.x[*pickedPoint], 4) : juce::String(), juce::dontSendNotification);

            if (! pointY.isBeingEdited())
                pointY.setText (point ? juce::String (layer->warp.y[*pickedPoint], 4) : juce::String(), juce::dontSendNotification);

            gridLabel.setText (layer != nullptr ? juce::String (layer->warp.columns) + juce::String::fromUTF8 (" \xc3\x97 ")
                                                    + juce::String (layer->warp.rows) + " points"
                                                : juce::String(),
                               juce::dontSendNotification);

            /*  THE OUTPUT'S OWN CANVAS LIES AS IT ALWAYS DID, normal and whole:
                only a zone has a blend and an opacity of its own. */
            blendBox.setEnabled (isZone && ! locked);
            opacityBox.setEnabled (isZone && ! locked);
            removeZone.setEnabled (isZone && ! locked);
            canvasBox.setEnabled (layer != nullptr && ! locked);
            addZone.setEnabled (! locked);

            for (auto* button : { &moreColumns, &fewerColumns, &moreRows, &fewerRows, &whole, &copyTo })
                button->setEnabled (layer != nullptr && ! locked);

            pointX.setEnabled (point && ! locked);
            pointY.setEnabled (point && ! locked);
        }

        void typedPoint()
        {
            const auto* layer = picked();

            if (layer == nullptr || ! pickedPoint.has_value() || locked)
                return;

            const auto x = osc::parseDouble (pointX.getText().trim().toStdString());
            const auto y = osc::parseDouble (pointY.getText().trim().toStdString());

            if (! x.has_value() || ! y.has_value())
                return;

            auto warp = layer->warp;
            warp.x[*pickedPoint] = *x;
            warp.y[*pickedPoint] = *y;
            writeWarp (warp, false);
        }

        void regrid (int columns, int rows)
        {
            const auto* layer = picked();

            if (layer == nullptr || locked)
                return;

            const auto wantColumns = std::clamp (layer->warp.columns + columns, 2, 16);
            const auto wantRows = std::clamp (layer->warp.rows + rows, 2, 16);

            if (wantColumns == layer->warp.columns && wantRows == layer->warp.rows)
                return;

            pickedPoint.reset();
            writeWarp (model::regridded (layer->warp, wantColumns, wantRows), true);
        }

        /*  THE WARP TO THE SHOW: its points alone while a point moves - one
            address, so a drag folds into one undo step - and its grid with them
            when the grid changes, as one gesture. Drawn at once, so the hand
            does not wait for the round trip. */
        void writeWarp (const model::WarpPoints& warp, bool withGrid)
        {
            if (pickedLayer >= layers.size() || locked || ! send)
                return;

            auto& layer = layers[pickedLayer];
            layer.warp = warp;

            if (withGrid)
                send (gesture::setNodes ({ { layer.base + "meshColumns", std::to_string (warp.columns) },
                                           { layer.base + "meshRows", std::to_string (warp.rows) },
                                           { layer.base + "mesh", model::warpText (warp) } }));
            else
                send (gesture::setNode (layer.base + "mesh", model::warpText (warp)));

            fillControls();
            repaint();
        }

        /*  COPY TO (§47.12, the author's picks): the picked warp's shape onto
            any other warp of the show, by output and then by warp - its grid
            and its points, one `node.setMany`, one undo step. */
        void chooseCopyTarget()
        {
            const auto* layer = picked();

            if (layer == nullptr || locked || ! send)
                return;

            juce::PopupMenu menu;
            std::string section;

            for (std::size_t n = 0; n < targets.size(); ++n)
            {
                const auto& target = targets[n];

                if (target.outputLabel != section)
                {
                    section = target.outputLabel;
                    menu.addSectionHeader (juce::String (section));
                }

                menu.addItem (static_cast<int> (n) + 1, juce::String (target.label), target.base != layer->base);
            }

            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&copyTo),
                                [safe = juce::Component::SafePointer<Editor> (this), warp = layer->warp] (int chosen)
                                {
                                    if (safe == nullptr || chosen <= 0 || safe->locked || ! safe->send
                                          || static_cast<std::size_t> (chosen) > safe->targets.size())
                                        return;

                                    const auto& target = safe->targets[static_cast<std::size_t> (chosen - 1)];
                                    safe->send (gesture::setNodes (model::warpCopyWrites (warp, target.base)));
                                });
        }

        //==========================================================================
        model::Theme theme;
        std::function<void (Event)> send;

        std::vector<model::WarpTarget> targets;
        std::map<std::string, std::size_t> pickedFor;

        std::string outputId;
        std::vector<Layer> layers;
        std::vector<model::CanvasRow> canvases;
        std::vector<std::string> canvasKeys;
        std::size_t pickedLayer = 0;
        std::optional<std::size_t> pickedPoint;
        bool dragging = false;
        bool locked = false;

        juce::Rectangle<int> listBounds, pictureBounds;

        juce::TextButton addZone { "+ Zone" }, removeZone { "Remove zone" };
        juce::TextButton moreColumns { "+ col" }, fewerColumns { "- col" }, moreRows { "+ row" }, fewerRows { "- row" };
        juce::TextButton whole { "Whole output" }, copyTo { "Copy to..." };
        juce::ComboBox canvasBox, blendBox;
        juce::Label opacityBox, pointX, pointY;
        juce::Label canvasLabel, blendLabel, opacityLabel, pointLabel, gridLabel, hint;
    };

    //==============================================================================
    WarpEditorPanel::WarpEditorPanel (const model::Theme& themeToUse, std::function<void (Event)> send,
                                      std::function<void()> backToUse)
        : theme (themeToUse), back (std::move (backToUse)),
          editor (std::make_unique<Editor> (themeToUse, std::move (send)))
    {
        addAndMakeVisible (*editor);

        title.setText ("Warps and zones", juce::dontSendNotification);
        title.setFont (Look::font (theme, 15.0f));
        title.setColour (juce::Label::textColourId, Look::colour (theme, "ink"));
        addAndMakeVisible (title);

        /*  ANOTHER OUTPUT WITHOUT LEAVING (the author, 2026-10-09: "a way to
            select which output we're working on ... to avoid closing and
            reopening to simply switch from one output to the next"). */
        outputBox.setWantsKeyboardFocus (false);
        outputBox.setTooltip ("The output whose warps are drawn");
        outputBox.onChange = [this]
        {
            const auto at = outputBox.getSelectedItemIndex();

            if (at >= 0 && static_cast<std::size_t> (at) < outputKeys.size())
                pendingOutput = outputKeys[static_cast<std::size_t> (at)];
        };
        addAndMakeVisible (outputBox);

        backButton.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x90 Video"));
        backButton.setWantsKeyboardFocus (false);
        backButton.setTooltip ("Back to the Video tab");
        backButton.onClick = [this] { if (back) back(); };
        addAndMakeVisible (backButton);
    }

    WarpEditorPanel::~WarpEditorPanel() = default;

    void WarpEditorPanel::open (const std::string& outputId, const tree::TreeSnapshot& snapshot)
    {
        pendingOutput.clear();
        editor->open (outputId, snapshot);
        refresh (snapshot);
        editor->grabKeyboardFocus();
    }

    void WarpEditorPanel::refresh (const tree::TreeSnapshot& snapshot)
    {
        //  An output picked from the bar, opened on this pass's snapshot.
        if (! pendingOutput.empty())
        {
            editor->open (pendingOutput, snapshot);
            pendingOutput.clear();
        }
        else
        {
            editor->refresh (snapshot);
        }

        //  THE OUTPUTS, by name, the one shown ticked - rebuilt only when they change.
        std::vector<std::string> keys;
        std::vector<std::string> labels;

        for (const auto& output : model::readVideoOutputs (snapshot))
        {
            keys.push_back (output.id);
            labels.push_back (output.label());
        }

        if (keys != outputKeys || labels != outputLabels)
        {
            outputKeys = keys;
            outputLabels = labels;
            outputBox.clear (juce::dontSendNotification);

            for (std::size_t n = 0; n < labels.size(); ++n)
                outputBox.addItem (juce::String (labels[n]), static_cast<int> (n) + 1);
        }

        for (std::size_t n = 0; n < outputKeys.size(); ++n)
            if (outputKeys[n] == editor->output() && outputBox.getSelectedItemIndex() != static_cast<int> (n))
                outputBox.setSelectedItemIndex (static_cast<int> (n), juce::dontSendNotification);
    }

    void WarpEditorPanel::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));
        g.setColour (Look::colour (theme, "rule"));
        g.drawHorizontalLine (44, 0.0f, static_cast<float> (getWidth()));
    }

    void WarpEditorPanel::resized()
    {
        auto area = getLocalBounds();
        auto bar = area.removeFromTop (44).reduced (12, 8);

        backButton.setBounds (bar.removeFromLeft (100));
        bar.removeFromLeft (16);
        title.setBounds (bar.removeFromLeft (150));
        bar.removeFromLeft (8);
        outputBox.setBounds (bar.removeFromLeft (std::min (320, bar.getWidth())));

        editor->setBounds (area);
    }
}
