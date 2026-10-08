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
#include <wfg/engine/midi/MidiMessages.h>
#include <wfg/engine/tree/Mount.h>

#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

    namespace
    {
        /** The device a cue's send routes to, declared or not: an OSC cue's
            by its address, a MIDI cue's port as it names it. */
        std::string routeOf (const doc::ShowDocument& document, const std::string& element,
                             const std::string& cueId)
        {
            if (element == "Osc")
                return deviceOf (document, attribute (document, "/godot/cue/" + cueId + "/address"));

            if (element == "Midi")
                return attribute (document, "/godot/cue/" + cueId + "/port");

            return {};
        }

        std::vector<std::string> wordsOf (const std::string& text)
        {
            std::istringstream in (text);
            std::vector<std::string> words;

            for (std::string word; in >> word;)
                words.push_back (word);

            return words;
        }

        /** A whole decimal number and nothing else, or false. */
        bool wholeNumber (const std::string& word, int& out)
        {
            if (word.empty() || word.size() > 6)
                return false;

            for (const auto c : word)
                if (! std::isdigit (static_cast<unsigned char> (c)))
                    return false;

            out = std::stoi (word);
            return true;
        }

        /*  WHICH NUMBER A MESSAGE TYPE CARRIES WHERE: the one-number types
            say it in the row their cue keeps it in - a program in `data1`, a
            pressure and a bend in `data2` - so the line reads as the rows. */
        bool oneNumber (const std::string& type)
        {
            return type == "programChange" || type == "channelPressure" || type == "pitchBend";
        }

        bool inData1 (const std::string& type)
        {
            return type == "programChange";
        }
    }

    std::string spellMessageOf (const doc::ShowDocument& document, const std::string& cueId)
    {
        const auto element = elementOf (document, cueId);
        const auto base = "/godot/cue/" + cueId + "/";

        if (element == "Osc")
            return attribute (document, base + "address") + " " + attribute (document, base + "value");

        if (element != "Midi")
            return {};

        /*  AS THE SEND READS THEM, defaults included: a row never written
            reads as its schema's default, and the line must say what went. */
        auto type = attribute (document, base + "type");

        if (type.empty())
            type = "noteOn";

        if (type == "sysex")
            return "sysex " + attribute (document, base + "sysex");

        const auto number = [&] (const char* name, const char* fallback)
        {
            const auto text = attribute (document, base + name);
            return text.empty() ? std::string (fallback) : text;
        };

        const auto channel = number ("channel", "1");

        if (oneNumber (type))
            return type + " " + channel + " " + number (inData1 (type) ? "data1" : "data2", "0");

        return type + " " + channel + " " + number ("data1", "0") + " " + number ("data2", "0");
    }

    std::string previousCommandOf (const doc::ShowDocument& document, const std::string& kind,
                                   const std::string& deviceId, const std::string& cueId)
    {
        if (deviceId.empty())
            return {};

        auto list = document.findById (cueId);

        while (list.isValid() && list.getType().toString() != "List")
            list = list.getParent();

        if (! list.isValid())
            return {};

        const auto wanted = kind == "midi" ? std::string ("Midi") : std::string ("Osc");

        /*  IN PLAY ORDER, depth first - a group's header, its members, its
            footer, wherever the file happens to keep the two sections among
            its children - and the last match before the cue wins. The client
            walks the published `headerOrder`, `order` and `footerOrder` the
            same way (`model::previousCommand`), so the inspector's greyed line
            is what the Doh sends. */
        std::string found;
        auto reached = false;

        std::function<void (const juce::ValueTree&)> walk;

        const auto visit = [&] (const juce::ValueTree& cue)
        {
            if (reached)
                return;

            const auto id = cue[juce::Identifier ("id")].toString().toStdString();

            if (id == cueId)
            {
                reached = true;
                return;
            }

            if (cue.getType().toString().toStdString() == wanted && routeOf (document, wanted, id) == deviceId)
                found = spellMessageOf (document, id);

            walk (cue);
        };

        walk = [&] (const juce::ValueTree& container)
        {
            for (const auto& child : container.getChildWithName ("Header"))
                visit (child);

            for (const auto& child : container)
            {
                const auto type = child.getType().toString();

                if (type != "Header" && type != "Footer" && type != "Persistent")
                    visit (child);
            }

            for (const auto& child : container.getChildWithName ("Footer"))
                visit (child);
        };

        walk (list);
        return reached ? found : std::string {};
    }

    std::string rollbackOfDevice (const doc::ShowDocument& document, const std::string& kind,
                                  const std::string& deviceId, const std::string& cueId)
    {
        if (auto own = attribute (document, "/godot/cue/" + cueId + "/dohRollback"); ! own.empty())
            return own;

        if (! deviceId.empty())
            if (auto general = attribute (document, "/godot/" + std::string (kind == "midi" ? "port" : "mount")
                                                      + "/" + deviceId + "/dohRollback");
                ! general.empty())
                return general;

        return previousCommandOf (document, kind, deviceId, cueId);
    }

    std::string rollbackOf (const doc::ShowDocument& document, const std::string& cueId)
    {
        const auto element = elementOf (document, cueId);

        if (element != "Osc" && element != "Midi")
            return {};

        return rollbackOfDevice (document, element == "Midi" ? "midi" : "osc",
                                 routeOf (document, element, cueId), cueId);
    }

    RollbackMessage parseRollback (const std::string& kind, const std::string& text)
    {
        RollbackMessage out;
        const auto words = wordsOf (text);

        if (words.empty())
            return out;

        if (kind == "osc")
        {
            /*  THE ADDRESS, THEN THE ATOMS - the rest of the line, which a
                string's own spaces may be part of, read as a value list
                (namespace draft §45). */
            const auto& address = words.front();

            if (address.front() != '/')
                return out;

            const auto afterAddress = text.find (address) + address.size();
            const auto start = text.find_first_not_of (" \t", afterAddress);
            auto atom = start == std::string::npos ? std::string {} : text.substr (start);

            while (! atom.empty() && std::isspace (static_cast<unsigned char> (atom.back())))
                atom.pop_back();

            const auto value = osc::valuesFromAtoms (atom);

            if (! value.has_value())
                return out;

            out.address = address;
            out.value = *value;
            out.ok = true;
            return out;
        }

        midi::MessageSpec spec;
        spec.type = words.front();

        if (spec.type == "sysex")
        {
            std::string hex;

            for (std::size_t n = 1; n < words.size(); ++n)
                hex += words[n] + " ";

            spec.sysex = hex;
        }
        else
        {
            const auto numbers = oneNumber (spec.type) ? 2u : 3u;

            if (words.size() != numbers + 1)
                return out;

            int channel = 0, first = 0, second = 0;

            if (! wholeNumber (words[1], channel) || ! wholeNumber (words[2], first))
                return out;

            if (numbers == 3 && ! wholeNumber (words[3], second))
                return out;

            spec.channel = channel;

            if (! oneNumber (spec.type))
            {
                spec.data1 = first;
                spec.data2 = second;
            }
            else if (inData1 (spec.type))
                spec.data1 = first;
            else
                spec.data2 = first;
        }

        const auto built = midi::messageFor (spec);

        if (! built.ok())
            return out;

        out.bytes = built.bytes;
        out.ok = true;
        return out;
    }
}
