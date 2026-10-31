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
    A JUCE pointer as the model's key (namespace draft §125, BFE): the one
    place a `juce::MouseInputSource` is turned into `model::pointerKey`, so
    every surface that takes several fingers keys them alike.
*/

#include <wfg/client/model/Pointers.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace wfg::client::ui
{
    inline int pointerKeyOf (const juce::MouseInputSource& source)
    {
        const auto type = source.getType();

        const auto kind = type == juce::MouseInputSource::InputSourceType::touch ? model::PointerKind::touch
                        : type == juce::MouseInputSource::InputSourceType::pen   ? model::PointerKind::pen
                                                                                  : model::PointerKind::mouse;

        return model::pointerKey (kind, source.getIndex());
    }

    /*  How many pointers JUCE says are down now: asked at a press, so one
        means the press is the only one and anything still listed lost its
        lift (`model::PointerTurns::sweep`). */
    inline int draggingPointers()
    {
        return juce::Desktop::getInstance().getNumDraggingMouseSources();
    }
}
