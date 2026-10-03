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
    WHAT A FADE MOVES, AND WHERE TO, as a mixer (namespace draft §26, PG; the
    author, 2026-10-03: "What is already represented as a slider should be a
    slider in the foot panel when editing a fade. EQ and inserted effects
    should open the EQ interface or inserted effect UI. Changing a parameter
    highlights the parameter to show the fade will affect it. Removing a
    changed value [...] will remove the changed value").

    THE STRIPS ARE THE SEND MIXER'S, for a fade: the target's level (or the
    DCA's trim), its speed when it plays a file, and a send into every mix.
    Each has a tick box under its number. A strip the fade moves is ticked, its
    cap in the accent and the word under it MOVES; one it leaves alone is
    clear, its cap dim, the word STAYS, and its number is what the target holds
    now. Moving a strip ticks it - the highlight - and clearing the box takes
    the entry out. The words carry it; the colour helps (§4.8).

    ON THE RIGHT, THE DOORS AND THE LIST: EQ... opens the EQ panel on the fade,
    a button per insert the target has opens that plugin's own window on the
    fade, Curve... the drawn shape; and under them every EQ number and plugin
    value the fade moves, by name, each with its tick box to clear.

    WHILE A HAND IS DOWN THE HAND IS DRAWN - the send mixer's rule, for its
    reason.
*/

#include <wfg/client/model/Foot.h>
#include <wfg/client/model/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wfg::client::ui
{
    class FadeMixerComponent final : public juce::Component
    {
    public:
        struct Actions
        {
            /** One `node.set`: a fade row, or one entry through the fade's door. */
            std::function<void (const std::string& address, const std::string& text)> set;

            /** The EQ panel, on this fade. */
            std::function<void (const std::string& fadeId)> openEq;

            /** The drawn curve, on this fade. */
            std::function<void (const std::string& fadeId)> openCurve;

            /** The plugin's own window, on this fade. */
            std::function<void (const std::string& fadeId, const std::string& pluginId)> editPlugin;

            /** A sentence for the panel's head. */
            std::function<void (const juce::String&)> say;
        };

        FadeMixerComponent (const model::Theme&, Actions);
        ~FadeMixerComponent() override;

        void applyTheme (const model::Theme&);

        /** The reading for this pass. Cheap when the mixer has not changed shape. */
        void show (const model::FootReading&);

        void paint (juce::Graphics&) override;
        void resized() override;

        /*  WHAT A HAND DOES, by the strip's index, for the mouse and for a
            test: a value asked for (which ticks the strip), and the tick box. */
        void valueWanted (std::size_t strip, double value);
        void tickWanted (std::size_t strip, bool moves);
        void untickMove (std::size_t row);

        const model::FadeMixReading& mix() const noexcept { return reading.fadeMix; }

    private:
        struct Strip;
        struct MoveRow;

        void rebuild();
        void refresh();
        std::string shapeOf() const;

        static std::string valueText (const model::FadeStrip&, double value);

        model::Theme theme;
        Actions actions;
        model::FootReading reading;

        std::vector<std::unique_ptr<Strip>> strips;
        std::vector<std::unique_ptr<MoveRow>> rows;
        std::vector<std::unique_ptr<juce::TextButton>> doors;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FadeMixerComponent)
    };
}
