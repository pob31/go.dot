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
    THIS MACHINE'S DEVICES, LOOKED FOR AGAIN WHEN SOMEBODY ASKS (author,
    2026-10-15: "Can the screen detection, MIDI detection, Audio device
    detection happen in the Show Settings panel when clicking on the drop down
    menu again and not just when the program starts?"; namespace draft §64).

    - `midi.rescan`: the MIDI inputs and outputs listed again, and every
      declared port with no device tried again - one plugged in since start is
      bound now - and every bound port whose device has gone let go of, so it
      says so. Listing MIDI devices blocks for milliseconds on Windows, so it is
      done on the message thread on this explicit ask, never on an idle tick.
    - `serial.rescan`: the serial ports listed at once rather than at the next
      five-second look.

    Both are this machine's alone (namespace draft §58, RD.3): never sent to a
    backup, no step of undo, nothing in the show. Taken and ignored by every
    verb that replays a log, which hands empty hooks.
*/

#include <functional>

namespace wfg { class CommandRegistry; }

namespace wfg
{
    void registerRescanCommands (CommandRegistry& registry, std::function<void()> midi, std::function<void()> serial);
}
