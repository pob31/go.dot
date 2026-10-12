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
#include <cstdlib>
#include <iterator>

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
    ClockCrossings clockCrossings (const TriggerIndex& index,
                                   const ClockReading& previous, const ClockReading& now)
    {
        ClockCrossings result;

        /*  The first reading crosses nothing, which is what keeps a show
            opened at 19:30:00 from firing the 19:30:00 cue because it happened
            to be started then; and so does the first after an outage, whose
            previous reading serve forgets. */
        if (previous.millisecondOfDay < 0 || now.millisecondOfDay < 0)
            return result;

        /*  THE WALL CLOCK'S ADVANCE, THE NEAR WAY ROUND THE DIAL. A time of
            day cannot say how many midnights lay between two readings, so the
            advance is taken as the shorter of the two ways from one to the
            other: 23:59:59 to 00:00:00 is a second forward, 19:30:05 to
            19:30:04 a second back, never a day less one second forward. An
            advance of half a day or more either way reads as the other way
            round, and is a step anyway: no tick is that late. */
        constexpr std::int64_t day = 86'400'000;
        auto wall = (now.millisecondOfDay - previous.millisecondOfDay) % day;

        if (wall >= day / 2)
            wall -= day;
        else if (wall < -day / 2)
            wall += day;

        const auto steady = now.steadyMilliseconds - previous.steadyMilliseconds;

        /*  A STEP IS THE WALL CLOCK MOVING WHERE THE STEADY CLOCK DID NOT -
            more than two seconds apart over one reading. Two seconds is far
            above what a slew can do between two ticks (fifty parts per
            million of 20 ms is a microsecond) and far above the two clocks'
            reading noise, and it is under anything a time-sync service steps
            by rather than slews. Nothing is crossed, either way: a step back
            would have re-run a stretch of the day, a step forward would have
            fired every trigger it jumped over in the same instant, and summer
            time starting skips an hour whose triggers are therefore not fired
            that night (AIY, which the author may overrule for this direction).
            The reading the step landed on becomes the next one's previous, so
            the tick after is time passing again and the step is said once. */
        if (std::abs (wall - steady) > clockStepMilliseconds)
        {
            result.stepped = true;
            result.stepMilliseconds = wall - steady;
            return result;
        }

        /*  A wall clock that stood still or went back a little - a time-sync
            service stepping it by less than the threshold - crosses nothing:
            going back is never a crossing. What it re-crosses afterwards is
            kept from firing twice by the day's memory, not here. */
        if (wall <= 0)
            return result;

        const auto previousSecondOfDay = static_cast<int> (previous.millisecondOfDay / 1000);
        const auto nowSecondOfDay = static_cast<int> (now.millisecondOfDay / 1000);

        if (previousSecondOfDay == nowSecondOfDay)
            return result;

        /*  MIDNIGHT is the interval wrapping, and it is only ever reached here
            when the steady clock went on as far as the wall clock did - which
            is the whole difference from the rule this replaced, where any
            `previous` greater than `now` was a midnight. */
        const auto wrapped = previousSecondOfDay > nowSecondOfDay;

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
                reading a second. Across midnight, what was crossed is
                everything after `previous` OR everything up to `now`, and each
                crossing falls on its own side's date. */
            const auto beforeMidnight = trigger.secondOfDay > previousSecondOfDay;
            const auto crossed = wrapped ? (beforeMidnight || trigger.secondOfDay <= nowSecondOfDay)
                                         : (beforeMidnight && trigger.secondOfDay <= nowSecondOfDay);

            if (crossed)
                result.crossed.push_back ({ trigger.id, trigger.secondOfDay,
                                            wrapped && beforeMidnight ? previous.date : now.date });
        }

        return result;
    }

    //==========================================================================
    std::vector<std::string> ClockDays::admit (const TriggerIndex& index,
                                               const std::vector<ClockCrossing>& crossed)
    {
        std::vector<std::string> admitted;

        if (crossed.empty())
            return admitted;

        /*  FORGET WHAT THE SHOW NO LONGER HOLDS, here rather than every tick:
            this runs only when a second was crossed, and a trigger deleted is
            a memory that would otherwise live as long as serve. A trigger
            whose time changed is forgotten with it - its memory is of the time
            it fired at, which nobody wants any more. */
        for (auto entry = fired.begin(); entry != fired.end();)
        {
            const auto still = std::any_of (index.triggers.begin(), index.triggers.end(),
                                            [&entry] (const Trigger& trigger)
                                            {
                                                return trigger.id == entry->first
                                                         && trigger.kind == triggerKind::clock
                                                         && trigger.secondOfDay == entry->second.secondOfDay;
                                            });

            entry = still ? std::next (entry) : fired.erase (entry);
        }

        for (const auto& crossing : crossed)
        {
            const auto known = fired.find (crossing.id);

            if (known != fired.end()
                  && known->second.date == crossing.date
                  && known->second.secondOfDay == crossing.secondOfDay)
                continue;

            fired[crossing.id] = { crossing.date, crossing.secondOfDay };
            admitted.push_back (crossing.id);
        }

        return admitted;
    }

    //==========================================================================
    std::string clockStepNote (const ClockReading& previous, const ClockReading& now,
                               std::int64_t stepMilliseconds)
    {
        /*  Built from whole numbers only: `std::to_string` of a double, and
            every printf of one, follows the process's locale, and this
            sentence is written under fr_FR as often as under C. */
        const auto timeOfDay = [] (std::int64_t millisecondOfDay)
        {
            const auto second = millisecondOfDay / 1000;
            const auto twoDigits = [] (std::int64_t value)
            {
                return (value < 10 ? std::string ("0") : std::string {}) + std::to_string (value);
            };

            return twoDigits (second / 3600) + ":" + twoDigits (second / 60 % 60) + ":"
                     + twoDigits (second % 60);
        };

        const auto size = stepMilliseconds < 0 ? -stepMilliseconds : stepMilliseconds;

        return std::string ("wall clock stepped ") + (stepMilliseconds < 0 ? "back " : "forward ")
                 + std::to_string (size / 1000) + "." + std::to_string (size % 1000 / 100)
                 + " s against the steady clock, " + timeOfDay (previous.millisecondOfDay)
                 + " to " + timeOfDay (now.millisecondOfDay)
                 + " - the clock triggers between them are not fired";
    }
}
