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

#include <wfg/engine/cue/DohSetting.h>

#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/tree/Mount.h>

#include <cstddef>
#include <map>
#include <string>

namespace wfg::cue
{
    namespace
    {
        std::string attribute (const doc::ShowDocument& document, const std::string& address)
        {
            return document.getAttribute (address).value_or (std::string {});
        }

        /** The element a cue is, by its identifier, or empty for none. */
        std::string elementOf (const doc::ShowDocument& document, const std::string& cueId)
        {
            const auto cue = document.findById (cueId);
            return cue.isValid() ? cue.getType().toString().toStdString() : std::string {};
        }
    }

    std::string deviceOf (const doc::ShowDocument& document, const std::string& address)
    {
        if (address.empty())
            return {};

        /*  IDENTIFIER ORDER, by a map rather than by the document's own order:
            the tie goes to the smaller identifier, which is where the mount
            table sends the bytes. */
        std::map<std::string, std::string> prefixes;

        for (const auto& mount : document.root().getChildWithName ("Mounts"))
        {
            if (mount.getType().toString() != "Mount")
                continue;

            const auto id = mount[juce::Identifier ("id")].toString().toStdString();

            if (! id.empty())
                prefixes[id] = attribute (document, "/godot/mount/" + id + "/prefix");
        }

        std::string best;
        std::size_t covered = 0;

        for (const auto& [id, prefix] : prefixes)
        {
            const auto length = tree::prefixMatchLength (address, prefix);

            if (length > covered)
            {
                covered = length;
                best = id;
            }
        }

        return best;
    }

    std::string deviceDoh (const doc::ShowDocument& document, const std::string& kind,
                           const std::string& id)
    {
        if (id.empty())
            return dohSetting::leave;

        return attribute (document, "/godot/" + kind + "/" + id + "/doh") == dohSetting::takeBack
                 ? dohSetting::takeBack
                 : dohSetting::leave;
    }

    std::string cueDoh (const doc::ShowDocument& document, const std::string& cueId)
    {
        const auto own = attribute (document, "/godot/cue/" + cueId + "/doh");

        if (own == dohSetting::takeBack || own == dohSetting::leave)
            return own;

        return dohSetting::device;
    }

    std::string dohOf (const doc::ShowDocument& document, const std::string& cueId)
    {
        const auto element = elementOf (document, cueId);

        if (element == "Osc")
            return dohOfDevice (document, "osc",
                                deviceOf (document, attribute (document, "/godot/cue/" + cueId + "/address")),
                                cueId);

        if (element == "Midi")
            return dohOfDevice (document, "midi", attribute (document, "/godot/cue/" + cueId + "/port"),
                                cueId);

        /*  NO OTHER KIND SENDS TO A DEVICE TODAY, so nothing of it is a
            device's to keep: a kind that comes to send to one takes the same
            attribute and a branch here. */
        return dohSetting::leave;
    }

    std::string dohOfDevice (const doc::ShowDocument& document, const std::string& kind,
                             const std::string& deviceId, const std::string& cueId)
    {
        if (const auto own = cueDoh (document, cueId); own != dohSetting::device)
            return own;

        /*  A DEVICE GONE SINCE IS NOBODY'S TO TAKE BACK: its row reads nothing,
            and nothing reads `leave`. */
        return deviceDoh (document, kind == "midi" ? "port" : "mount", deviceId);
    }

    std::string sendTargetOf (const doc::ShowDocument& document, const std::string& kind,
                              const std::string& cueId)
    {
        const auto element = kind == "midi" ? std::string ("port") : std::string ("mount");
        const auto deviceId = kind == "midi"
                                ? attribute (document, "/godot/cue/" + cueId + "/port")
                                : deviceOf (document, attribute (document, "/godot/cue/" + cueId + "/address"));

        if (deviceId.empty())
            return {};

        /*  DECLARED: a port a cue names and the show does not have is no
            device at all, and nothing reaches it. */
        if (! document.getAttribute ("/godot/" + element + "/" + deviceId + "/tx").has_value())
            return {};

        if (attribute (document, "/godot/" + element + "/" + deviceId + "/tx") == "false")
            return {};

        return deviceId;
    }

    bool playsSound (const doc::ShowDocument& document, const std::string& midiCueId)
    {
        const auto port = attribute (document, "/godot/cue/" + midiCueId + "/port");

        if (port.empty())
            return false;

        return attribute (document, "/godot/port/" + port + "/audible") == "true"
                 && attribute (document, "/godot/port/" + port + "/tx") != "false";
    }
}
