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
    The inspector: one cue's parameters, and the only place this client types.

    EVERY CONTROL IS BUILT FROM THE NODE and none of them from a table of field
    names: the type tags decide whether it is a switch or a box, the range
    decides its bounds, the closed set of values decides that it is a choice,
    and the description is what it says when somebody hovers it. That is
    §14.2's generic inspector, and it is what lets a row added to the parameter
    table appear here with no line written in this file.

    A COMMIT IS ONE `node.set` AND NOTHING ELSE. Typing changes nothing until
    the field is left or Return is pressed - the page learned that the hard way
    (a save sent from inside a field wrote the show WITHOUT the value still
    being typed) - and what goes out is the address the node carries, never one
    assembled here.

    WHAT IS BEHIND THE FOLD is whatever the engine is reporting rather than
    what anybody decided: read-only rows, by their own ACCESS and never by
    name. The author asked for that on the page in those terms, and a row that
    becomes writable leaves the fold by itself.
*/

#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class InspectorComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One committed field: the node's own address, and the text typed into it. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /*  ASKS THE MACHINE FOR A FILE, which is the one thing a
                browser cannot do (decision Y) and so the one control here
                that is the desktop's alone. It is an ACCELERATOR and never a
                sole route: the same field takes a typed name and sends the
                same `node.set`, which is what keeps §14.16's third rule. */
            std::function<void (const std::string& cueId)> chooseFile;

            /*  A FIELD THAT NAMES A CUE, committed as typed: a number, a name
                or an identifier. The window resolves it to the identifier the
                document stores and says so when it cannot. */
            std::function<void (const std::string& address, const std::string& text)> setCueRef;

            /*  OPENS THE PANEL AT THE FOOT on this cue, named by the subject
                the field carries. The inspector does not know what a foot
                panel is and must not: it passes on a word and a cue, and the
                window turns the pair into a `Subject`. */
            std::function<void (const std::string& cueId, const std::string& subject)> openPanel;

            /** Picks nothing, which is what closes this panel. */
            std::function<void()> close;
        };

        InspectorComponent (const model::Theme& theme, Actions actions);
        ~InspectorComponent() override;

        /** The cue to show. Cheap when it is the one already drawn. */
        void show (const model::Inspection& inspection);

        void applyTheme (const model::Theme& theme);

        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        struct Line;

        void rebuild (const model::Inspection& inspection);

        /** Puts the fold's own state on its button, as a twist rather than a colour. */
        void sayWhetherDetailsAreOpen();
        void layOut();

        /*  ONE COMMIT, N WRITES: a field over several cues carries every
            cue's address and each is written, which is what a batch edit is
            (§4.11: N decisions). One cue is one write. */
        void commitField (const model::Field& field, const std::string& text);
        void commitCueRef (const model::Field& field, const std::string& text);

        /** What a box shows for a field: its value, or that the cues disagree. */
        static juce::String shown (const model::Field& field);

        /*  Which item of a `busRef` menu stands for what the row currently
            says, counting from one as a ComboBox does. Nought when the stored
            identifier is in the menu nowhere - an output deleted out from
            under a cue - which leaves the menu showing nothing rather than
            silently picking the first one. */
        static int idForChoice (const model::Field& field);

        Actions actions;
        model::Theme theme;

        juce::Viewport viewport;
        juce::Component content;
        juce::Label heading;
        juce::TextButton detailsButton { "details" };
        juce::TextButton closeButton { "x" };

        std::vector<std::unique_ptr<Line>> lines;
        std::string drawnCue;
        std::size_t drawnFields = 0;
        bool detailsOpen = false;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InspectorComponent)
    };
}
