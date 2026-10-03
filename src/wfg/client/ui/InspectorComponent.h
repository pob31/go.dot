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

#include <wfg/client/model/Icons.h>
#include <wfg/client/model/Inspector.h>
#include <wfg/client/model/Theme.h>
#include <wfg/client/ui/Icons.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
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

            /*  A CLICK OR A TOUCH ON A NUMBER puts it on the surfaces' master
                dial (author, 2026-09-26), by the address the node gave. */
            std::function<void (const std::string& address)> dial;
        };

        InspectorComponent (const model::Theme& theme, Actions actions);
        ~InspectorComponent() override;

        /** The cue to show. Cheap when it is the one already drawn. */
        void show (const model::Inspection& inspection);

        void applyTheme (const model::Theme& theme);

        /*  WHICH PANEL THE FOOT HAS OPEN AND ON WHICH CUE, by the subject's
            word: the panel bar's button for it is lit while it is this cue's
            (author, 2026-09-30: the toggles "at the top"). Empty for none. */
        void showFoot (const std::string& subject, const std::string& cueId);

        /*  THE NUMBER THE MASTER DIAL TURNS, as the tree says: its line wears
            a dial before its name and a frame round its value - a mark and a
            line, never a colour alone (§4.8). Empty marks nothing. */
        void showDial (const std::string& address);

        void paint (juce::Graphics& g) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent& event) override;

        /*  A PRESS THAT LANDED ON `hit`: what mouseDown forwards to, and what a
            test drives without inventing a mouse event. */
        void pressedOn (const juce::Component* hit);

    private:
        struct Line;

        /*  A DRAWER'S HEAD: its twist, its icon, its name, and while it is
            shut how many rows are in it. Pressing it opens or shuts it. */
        class DrawerHead;

        /** A line's name, with the dial's mark when the dial is on it. */
        juce::String nameOf (const model::Field& field) const;
        void markDial();
        std::string dialed;

        void rebuild (const model::Inspection& inspection);
        void layOut();

        /*  THE DRAWERS (author, 2026-09-30: "we can also make more drawers
            for things"): every block of rows is one, and the details are one
            more. What a hand chose is remembered by the drawer's heading for
            as long as the window is open, so shutting "when" on one cue shuts
            it on the next. A drawer nobody has touched is open - unless every
            row in it means nothing for this cue, like a sampler's rows on a
            cue that is not in a sampler group, which start shut with their
            count showing. The details start shut, as the fold always did. */
        std::map<std::string, bool> drawerChosen;
        bool isShut (const std::string& drawer) const;
        void toggleDrawer (const std::string& drawer);

        /** The drawers' shut states as a string, for noticing that a refill changed one. */
        std::string drawerStates() const;

        /*  THE PANEL BAR: one button per panel this cue has at the foot, at
            the head of the inspector and out of the scrolling, lit while its
            panel is open on this cue. Rebuilt only when the set changes. */
        std::vector<std::unique_ptr<IconButton>> panelButtons;
        std::vector<std::string> panelWords;
        void rebuildPanels (const std::vector<model::Field>& panels);
        void lightPanels();
        int panelBarHeight() const noexcept;
        std::string footWord, footCue;

        /*  WHAT THE HEAD IS DRAWN WITH beside its words: the cue's icon in its
            kind's accent, and the cue's own colour as a tab, when it has one. */
        model::Icon headIcon = model::Icon::none;
        std::string headAccent;
        std::string headColour;
        void readHead (const model::Inspection& inspection);
        juce::Rectangle<int> headIconBox;

        /** The drawers' states as last laid out, so a refill relays only when one moved. */
        std::string laidDrawers;

        /*  ONE COMMIT, N WRITES: a field over several cues carries every
            cue's address and each is written, which is what a batch edit is
            (§4.11: N decisions). One cue is one write. */
        void commitField (const model::Field& field, const std::string& text);
        void commitCueRef (const model::Field& field, const std::string& text);

        /** What a box shows for a field: its value, or that the cues disagree. */
        static juce::String shown (const model::Field& field);

        /*  Whether the box shows what an empty row stands for rather than a
            value: drawn in the faint ink, and committed unchanged it writes
            nothing. */
        static bool standsIn (const model::Field& field)
        {
            return ! field.mixed && field.value.empty() && ! field.placeholder.empty();
        }

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
        std::unique_ptr<DrawerHead> detailsHead;
        juce::TextButton closeButton { "x" };

        std::vector<std::unique_ptr<Line>> lines;
        std::string drawnCue;

        /*  THE SHAPE OF THE PANEL AND NOT THE CUE IN IT, which is what stops
            it blinking (author, 2026-09-22: "is there a way so that the
            inspector when being rebuilt doesn't disappear and reappear?").

            Picking one media cue and then another used to tear every row down
            and build it again, because the key was the cue's identifier - and
            two media cues have exactly the same rows, in the same order, with
            different values in them. So the key is the SHAPE: each row's name,
            its control and whether it can be written. Same shape, same panel,
            and the values are set the way a poll sets them.

            The cost is that the lines outlive the cue they were built for, so
            nothing may capture that cue by value - see the buttons, which read
            `drawnCue` when they are pressed rather than remembering it. */
        std::string drawnShape;

        /** The panel's shape, for deciding rebuild against refill. */
        static std::string shapeOf (const model::Inspection&);

        /** What the head says: the cue's name and kind, or that nothing is picked. */
        static juce::String headingFor (const model::Inspection&);
        std::string drawnKind;

        int rowHeight() const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InspectorComponent)
    };
}
