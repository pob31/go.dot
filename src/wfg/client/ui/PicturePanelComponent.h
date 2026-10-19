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
    A VIDEO CUE'S PICTURE AT THE FOOT (namespace draft §47, AAG).

    The author, 2026-10-09: "Could we have another foot panel with all colour
    and geometry adjustments rather listing them in the inspector. When this
    panel is open the video monitor window could also be open."

    ON THE LEFT, THE CANVAS: its shape, and on it the picture's frame where
    the projector draws it (model/Picture, on `video::Placement`). The frame is
    dragged to move the picture, by a corner to scale it, by the round handle
    above it to turn it - Shift turns in fifteen-degree steps - and the arrow
    keys nudge it, a tenth of a percent, one with Shift. A mask's outline is
    drawn and dragged instead of a frame: a double click on an edge adds a
    corner, a right click on a corner takes it away.

    ON THE RIGHT, THE NUMBERS: the fit, the flips, the scale, offsets and
    turn; for a picture, a movie or a capture the grade - contrast,
    saturation, gamma, hue - and its four curves, drawn, one picked by its
    letter; for a fill or a mask its colour, from a swatch; for a mask its
    feather and whether it is turned inside out. A picture's, a movie's or a
    capture's KEY (namespace draft 61) shares the grade's column, picked
    above it - Grade or Key, the pick saying in words when a key is on: its
    alpha switch, the key by name, where it cuts and how softly, the screen's
    colour or the shape's, the spill, turned round. A TEXT CUE (61) has its
    words where the fit is, typed and written when the hand leaves them, and
    their style in the colour column: the font, the letters' size, bold and
    italic, their colour, the alignments, the spacing, the box they wrap in,
    the outline's colour, width and blur. Its FIELDS (61, AKM) are picked
    beside the font: each blank, its value tonight - typed and sent at once,
    under the lock too, since a field is never the show's - its address, and
    a button to copy it for whoever sends.

    A drag is a run of `node.set` on one address, which the document folds
    into one undo step - the offsets one `node.setMany`, their two addresses
    together. While a hand is down the hand's numbers are drawn, the document
    catching up behind. Under the lock it shows, and changes nothing.

    ON A FADE (namespace draft §94) it is the target's picture where the fade
    takes it: the frame drawn where it arrives, and only what a fade moves -
    how solid, the place, size and turn, a movie's speed, a blur cue's blur -
    each with a tick saying whether the fade moves it, written through the
    fade's door (`/godot/cue/<fade>/moves/video/<row>`). The rest is the
    cue's, and not drawn here.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Picture.h>
