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

#include <wfg/client/ui/SurfacePanelComponent.h>

#include <wfg/client/model/Fader.h>
#include <wfg/client/model/Gestures.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/ui/Look.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wfg::client::ui
{
    namespace
    {
        /*  THE SIZES, in pixels before the theme's type scale. A column is a
            little narrower than a send-mixer strip: a D700 has sixteen of them
            and a panel that could not show one bank on a laptop would be a
            panel nobody opened. */
        constexpr int columnWidth = 64;
        constexpr int columnGap = 4;
        constexpr int bandGap = 14;       // between two surfaces
        constexpr int bandHeight = 26;    // a surface's name over its strips
        constexpr int margin = 6;

        /*  WHAT A NUMBER KEY PRESSES WITH. A key has no height to strike at,
            and 100 is a firm hit that still leaves a clip's velocity mapping
            room above it (author's brief, 2026-09-23). */
        constexpr int keyVelocity = 100;

        /*  WHICH COLUMN A KEY IS, and which key to watch for its release.

            THE TOP ROW'S KEY CODES ARE WATCHED AND NOT ITS CHARACTERS. On the
            author's own AZERTY keyboard the key marked 1 types "&" unless
            shift is held, so a character is not something a hand can be
            relied on to produce; the physical key is what was pressed, and
            '1' to '8' are also what the operating system calls those keys
            whatever the layout says they type. The number pad is its own set
            of keys and is watched as such. Nothing with ctrl, alt or command
            held: those are the menu's. */
        struct Digit
        {
            int number = 0;
            int watch = 0;
        };

        Digit digitOf (const juce::KeyPress& key)
        {
            const auto mods = key.getModifiers();

            if (mods.isCtrlDown() || mods.isAltDown() || mods.isCommandDown())
                return {};

            const auto code = key.getKeyCode();

            const int numberPad[] { juce::KeyPress::numberPad1, juce::KeyPress::numberPad2,
                                    juce::KeyPress::numberPad3, juce::KeyPress::numberPad4,
                                    juce::KeyPress::numberPad5, juce::KeyPress::numberPad6,
                                    juce::KeyPress::numberPad7, juce::KeyPress::numberPad8 };

            for (auto at = 0; at < 8; ++at)
                if (code == numberPad[at])
                    return { at + 1, code };

            const auto typed = static_cast<int> (key.getTextCharacter());

            for (const auto candidate : { typed, code })
                if (candidate >= '1' && candidate <= '8')
                    return { candidate - '0', candidate };

            return {};
        }

        /*  A LEVEL AS `node.set` CARRIES IT: a tenth of a decibel, which is
            finer than anybody rides a fader and what `faderText` shows back,
            through the locale-free formatter. The added nought is not a slip:
            rounding a hair below zero gives negative zero, which the formatter
            spells "-0", and adding a positive zero turns it into the zero it
            spells "0". */
        double tenthOf (double decibels)
        {
            return std::round (std::clamp (decibels, model::silenceDb, model::loudestDb) * 10.0) / 10.0 + 0.0;
        }
    }

    //==============================================================================
    SurfacePanelComponent::SurfacePanelComponent (const model::Theme& themeToUse,
                                                  std::function<void (Event)> sendToUse)
        : theme (themeToUse), send (std::move (sendToUse))
    {
        viewport.setViewedComponent (&canvas, false);
        viewport.setScrollBarsShown (false, true);
        viewport.setWantsKeyboardFocus (false);
        canvas.setWantsKeyboardFocus (false);
        addAndMakeVisible (viewport);

        /*  THE KEYS ARE THIS PANEL'S, so it takes the keyboard when it is
            clicked; the canvas and the viewport pass a click's focus up to it
            rather than keeping it. */
        setWantsKeyboardFocus (true);
    }

    SurfacePanelComponent::~SurfacePanelComponent() = default;

    int SurfacePanelComponent::scaled (int base) const
    {
        return juce::roundToInt (static_cast<double> (base) * theme.type);
    }

    //==============================================================================
    void SurfacePanelComponent::show (const std::vector<model::SurfaceRow>& surfacesNow,
                                      const std::vector<model::StripRow>& stripsNow)
    {
        /*  THE HAND'S LATEST FIRST, before the readings move: a fader dragged
            since the last pass sends where it got to, once, on this pass's
            clock - which is what keeps a ride to one `node.set` a pass however
            fast the mouse reports (the send mixer's arrangement, and the
            bridge's coalescing for the hardware, §16.6). */
        flush();

        std::vector<model::StripRow> ordered;
        std::vector<Band> laidOut;
        std::string drawn;

        /*  SURFACE BY SURFACE, IN THE ORDER THE SHOW DECLARES THEM, each one's
            strips by index: the order a sampler group fills strips in (§16.5),
            so member three of a bank is the third column. A surface with no
            strips has nothing to play and draws no band. */
        for (const auto& surface : surfacesNow)
        {
            auto mine = model::stripsOf (stripsNow, surface.id);

            if (mine.empty())
                continue;

            /*  AND WHETHER IT CAN BE REACHED, in words beside its name: the
                panel is where somebody plays a surface whose hardware is not
                there (§3.17), so it says which one that is and why, rather than
                leaving them to find out from a motor that does not move. */
            Band band;
            band.label = surface.label();

            if (const auto state = surface.stateWord(); state != "connected")
                band.label += " \xc2\xb7 " + state;

            band.first = ordered.size();
            band.count = mine.size();
            laidOut.push_back (band);

            drawn += surface.id + ":" + std::to_string (band.count) + "|";

            for (auto& strip : mine)
                ordered.push_back (std::move (strip));
        }

        strips = std::move (ordered);
        bands = std::move (laidOut);

        /*  WHAT DECIDES WHERE THINGS ARE: which strips, and which of them have
            a fader. A word or a level changing moves nothing and costs a
            repaint; a strip appearing costs a layout. */
        for (const auto& strip : strips)
            drawn += strip.id + (strip.endpoint == "gate" ? "g" : "f");

        if (drawn != shape)
        {
            shape = drawn;
            layOut();
        }

        canvas.repaint();
    }

    int SurfacePanelComponent::wantedWidth() const
    {
        if (strips.empty())
            return 0;

        auto width = scaled (margin) * 2;

        for (std::size_t at = 0; at < bands.size(); ++at)
        {
            const auto count = static_cast<int> (bands[at].count);

            width += count * scaled (columnWidth) + juce::jmax (0, count - 1) * scaled (columnGap);

            if (at > 0)
                width += scaled (bandGap);
        }

        return width;
    }

    void SurfacePanelComponent::layOut()
    {
        columnX.assign (strips.size(), 0);

        auto x = scaled (margin);

        for (std::size_t at = 0; at < bands.size(); ++at)
        {
            if (at > 0)
                x += scaled (bandGap);

            const auto& band = bands[at];

            for (std::size_t column = band.first; column < band.first + band.count && column < columnX.size(); ++column)
            {
                columnX[column] = x;
                x += scaled (columnWidth);

                if (column + 1 < band.first + band.count)
                    x += scaled (columnGap);
            }
        }

        /*  AS WIDE AS THE DESK, and at least as wide as the window so the
            ground under a short desk is drawn; as tall as the viewport shows,
            asked twice because the sideways scrollbar appearing is what
            decides how tall that is. */
        canvas.setSize (juce::jmax (1, wantedWidth()), juce::jmax (1, viewport.getHeight()));
        canvas.setSize (juce::jmax (wantedWidth(), viewport.getMaximumVisibleWidth()),
                        juce::jmax (1, viewport.getMaximumVisibleHeight()));
        canvas.repaint();
    }

    void SurfacePanelComponent::resized()
    {
        viewport.setBounds (getLocalBounds());
        layOut();
    }

    void SurfacePanelComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-in"));
    }

    //==============================================================================
    SurfacePanelComponent::Parts SurfacePanelComponent::partsOf (std::size_t column) const
    {
        Parts parts;

        if (column >= strips.size() || column >= columnX.size())
            return parts;

        parts.column = juce::Rectangle<int> (columnX[column], scaled (bandHeight), scaled (columnWidth),
                                             juce::jmax (0, canvas.getHeight() - scaled (bandHeight)
                                                              - scaled (margin)));

        auto area = parts.column.reduced (scaled (4), scaled (4));

        parts.number = area.removeFromTop (scaled (16));
        parts.label = area.removeFromTop (scaled (28));
        parts.word = area.removeFromTop (scaled (18));
        area.removeFromTop (scaled (6));

        /*  A PAD STRIP IS ALL PAD. A pad controller's strip has no fader to
            draw (§16.6's profile table: its endpoint is a gate), so the pad
            takes the room a fader would have had - a big target is the whole
            point of a pad. */
        if (strips[column].endpoint == "gate")
        {
            parts.pad = area;
            return parts;
        }

        parts.pad = area.removeFromBottom (juce::jmin (scaled (60), area.getHeight() / 3));
        area.removeFromBottom (scaled (6));
        parts.value = area.removeFromBottom (scaled (16));
        parts.fader = area;

        return parts;
    }

    juce::Rectangle<int> SurfacePanelComponent::throwOf (const Parts& parts) const
    {
        return parts.fader.reduced (scaled (8), scaled (5));
    }

    std::size_t SurfacePanelComponent::columnAt (int x) const
    {
        const auto width = scaled (columnWidth);

        for (std::size_t column = 0; column < columnX.size() && column < strips.size(); ++column)
            if (x >= columnX[column] && x < columnX[column] + width)
                return column;

        return strips.size();
    }

    std::size_t SurfacePanelComponent::columnOf (const std::string& stripId) const
    {
        for (std::size_t column = 0; column < strips.size(); ++column)
            if (strips[column].id == stripId)
                return column;

        return strips.size();
    }

    double SurfacePanelComponent::levelShown (const model::StripRow& strip) const
    {
        /*  THE HAND WHILE A HAND IS ON IT, the engine the rest of the time -
            the send mixer's rule, for the send mixer's reason: a drag is a
            round trip through the tick thread, and a cap drawn from the reading
            alone lags the pointer by it. */
        if (grab.has_value() && grab->strip == strip.id)
            return grab->wantedDb;

        return strip.hasLevel ? strip.levelDb : model::silenceDb;
    }

    //==============================================================================
    juce::Colour SurfacePanelComponent::wordColour (const std::string& word) const
    {
        if (word == "playing" || word == "held")
            return Look::colour (theme, "live");

        if (word == "armed")
            return Look::colour (theme, "standby");

        if (word == "pending")
            return Look::colour (theme, "waiting");

        if (word == "stopping" || word == "closing")
            return Look::colour (theme, "stopping");

        if (word == "dca")
            return Look::colour (theme, "ink-dim");

        return Look::colour (theme, "ink-off");
    }

    juce::Colour SurfacePanelComponent::swatchOf (const model::StripRow& strip) const
    {
        /*  WHAT IT SOUNDS LIKE WHILE IT SOUNDS (PRD §3.30): the holder's
            timbre, "h s l" with the hue in degrees, while the strip says it is
            playing or held - drawn as the D700 lights it (SurfaceBridge's
            `colourFromTimbre`): the hue and the saturation as analysed, which
            is how broad the spectrum is (author, 2026-09-23), at full
            brightness, and silence dark. Otherwise, and whenever the analysis
            has not arrived, the colour somebody gave the cue; and with
            neither, the theme's own off colour rather than a black that would
            read as a colour somebody chose. */
        if ((strip.word == "playing" || strip.word == "held") && ! strip.timbre.empty())
        {
            const auto hsl = model::words (strip.timbre);

            if (hsl.size() == 3)
            {
                const auto hue = osc::parseDouble (hsl[0]);
                const auto saturation = osc::parseDouble (hsl[1]);
                const auto lightness = osc::parseDouble (hsl[2]);

                if (hue.has_value() && saturation.has_value() && lightness.has_value())
                {
                    if (! (*lightness > 0.0))
                        return Look::colour (theme, "ink-off");

                    const auto turn = std::fmod (std::fmod (*hue, 360.0) + 360.0, 360.0) / 360.0;

                    return juce::Colour::fromHSV (static_cast<float> (turn),
                                                  static_cast<float> (std::clamp (*saturation, 0.0, 1.0)),
                                                  1.0f, 1.0f);
                }
            }
        }

        /*  "#rrggbb" AND NOTHING ELSE, asked rather than handed to
            `Colour::fromString`, which answers black for what it cannot read
            (the timeline's rule, for the timeline's reason). */
        const juce::String authored (strip.cueColour);

        if (authored.length() == 7 && authored.startsWithChar ('#')
              && authored.substring (1).containsOnly ("0123456789abcdefABCDEF"))
            return juce::Colour::fromString ("ff" + authored.substring (1));

        return Look::colour (theme, "ink-off");
    }

    void SurfacePanelComponent::paintCanvas (juce::Graphics& g)
    {
        g.fillAll (Look::colour (theme, "panel-in"));

        /*  AN EMPTY DESK SAYS SO, and says where the strips come from. A blank
            window and a window that has stopped answering look the same. */
        if (strips.empty())
        {
            g.setColour (Look::colour (theme, "ink-off"));
            g.setFont (Look::font (theme, 13.0f));
            g.drawFittedText ("This show declares no surfaces. Add one in Show settings, on the Surfaces tab.",
                              canvas.getLocalBounds().reduced (scaled (16)), juce::Justification::centred, 3);
            return;
        }

        //  Each surface's name over its strips.
        for (const auto& band : bands)
        {
            if (band.count == 0 || band.first >= columnX.size())
                continue;

            const auto last = std::min (band.first + band.count - 1, columnX.size() - 1);
            const auto left = columnX[band.first];
            const auto right = columnX[last] + scaled (columnWidth);
            const auto area = juce::Rectangle<int> (left, scaled (4), right - left,
                                                    scaled (bandHeight) - scaled (8));

            g.setColour (Look::colour (theme, "panel-high"));
            g.fillRect (area);

            g.setColour (Look::colour (theme, "ink-dim"));
            g.setFont (Look::font (theme, 12.0f));
            g.drawFittedText (juce::String (band.label), area.reduced (scaled (6), 0),
                              juce::Justification::centredLeft, 1, 0.7f);
        }

        for (std::size_t column = 0; column < strips.size(); ++column)
            paintColumn (g, column);
    }

    void SurfacePanelComponent::paintColumn (juce::Graphics& g, std::size_t column)
    {
        const auto& strip = strips[column];
        const auto parts = partsOf (column);

        g.setColour (Look::colour (theme, "panel"));
        g.fillRect (parts.column);

        //  Its number on its surface, from one: fader one is strip one.
        g.setColour (Look::colour (theme, "ink-faint"));
        g.setFont (Look::font (theme, 11.0f));
        g.drawText (juce::String (strip.index + 1), parts.number, juce::Justification::centred, false);

        /*  WHAT IS ON IT: the cue's short name, else its name; a dca strip's
            DCA; a dash for a free strip (`StripRow::label`). Fitted rather
            than cut, over two lines - the short name is there so nothing has
            to be cut, and a panel has more room than a scribble strip. */
        g.setColour (Look::colour (theme, strip.cue.empty() && strip.role != "dca" ? "ink-off" : "ink"));
        g.setFont (Look::font (theme, 12.0f));
        g.drawFittedText (juce::String (strip.label()), parts.label, juce::Justification::centred, 2, 0.7f);

        /*  THE WORD, ALWAYS, AND THE COLOUR BESIDE IT (§4.8). The colour says
            which sound; the word says what it is doing, and a screen read by
            somebody who does not sort green from amber still says it. */
        {
            auto line = parts.word;
            const auto side = juce::jmin (scaled (10), line.getHeight());
            const auto swatch = line.removeFromLeft (side).withSizeKeepingCentre (side, side);

            g.setColour (swatchOf (strip));
            g.fillRect (swatch);
            g.setColour (Look::colour (theme, "rule"));
            g.drawRect (swatch, 1);

            line.removeFromLeft (scaled (4));

            g.setColour (wordColour (strip.word));
            g.setFont (Look::font (theme, 11.0f));
            g.drawFittedText (juce::String (strip.word.empty() ? std::string ("free") : strip.word), line,
                              juce::Justification::centredLeft, 1, 0.6f);
        }

        /*  THE FADER, where the strip has one: a groove, unity marked, and the
            cap where the level is. A strip riding nothing is drawn at the
            bottom, dimmed, and takes no drag - there is no node for a hand to
            hold. The number under it is always drawn: a fader's position is a
            colour-like fact, and a desk read across a booth is read by its
            numbers. */
        if (! parts.fader.isEmpty())
        {
            const auto track = throwOf (parts);
            const auto riding = ! strip.target.empty();
            const auto grabbed = grab.has_value() && grab->strip == strip.id;

            g.setColour (Look::colour (theme, "rule"));
            g.fillRect (juce::Rectangle<int> (track.getCentreX() - 1, track.getY(), 2, track.getHeight()));

            const auto placeOf = [&track] (double decibels)
            {
                return track.getBottom()
                         - juce::roundToInt (model::fractionForDb (decibels)
                                               * static_cast<double> (track.getHeight()));
            };

            g.setColour (Look::colour (theme, "ink-off"));
            g.drawHorizontalLine (placeOf (0.0), static_cast<float> (track.getX()),
                                  static_cast<float> (track.getRight()));

            const auto level = riding ? levelShown (strip) : model::silenceDb;
            const auto cap = placeOf (level);

            g.setColour (Look::colour (theme, grabbed ? "picked" : riding ? "ink" : "ink-off"));
            g.fillRect (juce::Rectangle<int> (track.getX(), cap - scaled (3), track.getWidth(), scaled (6)));

            g.setColour (Look::colour (theme, riding ? "ink-dim" : "ink-off"));
            g.setFont (Look::font (theme, 11.0f));
            g.drawText (riding && (strip.hasLevel || grabbed) ? juce::String (model::faderText (level))
                                                              : juce::String::fromUTF8 ("\xe2\x80\x93"),
                        parts.value, juce::Justification::centred, false);
        }

        /*  THE PAD, on every strip. Lit while a hand is on it - this one's, or
            the engine's word that a press somewhere is holding it - and the
            word above says which of those it is. */
        const auto byKey = std::any_of (keysDown.begin(), keysDown.end(),
                                        [&strip] (const auto& entry) { return entry.second.strip == strip.id; });
        const auto down = strip.held || byKey || (mousePad.has_value() && mousePad->strip == strip.id);
        const auto pad = parts.pad.toFloat().reduced (1.0f);

        g.setColour (Look::colour (theme, down ? "picked" : "panel-high"));
        g.fillRoundedRectangle (pad, 5.0f);
        g.setColour (Look::colour (theme, down ? "ink" : "rule"));
        g.drawRoundedRectangle (pad, 5.0f, 1.0f);

        /*  AND WHAT PRESSING IT DOES, in words, because nothing else on the
            screen says it. On a sampler strip where it is struck matters -
            the top is a hard hit and the bottom a soft one - and on a dca strip
            the pad is the strip's gate, which puts the trim back at unity. */
        g.setFont (Look::font (theme, 10.0f));
        g.setColour (Look::colour (theme, down ? "ink" : "ink-off"));

        const auto inside = parts.pad.reduced (scaled (3));

        if (strip.role == "dca")
        {
            if (! strip.target.empty())
                g.drawFittedText ("0 dB", inside, juce::Justification::centred, 1, 0.7f);
        }
        else if (inside.getHeight() >= scaled (32))
        {
            g.drawFittedText ("hard", inside, juce::Justification::centredTop, 1, 0.7f);
            g.drawFittedText ("soft", inside, juce::Justification::centredBottom, 1, 0.7f);
        }
    }

    //==============================================================================
    void SurfacePanelComponent::Canvas::paint (juce::Graphics& g)
    {
        owner.paintCanvas (g);
    }

    void SurfacePanelComponent::Canvas::mouseDown (const juce::MouseEvent& event)
    {
        owner.pressed (event);
    }

    void SurfacePanelComponent::Canvas::mouseDrag (const juce::MouseEvent& event)
    {
        owner.dragged (event);
    }

    void SurfacePanelComponent::Canvas::mouseUp (const juce::MouseEvent& event)
    {
        owner.released (event);
    }

    void SurfacePanelComponent::pressed (const juce::MouseEvent& event)
    {
        const auto column = columnAt (event.x);

        if (column >= strips.size())
            return;

        const auto parts = partsOf (column);
        const auto where = event.getPosition();

        if (parts.pad.contains (where))
        {
            /*  HOW FAR UP THE PAD, from the bottom edge: the top of a pad is
                a hard hit, as the top of a fader is loud. */
            const auto tall = static_cast<float> (juce::jmax (1, parts.pad.getHeight()));

            pressPad (column, static_cast<double> ((static_cast<float> (parts.pad.getBottom())
                                                      - event.position.y) / tall));
            return;
        }

        /*  THE FADER IS TAKEN WHERE IT IS, not where the pointer landed: a
            press is a grab, and a grab that jumped the level to the pixel
            under the hand would be a move nobody made. */
        if ((parts.fader.contains (where) || parts.value.contains (where)) && takeFader (column))
            grab->fromY = event.position.y;
    }

    void SurfacePanelComponent::dragged (const juce::MouseEvent& event)
    {
        if (! grab.has_value())
            return;

        const auto column = columnOf (grab->strip);

        if (column >= strips.size())
            return;

        const auto track = throwOf (partsOf (column));

        if (track.getHeight() <= 0)
            return;

        /*  MEASURED FROM WHERE THE HAND WENT DOWN, on the fader's own curve,
            and shift divides the travel by ten - the send mixer's drag, which
            is every fader's drag in this client. */
        const auto moved = (grab->fromY - event.position.y) / static_cast<float> (track.getHeight());
        const auto scale = event.mods.isShiftDown() ? 0.1f : 1.0f;

        dragFader (column, model::fractionForDb (grab->heldDb) + static_cast<double> (moved * scale));
    }

    void SurfacePanelComponent::released (const juce::MouseEvent&)
    {
        if (grab.has_value())
            letGoOfFader();

        if (mousePad.has_value())
        {
            const auto down = *mousePad;
            mousePad.reset();
            letGoOf (down);
            canvas.repaint();
        }
    }

    //==============================================================================
    std::optional<SurfacePanelComponent::Down> SurfacePanelComponent::press (std::size_t column, int velocity)
    {
        if (column >= strips.size())
            return std::nullopt;

        const auto& strip = strips[column];

        Down down;
        down.strip = strip.id;

        /*  A DCA STRIP'S PAD IS ITS GATE, AND THE GATE OF A DCA STRIP PUTS THE
            TRIM BACK AT UNITY (namespace draft §16.6, the virtual profile's gate
            being the drawn pad). `strip.press` is a sampler strip's and the
            engine refuses it on a dca strip, so a click here that sent one
            would be a click that only ever produced an error. One `node.set`
            on the node the fader rides, and nothing to let go of. */
        if (strip.role == "dca")
        {
            if (send && ! strip.target.empty())
                send (gesture::setNode (strip.target, "0"));

            return down;
        }

        if (send)
            send (gesture::pressStrip (strip.id, velocity));

        down.letGo = true;
        return down;
    }

    void SurfacePanelComponent::letGoOf (const Down& down)
    {
        if (down.letGo && send)
            send (gesture::releaseStrip (down.strip));
    }

    void SurfacePanelComponent::pressPad (std::size_t column, double height)
    {
        //  One pointer, one pad: a second press while one is down is not a hand.
        if (mousePad.has_value())
            return;

        const auto velocity = juce::jlimit (1, 127, 1 + juce::roundToInt (std::clamp (height, 0.0, 1.0) * 126.0));

        mousePad = press (column, velocity);
        canvas.repaint();
    }

    void SurfacePanelComponent::releasePad (std::size_t column)
    {
        if (! mousePad.has_value() || column >= strips.size() || strips[column].id != mousePad->strip)
            return;

        const auto down = *mousePad;
        mousePad.reset();
        letGoOf (down);
        canvas.repaint();
    }

    bool SurfacePanelComponent::takeFader (std::size_t column)
    {
        if (column >= strips.size())
            return false;

        const auto& strip = strips[column];

        //  One hand, one fader: already on this one is taken; on another, not.
        if (grab.has_value())
            return grab->strip == strip.id;

        /*  A PAD STRIP HAS NO FADER, and a strip riding nothing has no node
            to hold: both are drawn and neither can be taken. */
        if (strip.endpoint == "gate" || strip.target.empty())
            return false;

        Grab taken;
        taken.strip = strip.id;
        taken.target = strip.target;
        taken.heldDb = strip.hasLevel ? strip.levelDb : model::silenceDb;
        taken.wantedDb = taken.heldDb;
        grab = taken;

        /*  THE TOUCH GOES OUT AT ONCE, before any move: it is what tells the
            engine a hand is on this fader - a dip to the bottom while touched
            is a ride and not a release (§16.5) - and it has to be there before
            the first value is. */
        if (send)
            send (gesture::touchNode (taken.target));

        canvas.repaint();
        return true;
    }

    void SurfacePanelComponent::dragFader (std::size_t column, double fraction)
    {
        if (! takeFader (column))
            return;

        const auto wanted = tenthOf (model::dbForFraction (fraction));

        //  A move that lands on the same tenth is no move, and sends nothing.
        if (std::abs (wanted - grab->wantedDb) < 0.05)
            return;

        grab->wantedDb = wanted;
        grab->unsent = true;
        canvas.repaint();
    }

    void SurfacePanelComponent::endFader (std::size_t column)
    {
        if (! grab.has_value() || column >= strips.size() || strips[column].id != grab->strip)
            return;

        letGoOfFader();
    }

    void SurfacePanelComponent::flush()
    {
        if (! grab.has_value() || ! grab->unsent)
            return;

        grab->unsent = false;

        if (send)
            send (gesture::setNode (grab->target, osc::formatDouble (grab->wantedDb)));
    }

    void SurfacePanelComponent::letGoOfFader()
    {
        if (! grab.has_value())
            return;

        const auto held = *grab;
        grab.reset();

        /*  THE LAST VALUE, THEN THE RELEASE, in that order: the release is what
            the engine's fader-stop reads (released at the bottom, §16.5), so
            the value it reads has to have arrived first. */
        if (send)
        {
            if (held.unsent)
                send (gesture::setNode (held.target, osc::formatDouble (held.wantedDb)));

            send (gesture::releaseNode (held.target));
        }

        /*  AND THE CAP STAYS WHERE THE HAND LEFT IT until the next reading,
            rather than dropping back for a pass to the value the engine held
            before the last write landed. */
        if (const auto column = columnOf (held.strip);
            column < strips.size() && strips[column].target == held.target)
        {
            strips[column].levelDb = held.wantedDb;
            strips[column].hasLevel = true;
        }

        canvas.repaint();
    }

    void SurfacePanelComponent::releaseEverything()
    {
        if (grab.has_value())
            letGoOfFader();

        if (mousePad.has_value())
        {
            const auto down = *mousePad;
            mousePad.reset();
            letGoOf (down);
        }

        const auto held = keysDown;
        keysDown.clear();

        for (const auto& entry : held)
            letGoOf (entry.second);

        canvas.repaint();
    }

    //==============================================================================
    bool SurfacePanelComponent::keyPressed (const juce::KeyPress& key)
    {
        const auto digit = digitOf (key);

        if (digit.number < 1)
            return false;

        const auto column = static_cast<std::size_t> (digit.number - 1);

        /*  A HELD KEY REPEATS, and a repeat is not a second strike: the pad
            is pressed once and let go when the key comes up. */
        if (keysDown.count (column) != 0)
            return true;

        if (auto down = press (column, keyVelocity))
        {
            down->keyCode = digit.watch;
            keysDown.emplace (column, *down);
            canvas.repaint();
        }

        return true;
    }

    bool SurfacePanelComponent::keyStateChanged (bool)
    {
        /*  A KEY THAT IS NO LONGER DOWN LETS ITS PAD GO. Asked of each held
            key rather than inferred from the event, which says only that some
            key changed. */
        auto released = false;

        for (auto entry = keysDown.begin(); entry != keysDown.end();)
        {
            if (juce::KeyPress::isKeyCurrentlyDown (entry->second.keyCode))
            {
                ++entry;
                continue;
            }

            const auto down = entry->second;
            entry = keysDown.erase (entry);
            letGoOf (down);
            released = true;
        }

        if (released)
            canvas.repaint();

        return released;
    }

    void SurfacePanelComponent::focusLost (FocusChangeType)
    {
        /*  THE KEY-UP WILL NOT ARRIVE HERE ANY MORE, so a pad held by a key is
            let go now rather than left pressed - a hold clip would otherwise
            sound until somebody came back to this window. */
        if (keysDown.empty())
            return;

        const auto held = keysDown;
        keysDown.clear();

        for (const auto& entry : held)
            letGoOf (entry.second);

        canvas.repaint();
    }

    //==============================================================================
    SurfaceWindow::SurfaceWindow (const model::Theme& themeToUse, std::function<void (Event)> sendToUse,
                                  std::function<void()> panicToUse)
        : DocumentWindow ("Surfaces", Look::colour (themeToUse, "ground"), juce::DocumentWindow::closeButton),
          panic (std::move (panicToUse))
    {
        panel = std::make_unique<SurfacePanelComponent> (themeToUse, std::move (sendToUse));
        panel->setSize (960, 480);

        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (360, 320, 4000, 1600);
        setContentNonOwned (panel.get(), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    SurfaceWindow::~SurfaceWindow()
    {
        clearContentComponent();
    }

    void SurfaceWindow::refresh (const tree::TreeSnapshot& snapshot)
    {
        /*  HIDDEN, IT READS NOTHING: two walks of the tree a pass are worth
            paying for a desk somebody is looking at, and for nothing else. The
            next pass after it is shown again catches it up. */
        if (! isVisible())
            return;

        panel->show (model::readSurfaces (snapshot), model::readStrips (snapshot));
    }

    void SurfaceWindow::closeButtonPressed()
    {
        panel->releaseEverything();
        setVisible (false);
    }

    bool SurfaceWindow::keyPressed (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress (juce::KeyPress::escapeKey))
        {
            if (panic)
                panic();

            return true;
        }

        //  The number keys, when nothing inside has the keyboard to take them first.
        return panel->keyPressed (key);
    }

    bool SurfaceWindow::keyStateChanged (bool isKeyDown)
    {
        return panel->keyStateChanged (isKeyDown);
    }

    void SurfaceWindow::activeWindowStatusChanged()
    {
        DocumentWindow::activeWindowStatusChanged();

        /*  ANOTHER WINDOW TAKING OVER TAKES THE KEYS AND THE MOUSE WITH IT,
            and neither will report letting go here. Everything held is let go
            now, which is also what the hardware does when a hand leaves it. */
        if (panel != nullptr && ! isActiveWindow())
            panel->releaseEverything();
    }
}
