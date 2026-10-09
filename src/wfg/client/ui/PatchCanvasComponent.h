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

#pragma once

/*
    A PROCESS CUE'S PATCH AT THE FOOT (namespace draft §51, ACD, ACM; PC.5).

    The author chose Go.dot's own canvas over Pd's window ("Is there a prettier
    option than the basic Tcl/Tk built in Pd?"), built in rounds. This is the
    first: the patch drawn where Pd draws it (model/Patch), in Go.dot's grounds
    and inks; panned by the wheel or a drag with Alt or the middle button,
    zoomed with Ctrl or Cmd on the wheel or a pinch; boxes and lines picked by a
    click, Shift adding, or a band drawn on the empty canvas; the picked boxes
    dragged, nudged by the arrow keys - one pixel, ten with Shift - and deleted
    with Delete or Backspace, a line with them.

    EACH GESTURE IS ONE `node.set` of the cue's whole patch, sent when the hand
    lets go: a drag is not a run of writes. While the tree catches up the text
    sent is drawn, so the boxes do not jump back. Under the lock it draws and
    changes nothing, and says so.

    THE SECOND ROUND (PC.6): a double click on a box types into it - Return or
    a click elsewhere commits, an object or a message typed empty goes; Ctrl or
    Cmd with 1 to 5 places an object, a message, a number, a symbol or a
    comment where the pointer is, as Pd's do, and types into it; a drag from an
    outlet to an inlet draws a line; Ctrl or Cmd with A picks every box, with C
    copies the picked boxes and the lines between them, as Pd's own text, X
    cuts, V pastes beside where they were and D duplicates.

    IT TAKES THE KEYS IT USES AND NO OTHER: Space is still GO and Esc still the
    PANIC, from wherever the focus is - typing into a box included.
*/

#include <wfg/client/model/Patch.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <set>
#include <string>

namespace wfg::client::ui
{
    class PatchCanvasComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`, a row as text. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;

            /*  THE PATCH OPENED IN PLUGDATA OR PD (PC.7), and Pd offered for
                download where neither is on the machine. */
            std::function<void (const std::string& cueId)> openInEditor;
            std::function<void()> getEditor;
        };

        PatchCanvasComponent (const model::Theme&, Actions);

        void show (const model::PatchReading& reading);
        void applyTheme (const model::Theme&);

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
        bool keyPressed (const juce::KeyPress&) override;

        /*  FOR A TEST: the picked boxes and lines, by their index in the
            patch; and a point on the canvas as the component draws it. */
        const std::set<std::size_t>& pickedBoxes() const noexcept { return boxes; }
        const std::set<std::size_t>& pickedLines() const noexcept { return lines; }
        juce::Point<float> onScreen (double x, double y) const;
        double zoom() const noexcept { return scale; }

        /*  FOR A TEST: the box being typed into, its editor, and the text the
            clipboard would take. */
        std::optional<std::size_t> typingBox() const noexcept { return typing; }
        juce::TextEditor& typingEditor() noexcept { return editor; }
        juce::TextButton& editorButton() noexcept { return openButton; }
        void commitTyping();

    private:
        model::Theme theme;
        Actions actions;
        juce::TextButton openButton;

        model::PatchReading reading;
        std::string drawnText;              // the text the view was made from
        std::optional<std::string> held;    // sent, and drawn until the tree has it
        process::Patch patch;
        model::PatchView view;

        std::set<std::size_t> boxes;        // picked, by Patch::boxes index
        std::set<std::size_t> lines;        // picked, by Patch::lines index

        double scale = 1.0;
        juce::Point<double> origin;         // the canvas point at the component's top-left

        enum class Hand { none, moving, banding, panning, connecting };
        Hand hand = Hand::none;
        std::size_t connectFrom = 0;        // a view box's index
        int connectOutlet = 0;
        juce::Point<double> pointer { 20.0, 20.0 };   // where a placed box goes

        /*  THE BOX BEING TYPED INTO, by its index in the patch, and the editor
            laid over it. Esc goes on to the PANIC: it never stays here. */
        class Typing final : public juce::TextEditor
        {
        public:
            bool keyPressed (const juce::KeyPress& key) override
            {
                if (key.getKeyCode() == juce::KeyPress::escapeKey)
                    return false;
                return juce::TextEditor::keyPressed (key);
            }
        };
        Typing editor;
        std::optional<std::size_t> typing;

        void startTyping (std::size_t box);
        void place (model::Placed what);
        void copyPicked();
        void paste (const juce::String& piece);
        juce::Point<double> downAt, now;    // canvas points
        juce::Point<float> panFrom;
        juce::Point<double> originAtDown;

        void rebuild (const std::string& text);
        juce::Point<double> toCanvas (juce::Point<float> screen) const;
        void write (const std::string& text);
        bool editable() const;
        void zoomAbout (juce::Point<float> screen, double factor);
        void drawBox (juce::Graphics&, const model::PatchBoxView&, juce::Point<double> offset, bool picked) const;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchCanvasComponent)
    };
}