#include <wfg/client/model/Theme.h>
#include <wfg/engine/audio/MediaInfo.h>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    class PicturePanelComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`, a row as text. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** Several rows at once: one `node.setMany`, one undo step for the drag it is part of. */
            std::function<void (const std::vector<std::pair<std::string, std::string>>&)> setMany;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;

            /*  THE SHOW'S FONTS (namespace draft §62, ALA): an installed family
                carried, a file added, a family taken out - `restricted` once
                the operator has said yes to a licence restricting it. */
            std::function<void (const std::string& family, bool restricted)> carryFont;
            std::function<void (const std::string& path, bool restricted)> addFontFile;
            std::function<void (const std::string& family)> removeFont;
        };

        PicturePanelComponent (const model::Theme&, Actions);
        ~PicturePanelComponent() override;

        void show (const model::FootReading& reading, std::shared_ptr<const audio::MediaRecords> media);
        void applyTheme (const model::Theme&);

        /*  THE PICKED CUE'S PICTURE, as the monitor draws it (§47, AAH): drawn
            under the frame when there is one, so the hand sees what it moves. */
        void setPicture (const juce::Image& picture);

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        class View;
        class Curves;

        model::Theme theme;
        Actions actions;

        model::PictureReading reading;
        std::unique_ptr<View> view;
        std::unique_ptr<Curves> curves;

        juce::TextButton fitButtons[3];
        juce::ToggleButton flipH { "Flip across" }, flipV { "Flip up and down" }, invert { "Inside out" };
        juce::Slider scale, offsetX, offsetY, rotation, contrast, saturation, gamma, hue, feather;
        juce::Label scaleLabel, offsetXLabel, offsetYLabel, rotationLabel,
                    contrastLabel, saturationLabel, gammaLabel, hueLabel, featherLabel, paintLabel;
        juce::TextButton paintSwatch;
        juce::TextButton curvePicks[4];
        juce::TextButton curveReset { "Straight" };
        int curvePicked = 0;

        /*  THE GRADE OR THE KEY, in the one column (namespace draft 61). */
        juce::TextButton colourPages[2];
        int colourPage = 0;
        juce::ToggleButton useAlpha { "Use its alpha" }, keyInvert { "Turned round" };
        juce::ComboBox keyMode;
        juce::Slider keyLevel, keySoftness, keySpill;
        juce::Label keyLevelLabel, keySoftnessLabel, keySpillLabel, keyColourLabel;
        juce::TextButton keyColourSwatch;

        /*  A TEXT CUE'S WORDS AND STYLE (namespace draft 61). */
        juce::TextEditor wordsBox;
        juce::ComboBox fontMenu;
        juce::StringArray fonts;
        juce::ToggleButton bold { "Bold" }, italic { "Italic" };
        juce::TextButton alignButtons[3];
        juce::ComboBox alignVMenu;
        juce::Slider fontSize, lineSpacing, wrap, outlineWidth, outlineBlur;
        juce::Label fontSizeLabel, lineSpacingLabel, wrapLabel, outlineLabel, outlineBlurLabel;
        juce::TextButton letterSwatch, outlineSwatch;
        bool fontsFilled = false;
        std::string fontShown;

        /*  THE FONTS THE SHOW CARRIES (namespace draft §62) as the menu last
            listed them, and the fonts.* job this panel is waiting for: its
            subject, and the job number seen when it was asked. */
        std::string carriedShown;
        std::string fontJobWaiting;
        unsigned fontJobSeen = 0;
        std::unique_ptr<juce::FileChooser> fontChooser;
        void askFontJob (const std::string& verb, const std::string& subject, bool restricted);
        void followFontJob();
        bool carries (const std::string& family) const;

        /*  A TEXT CUE'S FIELDS (61, AKM), picked beside the font. */
        struct FieldRow
        {
            std::string name, address;
            juce::Label label;
            juce::TextEditor value;
            juce::TextButton copy { "Copy" };
        };

        /*  A BLUR CUE'S (61, AKN): how far, and whether an outline limits it. */
        juce::Slider blurAmount;
        juce::Label blurAmountLabel;

        /*  ON A FADE (§94): how solid and a movie's speed, which the cue's own
            panel leaves to the inspector; a tick per value a fade moves, in
            `fadeRowNames`' order; and a sentence saying what the panel is. */
        juce::Slider opacity, speed;
        juce::Label opacityLabel, speedLabel, fadeWords;
        juce::ToggleButton fadeTicks[7];
        juce::Slider* fadeSlider (int at);
        juce::Label* fadeLabel (int at);
        void tickFade (int at, bool moves);

        /** Where a row is written: the cue's own, or the fade's door for it. */
        std::string addressOf (const char* row) const;
        juce::TextButton wholeCanvas { "Whole canvas" }, aShape { "A shape" };

        juce::TextButton fieldsPick;
        juce::Label noFields;
        std::vector<std::unique_ptr<FieldRow>> fieldRows;
        bool fieldsPage = false;
        void refreshFields();
        void sendField (FieldRow& row);

        void build (juce::Slider& slider, juce::Label& label, const char* words, const char* row,
                    double low, double high, double mid, bool symmetric, const char* suffix, int decimals);
        void write (const char* row, const std::string& text);
        void writeMany (const std::vector<std::pair<std::string, std::string>>& rows);
        void writeCurve (int which, const model::CurvePoints& points);
        void refresh();
        void chooseColour (const char* row, const std::string& current, juce::Component& from);
        bool keyPage() const noexcept { return reading.keyable() && colourPage == 1; }
        void writeWords();
        void refreshFonts();
        juce::Rectangle<int> controlsArea() const;

        friend class View;
        friend class Curves;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PicturePanelComponent)
    };
}
