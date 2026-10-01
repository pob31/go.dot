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
    WHAT GO DOH! DOES WITH WHAT LEFT GO.DOT FOR A DEVICE (PRD §3.32, namespace
    draft §24; the author, 2026-10-01).

    His case: OSC cues to a light board that started sequences, moving heads
    repositioning before being lit - "fixing with Doh would really do more
    damage than the light operator handling the damage". So every network
    device and every MIDI port carries one setting, `leave` (left to its
    operator - the default for every device, whatever its kind) or `takeBack`,
    and an OSC or MIDI cue may override its device either way (`device` follows
    it). These are the readers, and nothing else: the rows are the document's,
    and what each value does is the Runner's.

    READ FROM THE DOCUMENT AND NEVER FROM THE MOUNT TABLE, because a replay may
    have no mount table at all and a Doh decision has to be the same there. A
    setting changed is a logged `node.set`, so every reading is replay-exact.

    READ FAIL-SAFE, because this repo spells its defaults by hand and the
    comparison decides which way an old or misspelt show falls: a device takes
    back only on the exact word `takeBack`, and a cue overrides only on the
    exact words `takeBack` or `leave`. Anything else - absent, empty, a typo -
    reads as the author's default, `leave` for a device and `device` for a cue.
*/

#include <string>

namespace wfg::doc
{
    class ShowDocument;
}

namespace wfg::cue
{
    namespace dohSetting
    {
        inline constexpr const char* takeBack = "takeBack";
        inline constexpr const char* leave    = "leave";
        inline constexpr const char* device   = "device";
    }

    /*  WHICH DECLARED NETWORK DEVICE AN ADDRESS BELONGS TO, or empty for none.

        THE ONE RESOLVER, and it is the rule the engine's `MountTable::mountOf`
        and the client's `model::deviceOf` already follow: the document's Mount
        elements in IDENTIFIER order, the LONGEST `prefixMatchLength` (which also
        splits a device of several roots), a tie to the SMALLEST identifier.
        Nothing refuses two devices covering one address equally - a main and a
        backup desk both at `/eos` - and the bytes go to the smaller id, so a
        walk in document order would read the setting of the desk that never
        received them. */
    std::string deviceOf (const doc::ShowDocument& document, const std::string& address);

    /*  A DEVICE'S OWN SETTING - `kind` is "mount" or "port" - read fail-safe:
        `takeBack` on the exact word, `leave` for anything else, a device that
        is not declared included. */
    std::string deviceDoh (const doc::ShowDocument& document, const std::string& kind,
                           const std::string& id);

    /*  A CUE'S OWN: `takeBack` or `leave` on the exact word, `device` for
        anything else. */
    std::string cueDoh (const doc::ShowDocument& document, const std::string& cueId);

    /*  THE EFFECTIVE SETTING OF AN OSC OR A MIDI CUE: its own when it
        overrides, else its device's - the device its address is under for an
        OSC cue, the port it names for a MIDI one - and `leave` for none. */
    std::string dohOf (const doc::ShowDocument& document, const std::string& cueId);

    /*  THE SAME FOR A RUN THAT REMEMBERED WHERE IT SENT (`Run::sentTo`): the
        cue's own when it overrides, else that device's, read now - a device
        gone since reads `leave`. `kind` is the run's: "osc" or "midi". */
    std::string dohOfDevice (const doc::ShowDocument& document, const std::string& kind,
                             const std::string& deviceId, const std::string& cueId);

    /*  WHERE A CUE'S SEND IS ROUTED, NOW: the device id, provided it is
        declared and its `tx` is on - anything but `false` is on, as the
        surface bridge reads a port - or empty when nothing could leave. */
    std::string sendTargetOf (const doc::ShowDocument& document, const std::string& kind,
                              const std::string& cueId);

    /*  WHETHER A MIDI CUE MAKES ITS SCENE HEARD (the author, 2026-10-01: "Should
        we have a toggle for this rather than assume?"): its port is marked
        "plays sound" (`Port/@audible`) and its `tx` is on. A port marked and not
        bound on this machine still counts - binding is the building's fact,
        not the show's. */
    bool playsSound (const doc::ShowDocument& document, const std::string& midiCueId);
}
