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

#include <wfg/engine/cue/AutoName.h>

#include <wfg/engine/document/Schema.h>
#include <wfg/engine/osc/OscValue.h>

#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  HOW FAR A NAME IS FOLLOWED THROUGH TARGETS: a fade of a stop of a
            sound is three links, and nothing a show does needs more. Past it -
            which only a loop reaches - the target is named by its number. */
        constexpr int deepest = 4;

        /*  A ROW AS THE SHOW READS IT, its default included: the canonical
            writer leaves out a value equal to its default, so a fade saved to
            silence has no `level` at all. The defaults are the parameter
            table's, as `cue::Reader`'s are. */
        std::string defaultOf (std::string_view owner, std::string_view name)
        {
            for (const auto* row : doc::Schema::rowsForOwner (owner))
                if (row->name == name)
                    return std::string (row->defaultText);

            return {};
        }

        std::string text (const juce::ValueTree& node, std::string_view owner, const char* name)
        {
            const juce::Identifier property { name };

            if (node.hasProperty (property))
                return node[property].toString().toStdString();

            return defaultOf (owner, name);
        }

        double number (const juce::ValueTree& node, std::string_view owner, const char* name)
        {
            const juce::Identifier property { name };

            //  Off the var, never through text: a locale question avoided.
            if (node.hasProperty (property))
                return static_cast<double> (node[property]);

            return osc::parseDouble (defaultOf (owner, name)).value_or (0.0);
        }

        std::string whole (const juce::ValueTree& node, std::string_view owner, const char* name)
        {
            return std::to_string (static_cast<int> (number (node, owner, name)));
        }

        bool flag (const juce::ValueTree& node, std::string_view owner, const char* name)
        {
            const juce::Identifier property { name };

            if (node.hasProperty (property))
                return static_cast<bool> (node[property]);

            return defaultOf (owner, name) == "true";
        }

        std::string own (const juce::ValueTree& node, const char* name)
        {
            return node[juce::Identifier (name)].toString().toStdString();
        }

        /*  A FILE AS A PERSON CALLS IT: its last part, without its extension -
            what a drop has named a new cue since Phase 2. */
        std::string stemOf (const std::string& path)
        {
            const auto slash = path.find_last_of ("/\\");
            auto last = slash == std::string::npos ? path : path.substr (slash + 1);
            const auto dot = last.find_last_of ('.');

            if (dot != std::string::npos && dot > 0)
                last.resize (dot);

            return last;
        }

        /*  AN OSC VALUE AS IT READS, its type tag left out: f:0.5 is 0.5, i:3
            is 3. The tag tells two cues apart for the machine; the name is for
            the person, who reads the number. */
        std::string valueWords (const std::string& spelled)
        {
            std::istringstream in (spelled);
            std::string out;

            for (std::string word; in >> word;)
            {
                if (word.size() > 2 && word[1] == ':' && std::isalpha (static_cast<unsigned char> (word[0])))
                    word.erase (0, 2);

                out += (out.empty() ? "" : " ") + word;
            }

            return out;
        }

        std::string oscWords (const juce::ValueTree& cue)
        {
            const auto address = own (cue, "address");

            if (address.empty())
                return {};

            const auto value = valueWords (own (cue, "value"));
            auto out = value.empty() ? address : address + " " + value;

            int further = 0;

            for (const auto child : cue)
                if (child.hasType ("Message"))
                    ++further;

            if (further > 0)
                out += " (+" + std::to_string (further) + ")";

            return out;
        }

        /*  A SYSEX AS A MANUAL PRINTS ITS START: the first six bytes, and an
            ellipsis when there is more, since the bytes after the maker's are
            what tells two messages apart and a whole dump is not a name. */
        std::string sysexWords (const std::string& hex)
        {
            std::string digits;

            for (const auto c : hex)
                if (std::isxdigit (static_cast<unsigned char> (c)))
                    digits += static_cast<char> (std::toupper (static_cast<unsigned char> (c)));

            if (digits.empty())
                return {};

            std::string out = "SysEx";
            const std::size_t shown = 6;
            std::size_t bytes = 0;

            for (std::size_t at = 0; at + 1 < digits.size(); at += 2, ++bytes)
                if (bytes < shown)
                    out += " " + digits.substr (at, 2);

            if (bytes > shown)
                out += " ...";

            return out;
        }

        std::string midiWords (const juce::ValueTree& cue, const std::string& portName)
        {
            const auto type = text (cue, "midi", "type");
            const auto channel = ", ch " + whole (cue, "midi", "channel");
            const auto data1 = whole (cue, "midi", "data1");
            const auto data2 = whole (cue, "midi", "data2");

            std::string out;

            if (type == "programChange")        out = "Program change " + data1 + channel;
            else if (type == "controlChange")   out = "CC " + data1 + " = " + data2 + channel;
            else if (type == "noteOn")          out = "Note on " + data1 + " vel " + data2 + channel;
            else if (type == "noteOff")         out = "Note off " + data1 + channel;
            else if (type == "aftertouch")      out = "Aftertouch " + data1 + " = " + data2 + channel;
            else if (type == "channelPressure") out = "Pressure " + data2 + channel;
            else if (type == "pitchBend")       out = "Pitch bend " + data2 + channel;
            else if (type == "sysex")           out = sysexWords (own (cue, "sysex"));

            if (out.empty())
                return {};

            return portName.empty() ? out : out + " on " + portName;
        }

        /*  A TRANSPORT CUE'S VERB, as the inspector words it (Inspector.cpp's
            `wordTheVerb`), put around its target's name. */
        std::string transportWords (const std::string& verb, bool andGo, const std::string& target,
                                    const std::string& slice)
        {
            if (verb == "fade")           return "Fade out and stop " + target;
            if (verb == "afterMember")    return "Stop " + target + " after this member";
            if (verb == "afterIteration") return "Stop " + target + " after this round";
            if (verb == "advance")        return slice.empty() ? "Advance " + target
                                                               : "Advance " + target + " to " + slice;
            if (verb == "record")         return "Rec " + target;
            if (verb == "loop")           return "Loop " + target;
            if (verb == "overdub")        return "Overdub " + target;
            if (verb == "clear")          return "Clear " + target;
            if (verb == "enable")         return "Enable " + target;
            if (verb == "disable")        return "Disable " + target;
            if (verb == "jump")           return andGo ? "Jump to " + target + " and Go" : "Jump to " + target;

            return "Stop " + target;        // hard, and a verb added later until it has words
        }

        bool fadeMovesOnlyTheLevel (const juce::ValueTree& fade)
        {
            return ! flag (fade, "fade", "rateOn")
                && own (fade, "sends").empty() && own (fade, "eq").empty()
                && own (fade, "fx").empty() && own (fade, "video").empty();
        }
    }

    AutoNames::AutoNames (const juce::ValueTree& showRoot)
    {
        std::vector<juce::ValueTree> pending { showRoot };

        while (! pending.empty())
        {
            const auto node = pending.back();
            pending.pop_back();

            if (node.hasProperty (idProperty))
                byId.emplace (node[idProperty].toString().toStdString(), node);

            for (const auto child : node)
                pending.push_back (child);
        }
    }

    juce::ValueTree AutoNames::find (const std::string& id) const
    {
        const auto found = byId.find (id);
        return found != byId.end() ? found->second : juce::ValueTree {};
    }

    std::string AutoNames::nameOf (const std::string& id) const
    {
        return id.empty() ? std::string {} : own (find (id), "name");
    }

    std::string AutoNames::of (const juce::ValueTree& cue) const
    {
        return of (cue, 0);
    }

    std::string AutoNames::shownAs (const juce::ValueTree& cue) const
    {
        const auto name = own (cue, "name");
        return name.empty() ? of (cue) : name;
    }

    std::string AutoNames::targetWords (const std::string& targetId, int depth) const
    {
        const auto target = find (targetId);

        if (! target.isValid())
            return {};

        if (const auto name = own (target, "name"); ! name.empty())
            return name;

        if (depth < deepest)
            if (const auto automatic = of (target, depth + 1); ! automatic.empty())
                return automatic;

        if (const auto cueNumber = own (target, "number"); ! cueNumber.empty())
            return "cue " + cueNumber;

        auto kind = target.getType().toString().toLowerCase().toStdString();
        return kind == "transport" ? std::string ("stop cue") : kind;
    }

    std::string AutoNames::of (const juce::ValueTree& cue, int depth) const
    {
        const auto element = cue.getType().toString().toStdString();

        if (element == "Fade")
        {
            /*  A DCA'S FADE: the trim it moves, and no target read (§3.28). */
            if (const auto dca = own (cue, "dca"); ! dca.empty())
            {
                const auto name = nameOf (dca);
                return name.empty() ? std::string ("Fade DCA") : "Fade DCA " + name;
            }

            const auto target = targetWords (own (cue, "target"), depth);

            if (target.empty())
                return {};

            const auto toSilence = flag (cue, "fade", "levelOn")
                                && number (cue, "fade", "level") <= -120.0
                                && fadeMovesOnlyTheLevel (cue);
            const auto stops = flag (cue, "fade", "stopWhenDone");

            if (toSilence)
                return (stops ? "Fade out and stop " : "Fade out ") + target;

            return (stops ? "Fade and stop " : "Fade ") + target;
        }

        if (element == "Transport")
        {
            const auto target = targetWords (own (cue, "target"), depth);

            if (target.empty())
                return {};

            return transportWords (text (cue, "transport", "verb"), flag (cue, "transport", "andGo"),
                                   target, nameOf (own (cue, "range")));
        }

        if (element == "Start")
        {
            const auto target = targetWords (own (cue, "target"), depth);
            return target.empty() ? std::string {} : "Start " + target;
        }

        if (element == "Osc")
            return oscWords (cue);

        if (element == "Midi")
            return midiWords (cue, nameOf (own (cue, "port")));

        if (element == "Media")
            return stemOf (own (cue, "file"));

        if (element == "Mic")
            return nameOf (own (cue, "input"));

        if (element == "Video")
        {
            const auto source = text (cue, "video", "source");

            if (source == "picture" || source == "movie")
                return stemOf (own (cue, "file"));

            if (source == "capture")
            {
                const auto input = nameOf (own (cue, "videoInput"));
                return input.empty() ? std::string ("Capture") : input;
            }

            return source == "mask" ? std::string ("Mask") : std::string ("Fill");
        }

        return {};
    }
}
