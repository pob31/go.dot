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

#include <wfg/client/ui/PatchCanvasComponent.h>
#include <wfg/client/ui/Look.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wfg::client::ui
{
    using process::BoxKind;

    namespace
    {
        constexpr double margin = 12.0;     // the empty border around a patch, in canvas pixels
        constexpr double minScale = 0.25;
        constexpr double maxScale = 4.0;
    }

    PatchCanvasComponent::PatchCanvasComponent (const model::Theme& themeToUse, Actions actionsToUse)
        : theme (themeToUse), actions (std::move (actionsToUse))
    {
        setWantsKeyboardFocus (true);
        setTitle ("Patch");
        setDescription ("The boxes and lines this process cue runs");
    }

    void PatchCanvasComponent::applyTheme (const model::Theme& themeToUse)
    {
        theme = themeToUse;
        repaint();
    }

    void PatchCanvasComponent::rebuild (const std::string& text)
    {
        drawnText = text;
        patch = process::parsePatch (text);
        view = model::viewPatch (patch);

        //  A pick that names a box or line no longer there is let go.
        for (auto at = boxes.begin(); at != boxes.end();)
            at = *at < patch.boxes.size() ? std::next (at) : boxes.erase (at);
        for (auto at = lines.begin(); at != lines.end();)
            at = *at < patch.lines.size() ? std::next (at) : lines.erase (at);
    }

    void PatchCanvasComponent::show (const model::PatchReading& readingToShow)
    {
        const bool otherCue = readingToShow.cueId != reading.cueId;
        reading = readingToShow;

        if (otherCue)
        {
            held.reset();
            boxes.clear();
            lines.clear();
            origin = { -margin, -margin };
            scale = 1.0;
        }

        /*  WHAT IS DRAWN: what was sent, until the tree has it - or has moved on
            past it, an edit from somewhere else winning. */
        if (held.has_value() && reading.text == *held)
            held.reset();

        const auto& wanted = held.has_value() && hand == Hand::none ? *held : reading.text;

        if (wanted != drawnText || otherCue)
        {
            rebuild (wanted);
            repaint();
        }
    }

    bool PatchCanvasComponent::editable() const
    {
        return ! reading.cueId.empty() && ! reading.locked && static_cast<bool> (actions.set);
    }

    void PatchCanvasComponent::write (const std::string& text)
    {
        if (text == drawnText || ! editable())
            return;

        held = text;
        rebuild (text);
        actions.set ("/godot/cue/" + reading.cueId + "/patch", text);
        repaint();
    }

    juce::Point<double> PatchCanvasComponent::toCanvas (juce::Point<float> screen) const
    {
        return { origin.x + static_cast<double> (screen.x) / scale,
                 origin.y + static_cast<double> (screen.y) / scale };
    }

    juce::Point<float> PatchCanvasComponent::onScreen (double x, double y) const
    {
        return { static_cast<float> ((x - origin.x) * scale), static_cast<float> ((y - origin.y) * scale) };
    }

    void PatchCanvasComponent::zoomAbout (juce::Point<float> screen, double factor)
    {
        const auto anchor = toCanvas (screen);
        scale = std::clamp (scale * factor, minScale, maxScale);
        origin = { anchor.x - static_cast<double> (screen.x) / scale, anchor.y - static_cast<double> (screen.y) / scale };
        repaint();
    }

    void PatchCanvasComponent::resized()
    {
        repaint();
    }

    void PatchCanvasComponent::drawBox (juce::Graphics& g, const model::PatchBoxView& box,
                                        juce::Point<double> offset, bool picked) const
    {
        const auto topLeft = onScreen (box.x + offset.x, box.y + offset.y);
        const auto w = static_cast<float> (box.w * scale);
        const auto h = static_cast<float> (box.h * scale);
        const juce::Rectangle<float> area { topLeft.x, topLeft.y, w, h };

        const auto ink = Look::colour (theme, picked ? "picked" : "ink");
        const auto edge = Look::colour (theme, picked ? "picked" : "ink-dim");

        if (box.kind == BoxKind::other || w <= 0.0f || h <= 0.0f)
            return;

        //  THE BOX'S OUTLINE, as Pd tells its kinds apart.
        g.setColour (edge);
        const auto line = std::max (1.0f, static_cast<float> (scale));

        switch (box.kind)
        {
            case BoxKind::message:
            {
                //  A message box: its right edge folded in, Pd's flag.
                const auto notch = std::min (4.0f * static_cast<float> (scale), w / 3.0f);
                juce::Path flag;
                flag.startNewSubPath (area.getX(), area.getY());
                flag.lineTo (area.getRight() + notch, area.getY());
                flag.lineTo (area.getRight(), area.getY() + notch);
                flag.lineTo (area.getRight(), area.getBottom() - notch);
                flag.lineTo (area.getRight() + notch, area.getBottom());
                flag.lineTo (area.getX(), area.getBottom());
                flag.closeSubPath();
                g.strokePath (flag, juce::PathStrokeType (line));
                break;
            }

            case BoxKind::number:
            case BoxKind::symbol:
            case BoxKind::list:
            {
                //  An atom box: its top right corner cut.
                const auto cut = std::min (4.0f * static_cast<float> (scale), h / 2.0f);
                juce::Path atom;
                atom.startNewSubPath (area.getX(), area.getY());
                atom.lineTo (area.getRight() - cut, area.getY());
                atom.lineTo (area.getRight(), area.getY() + cut);
                atom.lineTo (area.getRight(), area.getBottom());
                atom.lineTo (area.getX(), area.getBottom());
                atom.closeSubPath();
                g.strokePath (atom, juce::PathStrokeType (line));
                break;
            }

            case BoxKind::comment:
                //  A comment has no outline; a picked one is underlined in the picked ink.
                if (picked)
                    g.drawLine (area.getX(), area.getBottom(), area.getRight(), area.getBottom(), line);
                break;

            case BoxKind::object:
            case BoxKind::subpatch:
            case BoxKind::other:
                g.drawRect (area, line);
                break;
        }

        //  ITS INLETS AND OUTLETS, small and solid on its edges.
        g.setColour (edge);
        const auto portW = static_cast<float> (model::portWidth * scale);
        const auto portH = std::max (2.0f, static_cast<float> (model::portHeight * scale));
        for (int p = 0; p < box.inlets; ++p)
        {
            const auto at = onScreen (model::portX (box, p, box.inlets) + offset.x, box.y + offset.y);
            g.fillRect (at.x, at.y, portW, portH);
        }
        for (int p = 0; p < box.outlets; ++p)
        {
            const auto at = onScreen (model::portX (box, p, box.outlets) + offset.x, box.y + box.h + offset.y);
            g.fillRect (at.x, at.y - portH, portW, portH);
        }

        //  ITS WORDS, Pd's lines, in a fixed-width face the size Pd's are.
        g.setColour (ink);
        const auto fontHeight = static_cast<float> (view.font.lineHeight * scale * 0.82);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), fontHeight, juce::Font::plain)));

        if (box.lines.empty())
        {
            //  A GUI box drawn as its kind's word, small, until PC.8 draws it.
            const auto words = process::splitWords (patch.boxes[box.box].text);
            const auto name = words.empty() ? std::string {} : process::unescaped (words[0]);
            g.setFont (Look::font (theme, std::max (8.0f, std::min (h, w) * 0.4f)));
            g.drawFittedText (juce::String (name), area.toNearestInt(), juce::Justification::centred, 1);
            return;
        }

        auto y = area.getY() + static_cast<float> (3.0 * scale);
        for (const auto& text : box.lines)
        {
            g.drawText (juce::String::fromUTF8 (text.c_str()),
                        juce::Rectangle<float> (area.getX() + static_cast<float> (2.0 * scale), y,
                                                std::max (w, 1.0f) + 200.0f, static_cast<float> (view.font.lineHeight * scale)),
                        juce::Justification::centredLeft, false);
            y += static_cast<float> (view.font.lineHeight * scale);
        }
    }

    void PatchCanvasComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel"));

        if (reading.cueId.empty())
            return;

        //  Where the picked boxes are while a hand drags them.
        const juce::Point<double> moving = hand == Hand::moving
                                             ? juce::Point<double> (std::round (now.x - downAt.x), std::round (now.y - downAt.y))
                                             : juce::Point<double> {};
        const auto offsetOf = [&] (std::size_t viewIndex)
        {
            return boxes.count (view.boxes[viewIndex].box) > 0 ? moving : juce::Point<double> {};
        };

        //  THE LINES, under the boxes.
        for (const auto& drawn : view.lines)
        {
            const auto from = offsetOf (drawn.fromView), to = offsetOf (drawn.toView);
            const auto a = onScreen (drawn.x1 + from.x, drawn.y1 + from.y);
            const auto b = onScreen (drawn.x2 + to.x, drawn.y2 + to.y);
            const bool picked = lines.count (drawn.line) > 0;
            g.setColour (Look::colour (theme, picked ? "picked" : "ink-faint"));
            g.drawLine (a.x, a.y, b.x, b.y, std::max (1.0f, static_cast<float> (scale)) * (picked ? 2.0f : 1.0f));
        }

        for (std::size_t n = 0; n < view.boxes.size(); ++n)
            drawBox (g, view.boxes[n], offsetOf (n), boxes.count (view.boxes[n].box) > 0);

        //  THE BAND a hand is drawing.
        if (hand == Hand::banding)
        {
            const auto a = onScreen (downAt.x, downAt.y), b = onScreen (now.x, now.y);
            const auto band = juce::Rectangle<float> (a, b);
            g.setColour (Look::colour (theme, "picked").withAlpha (0.15f));
            g.fillRect (band);
            g.setColour (Look::colour (theme, "picked"));
            g.drawRect (band, 1.0f);
        }

        //  WHAT IT CANNOT SHOW, said in the corner rather than nowhere.
        if (! patch.problem.empty() || view.boxes.empty())
        {
            g.setColour (Look::colour (theme, ! patch.problem.empty() ? "failed" : "ink-faint"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawText (! patch.problem.empty() ? juce::String ("Not a patch: ") + juce::String (patch.problem)
                                                : juce::String ("An empty patch"),
                        getLocalBounds().reduced (8).removeFromBottom (20), juce::Justification::bottomLeft, true);
        }
    }

    void PatchCanvasComponent::mouseDown (const juce::MouseEvent& e)
    {
        grabKeyboardFocus();
        downAt = now = toCanvas (e.position);

        if (e.mods.isMiddleButtonDown() || e.mods.isAltDown())
        {
            hand = Hand::panning;
            panFrom = e.position;
            originAtDown = origin;
            return;
        }

        const auto hit = model::hitPatch (view, downAt.x, downAt.y);
        const bool adding = e.mods.isShiftDown();

        if (hit.what == model::PatchHit::What::line)
        {
            const auto line = view.lines[hit.item].line;
            if (! adding)
            {
                boxes.clear();
                lines.clear();
            }
            if (lines.count (line) > 0 && adding)
                lines.erase (line);
            else
                lines.insert (line);
            hand = Hand::none;
            repaint();
            return;
        }

        if (hit.what == model::PatchHit::What::box || hit.what == model::PatchHit::What::inlet
             || hit.what == model::PatchHit::What::outlet)
        {
            const auto box = view.boxes[hit.item].box;
            if (adding)
            {
                if (boxes.count (box) > 0)
                    boxes.erase (box);
                else
                    boxes.insert (box);
            }
            else if (boxes.count (box) == 0)
            {
                boxes = { box };
                lines.clear();
            }
            hand = boxes.count (box) > 0 && editable() ? Hand::moving : Hand::none;
            repaint();
            return;
        }

        if (! adding)
        {
            boxes.clear();
            lines.clear();
        }
        hand = Hand::banding;
        repaint();
    }

    void PatchCanvasComponent::mouseDrag (const juce::MouseEvent& e)
    {
        if (hand == Hand::panning)
        {
            const auto moved = e.position - panFrom;
            origin = { originAtDown.x - static_cast<double> (moved.x) / scale,
                       originAtDown.y - static_cast<double> (moved.y) / scale };
            repaint();
            return;
        }

        now = toCanvas (e.position);
        if (hand != Hand::none)
            repaint();
    }

    void PatchCanvasComponent::mouseUp (const juce::MouseEvent&)
    {
        const auto was = hand;
        hand = Hand::none;

        if (was == Hand::moving)
        {
            const auto dx = static_cast<int> (std::round (now.x - downAt.x));
            const auto dy = static_cast<int> (std::round (now.y - downAt.y));
            if (dx != 0 || dy != 0)
                write (model::patchMoved (drawnText, { boxes.begin(), boxes.end() }, dx, dy));
        }
        else if (was == Hand::banding)
        {
            for (const auto n : model::boxesTouched (view, downAt.x, downAt.y, now.x, now.y))
                boxes.insert (view.boxes[n].box);
        }

        if (! editable() && was == Hand::none && ! reading.cueId.empty() && reading.locked && actions.say)
            actions.say ("The show is locked: unlock it to edit the patch");

        repaint();
    }

    void PatchCanvasComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            zoomAbout (e.position, std::pow (1.15, static_cast<double> (wheel.deltaY) * 4.0));
            return;
        }

        //  THE WHEEL PANS: down and up, and across with Shift or a sideways wheel.
        const auto across = e.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX;
        const auto down = e.mods.isShiftDown() ? 0.0f : wheel.deltaY;
        origin.x -= static_cast<double> (across) * 160.0 / scale;
        origin.y -= static_cast<double> (down) * 160.0 / scale;
        repaint();
    }

    void PatchCanvasComponent::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
    {
        zoomAbout (e.position, static_cast<double> (scaleFactor));
    }

    bool PatchCanvasComponent::keyPressed (const juce::KeyPress& key)
    {
        const auto code = key.getKeyCode();

        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        {
            if (boxes.empty() && lines.empty())
                return false;
            if (! editable())
            {
                if (actions.say)
                    actions.say ("The show is locked: unlock it to edit the patch");
                return true;
            }
            const auto text = model::patchDeleted (drawnText, { boxes.begin(), boxes.end() }, { lines.begin(), lines.end() });
            boxes.clear();
            lines.clear();
            write (text);
            return true;
        }

        //  THE ARROWS NUDGE the picked boxes: a pixel, ten with Shift.
        const int step = key.getModifiers().isShiftDown() ? 10 : 1;
        int dx = 0, dy = 0;
        if (code == juce::KeyPress::leftKey)  dx = -step;
        if (code == juce::KeyPress::rightKey) dx = step;
        if (code == juce::KeyPress::upKey)    dy = -step;
        if (code == juce::KeyPress::downKey)  dy = step;

        if ((dx != 0 || dy != 0) && ! boxes.empty() && ! key.getModifiers().isCommandDown())
        {
            write (model::patchMoved (drawnText, { boxes.begin(), boxes.end() }, dx, dy));
            return true;
        }

        return false;
    }
}
