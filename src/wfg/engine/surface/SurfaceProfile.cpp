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

        if (button.button == Button::play)      return Action::go;
        if (button.button == Button::stop)      return Action::stop;
        if (button.button == Button::rewind)    return Action::rewind;
        if (button.button == Button::forward)   return Action::forward;

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
