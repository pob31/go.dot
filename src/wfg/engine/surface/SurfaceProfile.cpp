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

#include <wfg/engine/surface/SurfaceProfile.h>

#include <optional>
#include <string_view>

namespace wfg::surface
{
    static_assert (colourIntervalTicks >= 1, "a colour write needs at least a tick to itself");
    static_assert (doubleStopTicks >= 1, "a double STOP needs a window");
    static_assert (7 * (pageBlinkOnTicks + pageBlinkOffTicks) <= pageBlinkCycleTicks,
                   "seven blinks must fit in a page button's cycle");

    std::optional<Profile> profileFor (std::string_view word)
    {
        if (word == "virtual")   return Profile::virtualPanel;
        if (word == "mcu")       return Profile::mcu;
        if (word == "d700")      return Profile::d700;
        if (word == "midiPads")  return Profile::midiPads;

        return std::nullopt;
    }

    std::string_view profileWord (Profile profile) noexcept
    {
        switch (profile)
        {
            case Profile::virtualPanel: return "virtual";
            case Profile::mcu:          return "mcu";
            case Profile::d700:         return "d700";
            case Profile::midiPads:     return "midiPads";
        }

        return "virtual";
    }

    Topology topologyOf (Profile profile) noexcept
    {
        Topology topology;

        switch (profile)
        {
            case Profile::virtualPanel:
                /*  What the panel draws is the client's business (§16.7); the
                    bridge sends it nothing and reads nothing from it. */
                topology.stripsPerPort = 0;
                topology.hasEncoders = false;
                topology.hasDisplay = false;
                topology.drivenOverMidi = false;
                break;

            case Profile::mcu:
                topology.hasMeters = true;
                break;

            case Profile::d700:
                /*  Mackie, and on top of it the D700's own: an RGB surround on
                    every encoder, three native rows and a number field - and
                    faders that land on their engraving (+7 at the top). */
                topology.hasRgb = true;
                topology.nativeDisplay = true;
                topology.hasMeters = true;
                topology.faderLaw = FaderLaw::d700;
                break;

            case Profile::midiPads:
                /*  A pad per strip, a note each from `firstNote`, on whichever
                    of its ports the controller sends. No motor, no touch, no
                    display: a pad is a fader without a motor (decision AA). */
                topology.stripsPerPort = 0;
                topology.hasFaders = false;
                topology.hasTouch = false;
                topology.hasEncoders = false;
                topology.hasDisplay = false;
                topology.hasPads = true;
                break;
        }

        return topology;
    }

    Action actionFor (Profile profile, ButtonId button) noexcept
    {
        switch (profile)
        {
            case Profile::mcu:
            case Profile::d700:
                break;

            case Profile::virtualPanel:
            case Profile::midiPads:
                return Action::none;
        }

        /*  A per-strip button names its strip in `index`; one that does not is
            not a gate anybody pressed. */
        if (button.button == Button::vpotPress)
            return button.index >= 0 ? Action::gate : Action::none;

        if (button.button == Button::mute)
            return button.index >= 0 ? Action::kill : Action::none;

        if (button.button == Button::solo)
            return button.index >= 0 ? Action::solo : Action::none;

        if (button.button == Button::rec)
            return button.index >= 0 ? Action::startLevel : Action::none;

        if (button.button == Button::select)
            return button.index >= 0 ? Action::aim : Action::none;

        if (button.button == Button::assignEq)      return Action::eqPage;
        if (button.button == Button::assignSend)    return Action::sendPage;

        /*  FX, the Mackie "Plug-In" (2026-09-26): the aimed cue's inserts. */
        if (button.button == Button::assignPlugin)  return Action::fxPage;

        /*  THE MASTER DIAL'S CLICK AND DOUBLE CLICK (author, 2026-09-26: "click
            could be deselect and double click back to default" - "The D700
            can do it at hardware level"). The dial presses F3; with double
            click ticked for it in the Configurator the firmware withholds the
            click until the gesture resolves and sends a double as another note
            alone - F4, by `*`'s pattern (F1 single, F2 double), to confirm at
            the bench. A Mackie's F3 and F4 are its own function keys and stay
            unmapped. */
        if (profile == Profile::d700 && button.button == Button::function)
        {
            if (button.index == 2) return Action::dialLetGo;
            if (button.index == 3) return Action::dialRest;
        }

        /*  `*`, under the Mackie preset the D700 is pinned to: F1, and F2 for
            its double press when the Configurator is asked for one. */
        if (button.button == Button::function && (button.index == 0 || button.index == 1))
            return Action::leavePage;

        if (button.button == Button::play)      return Action::go;
        if (button.button == Button::stop)      return Action::stop;
        if (button.button == Button::rewind)    return Action::rewind;
        if (button.button == Button::forward)   return Action::forward;

        /*  THE D700'S ARROWS MOVE THE STANDBY (author, 2026-09-26: "Can the
            up(-left) and down(-right) arrows on the D700 be used to move the
            standby cursor?"). They send the Mackie bank notes, and the D700 has
            no rewind or forward, so without this nothing on it moved the
            pointer but GO. A Mackie keeps its bank arrows for banking (§3.9d);
            it has the transport's own pair. */
        if (profile == Profile::d700)
        {
            if (button.button == Button::bankLeft)  return Action::rewind;
            if (button.button == Button::bankRight) return Action::forward;
        }

        return Action::none;
    }

    int meterStepFor (double db) noexcept
    {
        int step = 0;

        for (const auto from : meterStepsDb)
            if (db >= from)
                ++step;

        return step;
    }
}
