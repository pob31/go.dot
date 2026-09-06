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

#include <wfg/engine/cue/TriggerIndex.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  Read through the DOCUMENT rather than off the ValueTree, because the
            canonical writer omits an attribute holding its default and the
            reader leaves it absent - so a trigger that has never had `enabled`
            written to it has no such property at all, and asking the tree
            directly answers `false` for every trigger in the show. That exact
            mistake cost an afternoon in the group scheduler; it is not being
            made twice. */
        std::string textOf (const doc::ShowDocument& document,
                            const std::string& id, const char* name)
        {
            return document.getAttribute ("/godot/trigger/" + id + "/" + name)
                     .value_or (std::string {});
        }

        int numberOf (const doc::ShowDocument& document,
                      const std::string& id, const char* name, int fallback)
        {
            const auto text = textOf (document, id, name);

            if (text.empty())
                return fallback;

            int value {};
            const auto* last = text.data() + text.size();

            if (std::from_chars (text.data(), last, value).ptr != last)
                return fallback;

            return value;
        }

        void collect (const doc::ShowDocument& document, const juce::ValueTree& node,
                      const std::string& cueId, std::vector<Trigger>& out)
        {
            for (const auto& child : node)
            {
                const auto element = child.getType().toString();

                if (element == "Trigger")
                {
                    const auto id = child[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    Trigger trigger;
                    trigger.id = id;
                    trigger.cue = cueId;
                    trigger.kind = textOf (document, id, "kind");
                    trigger.enabled = textOf (document, id, "enabled") != "false";
                    trigger.address = textOf (document, id, "address");
                    trigger.value = textOf (document, id, "value");
                    trigger.port = textOf (document, id, "port");
                    trigger.channel = numberOf (document, id, "channel", 0);
                    trigger.type = textOf (document, id, "type");
                    trigger.number = numberOf (document, id, "number", 0);
                    trigger.data = numberOf (document, id, "data", -1);
                    trigger.secondOfDay = secondOfDayFor (textOf (document, id, "at"));

                    out.push_back (std::move (trigger));
                    continue;
                }

                /*  Down through everything else: a group's members, a header's
                    cues, a footer's. A trigger belongs to the nearest cue above
                    it, which is what `cueId` carries down. */
                const auto here = child.hasProperty (idProperty)
                                    && ! doc::ShowDocument::ownerForElement (
                                           element.toStdString()).empty();

                collect (document, child,
                         here && doc::ShowDocument::ownerForElement (element.toStdString()) == "cue"
                           ? child[idProperty].toString().toStdString()
                           : cueId,
                         out);
            }
        }
    }

    //==========================================================================
    std::shared_ptr<const TriggerIndex> TriggerIndex::build (const doc::ShowDocument& document)
    {
        auto index = std::make_shared<TriggerIndex>();
        collect (document, document.root(), {}, index->triggers);
        return index;
    }

    //==========================================================================
    int secondOfDayFor (const std::string& text)
    {
        /*  `HH:MM:SS`, and nothing else. Refusing everything that is not that
            shape is what makes a mistyped time a thing `wfg validate` can
            mention rather than a trigger that fires at some hour nobody meant.

            Parsed by position rather than by a scanner, because the scanner
            everybody reaches for is locale-sensitive and this is a serialisation
            surface: `%d` under a locale that groups digits is exactly the class
            of bug the whole repository runs its tests twice to catch. */
        if (text.size() != 8 || text[2] != ':' || text[5] != ':')
            return -1;

        const auto twoDigits = [&text] (std::size_t at) -> int
        {
            if (! std::isdigit (static_cast<unsigned char> (text[at]))
                  || ! std::isdigit (static_cast<unsigned char> (text[at + 1])))
                return -1;

            return (text[at] - '0') * 10 + (text[at + 1] - '0');
        };

        const auto hours = twoDigits (0);
        const auto minutes = twoDigits (3);
        const auto seconds = twoDigits (6);

        if (hours < 0 || minutes < 0 || seconds < 0
              || hours > 23 || minutes > 59 || seconds > 59)
            return -1;

        return hours * 3600 + minutes * 60 + seconds;
    }

    //==========================================================================
    bool valueMatches (const std::string& wanted, const std::vector<osc::Value>& args)
    {
        /*  EMPTY MATCHES ANYTHING, including a message with no arguments at
            all, which is what a bare address means to most of the things that
            will be sending one. A surface that sends `/go` with a float 1 on
            press and a float 0 on release needs the value; a foot switch that
            sends `/go` and nothing else must not have to. */
        if (wanted.empty())
            return true;

        const auto atom = osc::Value::fromAtom (wanted);

        if (! atom.has_value())
            return false;

        /*  ANY ARGUMENT, not the first. A message carrying several is a message
            whose second argument may be the one that means something, and
            insisting on position would make this useless against half the
            surfaces in a rack. */
        return std::any_of (args.begin(), args.end(),
                            [&atom] (const osc::Value& arg) { return arg == *atom; });
    }

    //==========================================================================
    std::vector<std::string> matchOsc (const TriggerIndex& index,
                                       const std::string& address,
                                       const std::vector<osc::Value>& args)
    {
        std::vector<std::string> fired;

        for (const auto& trigger : index.triggers)
        {
            if (! trigger.enabled || trigger.kind != triggerKind::osc)
                continue;

            if (trigger.address.empty() || trigger.address != address)
                continue;

            if (! valueMatches (trigger.value, args))
                continue;

            fired.push_back (trigger.id);
        }

        return fired;
    }

    //==========================================================================
    std::vector<std::string> matchMidi (const TriggerIndex& index, const MidiEvent& event)
    {
        std::vector<std::string> fired;

        for (const auto& trigger : index.triggers)
        {
            if (! trigger.enabled || trigger.kind != triggerKind::midi)
                continue;

            if (trigger.type != event.type)
                continue;

            /*  EMPTY IS ANY INPUT, which is what a rig with one MIDI cable
                wants and is the honest default: naming a port that is not bound
                would be a trigger that silently never fires. */
            if (! trigger.port.empty() && trigger.port != event.port)
                continue;

            //  Nought is any channel, and it is nought rather than -1 because
            //  MIDI channels are one-based everywhere a musician sees them.
            if (trigger.channel != 0 && trigger.channel != event.channel)
                continue;

            if (trigger.number != event.number)
                continue;

            /*  -1 IS ANY VELOCITY, and it has to be something outside 0..127
                rather than nought: a note-on with velocity nought is a note-off
                on a great many devices, so matching on it is a thing somebody
                will one day need to do deliberately. */
            if (trigger.data >= 0 && trigger.data != event.data)
                continue;

            fired.push_back (trigger.id);
        }

        return fired;
    }

    //==========================================================================
    std::vector<std::string> clockCrossings (const TriggerIndex& index,
                                             int previousSecondOfDay, int nowSecondOfDay)
    {
        std::vector<std::string> fired;

        if (previousSecondOfDay < 0 || nowSecondOfDay < 0)
            return fired;

        for (const auto& trigger : index.triggers)
        {
            if (! trigger.enabled || trigger.kind != triggerKind::clock)
                continue;

            if (trigger.secondOfDay < 0)
                continue;

            /*  HALF OPEN, `(previous, now]`, so that a second is crossed
                exactly once however often the clock is read. Closed at both
                ends would fire twice for a reading that landed on the second;
                open at both would miss it entirely at any rate slower than one
                reading a second.

                MIDNIGHT is the interval wrapping rather than a special case
                bolted on: `previous` greater than `now` means the day turned
                over between the two readings, and what was crossed is
                everything after `previous` OR everything up to `now`. */
            const auto crossed = previousSecondOfDay <= nowSecondOfDay
                                   ? (trigger.secondOfDay > previousSecondOfDay
                                        && trigger.secondOfDay <= nowSecondOfDay)
                                   : (trigger.secondOfDay > previousSecondOfDay
                                        || trigger.secondOfDay <= nowSecondOfDay);

            if (crossed)
                fired.push_back (trigger.id);
        }

        return fired;
    }
}
