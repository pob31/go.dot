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
#include <wfg/engine/osc/OscValue.h>

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

        /*  THE DOOR TO PLUGDATA OR PD (PC.7), in the canvas's corner: what it
            cannot do yet, done there, and each save coming back here. */
        addAndMakeVisible (openButton);
        openButton.onClick = [this]
        {
            if (reading.editor.empty())
            {
                if (actions.getEditor)
                    actions.getEditor();
            }
            else if (actions.openInEditor && ! reading.cueId.empty())
            {
                actions.openInEditor (reading.cueId);
            }
        };

        addChildComponent (editor);
        editor.setMultiLine (false);
        editor.setReturnKeyStartsNewLine (false);
        editor.onReturnKey = [this]
        {
            commitTyping();
            grabKeyboardFocus();
        };
        editor.onFocusLost = [this] { commitTyping(); };
        editor.setTitle ("A box's words");
    }

    void PatchCanvasComponent::startTyping (std::size_t box)
    {
        const auto* shown = view.viewOf (box);
        if (shown == nullptr || ! editable())
            return;
        if (shown->kind != BoxKind::object && shown->kind != BoxKind::message && shown->kind != BoxKind::comment)
            return;

        //  Its words as Pd shows them, on one line.
        std::string words;
        for (const auto& word : process::splitWords (patch.boxes[box].text))
            words += (words.empty() ? "" : " ") + process::unescaped (word);

        typing = box;
        const auto at = onScreen (shown->x, shown->y);
        editor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                       static_cast<float> (view.font.lineHeight * scale * 0.82),
                                                       juce::Font::plain)));
        editor.setBounds (juce::Rectangle<float> (at.x, at.y,
                                                  std::max (static_cast<float> (shown->w * scale), 140.0f),
                                                  static_cast<float> ((view.font.lineHeight + 6.0) * scale))
                              .toNearestInt());
        editor.setText (juce::String::fromUTF8 (words.c_str()), juce::dontSendNotification);
        editor.setVisible (true);
        editor.grabKeyboardFocus();
        editor.selectAll();
    }

    void PatchCanvasComponent::commitTyping()
    {
        if (! typing.has_value())
            return;

        const auto box = *typing;
        typing.reset();
        editor.setVisible (false);
        write (model::patchTyped (drawnText, box, editor.getText().toStdString()));
    }

    void PatchCanvasComponent::place (model::Placed what)
    {
        if (! editable())
            return;

        const auto x = static_cast<int> (std::round (pointer.x));
        const auto y = static_cast<int> (std::round (pointer.y));
        write (model::patchPlaced (drawnText, what, x, y));

        //  The box placed is the last, and the words it needs are typed into it.
        if (! patch.boxes.empty())
        {
            boxes = { patch.boxes.size() - 1 };
            lines.clear();
            if (what == model::Placed::object || what == model::Placed::message || what == model::Placed::comment)
                startTyping (patch.boxes.size() - 1);
        }
    }

    void PatchCanvasComponent::copyPicked()
    {
        if (! boxes.empty())
            juce::SystemClipboard::copyTextToClipboard (
                juce::String::fromUTF8 (model::patchCopied (drawnText, { boxes.begin(), boxes.end() }).c_str()));
    }

    void PatchCanvasComponent::paste (const juce::String& piece)
    {
        if (! editable())
            return;

        const auto text = piece.toStdString();
        const auto parsed = process::parsePatch ("#N canvas 0 0 10 10 12;\n" + text);
        if (! parsed.problem.empty() || parsed.boxes.empty())
        {
            if (actions.say)
                actions.say ("Nothing to paste: the clipboard holds no boxes");
            return;
        }

        const auto pasted = model::patchPasted (drawnText, text, 10, 10);
        write (pasted.text);
        boxes = { pasted.boxes.begin(), pasted.boxes.end() };
        lines.clear();
        repaint();
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

        //  THE VALUES ON ITS NAMED PORTS (PC.8), drawn while it runs.
        if (auto fresh = model::portValues (reading.ports); fresh != values)
        {
            values = std::move (fresh);
            repaint();
        }

        //  The door's words: where it opens, or Pd's download as it goes.
        juce::String words;
        if (! reading.editor.empty())
            words = "Open in " + juce::String (reading.editor);
        else if (reading.editorInstall.rfind ("downloading", 0) == 0 || reading.editorInstall.rfind ("unpacking", 0) == 0
                  || reading.editorInstall.rfind ("checking", 0) == 0)
            words = "Getting Pd...";
        else
            words = "Get Pd...";
        if (openButton.getButtonText() != words)
            openButton.setButtonText (words);
        openButton.setEnabled (! reading.cueId.empty() && ! reading.locked);
        openButton.setTooltip ("A subpatch, an array or a box's properties, in plugdata or Pure Data:"
                               " each save there comes back here");
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
        openButton.setBounds (getLocalBounds().removeFromTop (30).removeFromRight (150).reduced (4));
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
            /*  A GUI BOX AS ITSELF (PC.8), showing what its send or receive last
                carried: a toggle crossed when on, a bang's circle, a slider's
                place, a radio's cell, a number box's number. */
            const auto gui = model::guiOf (patch, box.box);
            const auto value = gui.has_value() ? shownValue (*gui) : std::nullopt;
            g.setColour (ink);

            if (gui.has_value() && gui->kind == "tgl")
            {
                if (value.has_value() && (*value < 0.0 || *value > 0.0))
                {
                    g.drawLine (area.getX() + 2.0f, area.getY() + 2.0f, area.getRight() - 2.0f, area.getBottom() - 2.0f, line);
                    g.drawLine (area.getRight() - 2.0f, area.getY() + 2.0f, area.getX() + 2.0f, area.getBottom() - 2.0f, line);
                }
            }
            else if (gui.has_value() && gui->kind == "bng")
            {
                g.drawEllipse (area.reduced (2.0f), line);
            }
            else if (gui.has_value() && (gui->kind == "hsl" || gui->kind == "vsl"))
            {
                const auto span = gui->high - gui->low;
                const auto at = value.has_value() && (span < 0.0 || span > 0.0)
                                  ? static_cast<float> (std::clamp ((*value - gui->low) / span, 0.0, 1.0)) : 0.0f;
                if (gui->kind == "hsl")
                {
                    const auto x = area.getX() + at * area.getWidth();
                    g.drawLine (x, area.getY(), x, area.getBottom(), line * 2.0f);
                }
                else
                {
                    const auto y = area.getBottom() - at * area.getHeight();
                    g.drawLine (area.getX(), y, area.getRight(), y, line * 2.0f);
                }
            }
            else if (gui.has_value() && (gui->kind == "hradio" || gui->kind == "vradio"))
            {
                const auto cells = static_cast<float> (gui->cells);
                const bool across = gui->kind == "hradio";
                for (int c = 1; c < gui->cells; ++c)
                {
                    const auto f = static_cast<float> (c) / cells;
                    if (across)
                        g.drawLine (area.getX() + f * area.getWidth(), area.getY(), area.getX() + f * area.getWidth(), area.getBottom(), line);
                    else
                        g.drawLine (area.getX(), area.getY() + f * area.getHeight(), area.getRight(), area.getY() + f * area.getHeight(), line);
                }
                if (value.has_value())
                {
                    const auto cell = std::clamp (static_cast<float> (std::floor (*value)), 0.0f, cells - 1.0f);
                    const auto lit = across ? juce::Rectangle<float> (area.getX() + cell * area.getWidth() / cells, area.getY(),
                                                                      area.getWidth() / cells, area.getHeight())
                                            : juce::Rectangle<float> (area.getX(), area.getY() + cell * area.getHeight() / cells,
                                                                      area.getWidth(), area.getHeight() / cells);
                    g.fillRect (lit.reduced (2.0f));
                }
            }
            else
            {
                const auto words = process::splitWords (patch.boxes[box.box].text);
                const auto name = words.empty() ? std::string {} : process::unescaped (words[0]);
                const auto said = gui.has_value() && gui->kind == "nbx"
                                    ? juce::String (value.has_value() ? osc::formatDouble (*value) : std::string ("0"))
                                    : juce::String (name);
                g.setFont (Look::font (theme, std::max (8.0f, std::min (h, w) * 0.6f)));
                g.drawFittedText (said, area.toNearestInt(), juce::Justification::centred, 1);
            }
            return;
        }

        /*  A SEND OR A RECEIVE BOX'S LAST VALUE (PC.8), beside it while it runs. */
        if (box.kind == BoxKind::object)
        {
            const auto words = process::splitWords (patch.boxes[box.box].text);
            if (words.size() >= 2)
            {
                const auto cls = process::unescaped (words[0]);
                if (cls == "s" || cls == "send" || cls == "r" || cls == "receive")
                    if (const auto found = values.find (process::unescaped (words[1])); found != values.end())
                    {
                        g.setColour (Look::colour (theme, "ink-faint"));
                        g.setFont (Look::font (theme, std::max (9.0f, static_cast<float> (11.0 * scale))));
                        g.drawText (juce::String::fromUTF8 (("= " + found->second).c_str()),
                                    juce::Rectangle<float> (area.getRight() + 6.0f, area.getY(), 240.0f, h),
                                    juce::Justification::centredLeft, true);
                    }
            }
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

        //  THE LINE a hand is drawing from an outlet.
        if (hand == Hand::connecting && connectFrom < view.boxes.size())
        {
            const auto& from = view.boxes[connectFrom];
            const auto a = onScreen (model::portX (from, connectOutlet, from.outlets) + (model::portWidth - 1.0) / 2.0,
                                     from.y + from.h);
            const auto b = onScreen (now.x, now.y);
            g.setColour (Look::colour (theme, "picked"));
            g.drawLine (a.x, a.y, b.x, b.y, std::max (1.0f, static_cast<float> (scale)));
        }

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

        //  PLAYING, said: a hand reaches the patch rather than moving it.
        if (playing)
        {
            g.setColour (Look::colour (theme, "live"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawText (reading.runId.empty() ? "Playing - but the cue is not running" : "Playing - clicks reach the patch (Ctrl+E edits)",
                        getLocalBounds().reduced (8).removeFromTop (20), juce::Justification::topLeft, true);
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

    void PatchCanvasComponent::mouseMove (const juce::MouseEvent& e)
    {
        pointer = toCanvas (e.position);
    }

    void PatchCanvasComponent::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const auto at = toCanvas (e.position);
        const auto hit = model::hitPatch (view, at.x, at.y);
        if (hit.what == model::PatchHit::What::box)
            startTyping (view.boxes[hit.item].box);
    }

    std::optional<double> PatchCanvasComponent::shownValue (const model::GuiBox& gui) const
    {
        for (const auto* name : { &gui.send, &gui.receive })
            if (! name->empty())
                if (const auto found = values.find (*name); found != values.end())
                    if (const auto value = osc::parseDouble (found->second.substr (0, found->second.find (' '))))
                        return value;
        return std::nullopt;
    }

    void PatchCanvasComponent::playAt (std::size_t box, juce::Point<double> at, bool landing)
    {
        const auto gui = model::guiOf (patch, box);
        const auto* shown = view.viewOf (box);
        if (! gui.has_value() || shown == nullptr || gui->receive.empty() || reading.runId.empty() || ! actions.send)
            return;

        //  A toggle and a bang act as the hand lands, not as it moves.
        if (! landing && (gui->kind == "tgl" || gui->kind == "bng"))
            return;

        const auto fx = shown->w > 0.0 ? (at.x - shown->x) / shown->w : 0.0;
        const auto fy = shown->h > 0.0 ? (at.y - shown->y) / shown->h : 0.0;
        if (landing)
            playingFrom = shownValue (*gui).value_or (0.0);

        const auto atoms = model::guiPress (*gui, fx, fy, shownValue (*gui), playingFrom + (downAt.y - at.y));
        if (! atoms.has_value())
            return;

        actions.send (reading.runId, gui->receive, *atoms);

        //  Shown at once, before the patch answers.
        if (! atoms->empty() && atoms->front().isNumber)
            values[gui->receive] = osc::formatDouble (atoms->front().number);
        repaint();
    }

    void PatchCanvasComponent::mouseDown (const juce::MouseEvent& e)
    {
        commitTyping();
        grabKeyboardFocus();
        downAt = now = pointer = toCanvas (e.position);

        if (playing)
        {
            const auto hit = model::hitPatch (view, downAt.x, downAt.y);
            playingBox.reset();
            if (hit.what == model::PatchHit::What::box)
            {
                playingBox = view.boxes[hit.item].box;
                playAt (*playingBox, downAt, true);
            }
            return;
        }

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

        //  AN OUTLET GRABBED starts a line (PC.6).
        if (hit.what == model::PatchHit::What::outlet && editable() && ! adding)
        {
            hand = Hand::connecting;
            connectFrom = hit.item;
            connectOutlet = hit.port;
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
        if (playing)
        {
            if (playingBox.has_value())
                playAt (*playingBox, toCanvas (e.position), false);
            return;
        }

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
        playingBox.reset();
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
        else if (was == Hand::connecting)
        {
            //  Let go over an inlet - or the box, its nearest inlet then - joins them.
            const auto hit = model::hitPatch (view, now.x, now.y);
            if (hit.what == model::PatchHit::What::inlet)
                write (model::patchConnected (view, drawnText, connectFrom, connectOutlet, hit.item, hit.port));
            else if (hit.what == model::PatchHit::What::box && view.boxes[hit.item].inlets > 0)
            {
                const auto& to = view.boxes[hit.item];
                int nearest = 0;
                double best = 1e9;
                for (int p = 0; p < to.inlets; ++p)
                {
                    const auto d = std::abs (model::portX (to, p, to.inlets) + model::portWidth / 2.0 - now.x);
                    if (d < best)
                    {
                        best = d;
                        nearest = p;
                    }
                }
                write (model::patchConnected (view, drawnText, connectFrom, connectOutlet, hit.item, nearest));
            }
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
        const auto command = key.getModifiers().isCommandDown();

        //  PD'S CTRL+E: playing the patch, or editing it.
        if (command && (code == 'E' || code == 'e'))
        {
            commitTyping();
            playing = ! playing;
            boxes.clear();
            lines.clear();
            repaint();
            return true;
        }

        //  PD'S CTRL+1 TO CTRL+5: an object, a message, a number, a symbol, a comment.
        if (command && code >= '1' && code <= '5')
        {
            static constexpr model::Placed placed[] = { model::Placed::object, model::Placed::message,
                                                        model::Placed::number, model::Placed::symbol,
                                                        model::Placed::comment };
            place (placed[code - '1']);
            return true;
        }

        if (command && (code == 'A' || code == 'a'))
        {
            boxes.clear();
            for (std::size_t b = 0; b < patch.boxes.size(); ++b)
                if (patch.boxes[b].canvas == 0 && patch.boxes[b].kind != BoxKind::other)
                    boxes.insert (b);
            lines.clear();
            repaint();
            return true;
        }

        if (command && (code == 'C' || code == 'c'))
        {
            copyPicked();
            return true;
        }

        if (command && (code == 'X' || code == 'x'))
        {
            copyPicked();
            if (! boxes.empty() && editable())
            {
                const auto text = model::patchDeleted (drawnText, { boxes.begin(), boxes.end() }, {});
                boxes.clear();
                write (text);
            }
            return true;
        }

        if (command && (code == 'V' || code == 'v'))
        {
            paste (juce::SystemClipboard::getTextFromClipboard());
            return true;
        }

        if (command && (code == 'D' || code == 'd'))
        {
            if (! boxes.empty())
                paste (juce::String::fromUTF8 (model::patchCopied (drawnText, { boxes.begin(), boxes.end() }).c_str()));
            return true;
        }

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
