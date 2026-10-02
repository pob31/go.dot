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

#include <wfg/engine/midi/SendQueue.h>

#include <algorithm>
#include <iterator>
#include <utility>

namespace wfg::midi
{
    void NoteLedger::observe (const Outgoing& message)
    {
        if (! message.cue || message.bytes.size() < 3)
            return;

        const auto status = static_cast<std::uint8_t> (message.bytes[0] & 0xF0);
        const auto channel = static_cast<std::uint8_t> (message.bytes[0] & 0x0F);
        const auto key = message.bytes[1];
        const auto velocity = message.bytes[2];

        if (status == 0x90 && velocity > 0)
        {
            auto& pressedBy = notes[Key { message.port, channel, key }];

            if (std::find (pressedBy.begin(), pressedBy.end(), message.run) == pressedBy.end())
                pressedBy.push_back (message.run);

            return;
        }

        /*  A NOTE-ON AT VELOCITY NOUGHT ENDS THE NOTE, because that is what a
            synth does with it - the MIDI specification makes it a note-off -
            whatever the cue called it, and however the input side files it. */
        if (status == 0x80 || status == 0x90)
        {
            notes.erase (Key { message.port, channel, key });
            return;
        }

        /*  ALL SOUND OFF AND ALL NOTES OFF (CC 120, CC 123): the cue has ended
            every note on that channel itself (2026-10-02, namespace draft
            §23.10), so a double Esc owes them nothing more. */
        if (status == 0xB0 && (key == 120 || key == 123))
        {
            for (auto at = notes.begin(); at != notes.end();)
            {
                if (std::get<0> (at->first) == message.port && std::get<1> (at->first) == channel)
                    at = notes.erase (at);
                else
                    ++at;
            }
        }
    }

    std::vector<Outgoing> NoteLedger::releaseAll()
    {
        std::vector<Outgoing> out;
        out.reserve (notes.size());

        /*  `0x8n key 0` (2026-10-02, namespace draft §23.10): the note-off a
            cue's own `noteOff` type spells when its second byte is left at
            nought, and JUCE's own - one spelling of a note-off in the show. */
        for (const auto& entry : notes)
        {
            const auto status = static_cast<std::uint8_t> (0x80 | std::get<1> (entry.first));
            out.push_back ({ std::get<0> (entry.first), Bytes { status, std::get<2> (entry.first), 0 }, {}, false });
        }

        notes.clear();
        return out;
    }

    void NoteLedger::forgetPort (const std::string& port)
    {
        for (auto at = notes.begin(); at != notes.end();)
        {
            if (std::get<0> (at->first) == port)
                at = notes.erase (at);
            else
                ++at;
        }
    }

    std::vector<std::string> NoteLedger::runsOf (const std::string& port, std::uint8_t channel,
                                                 std::uint8_t note) const
    {
        const auto found = notes.find (Key { port, channel, note });
        return found == notes.end() ? std::vector<std::string> {} : found->second;
    }

    //==============================================================================
    void SendQueue::push (Outgoing message)
    {
        items.push_back (std::move (message));
    }

    bool SendQueue::pop (Outgoing& out)
    {
        if (items.empty())
            return false;

        out = std::move (items.front());
        items.pop_front();
        return true;
    }

    void SendQueue::markLeaving (const Outgoing& message)
    {
        if (message.cue)
            ledger.observe (message);
    }

    std::size_t SendQueue::dropQueued()
    {
        const auto before = items.size();

        items.erase (std::remove_if (items.begin(), items.end(),
                                     [] (const Outgoing& item) { return item.cue; }),
                     items.end());

        const auto dropped = before - items.size();

        /*  AT THE FRONT, in order, and not a cue's: what is left in the queue is
            a surface's, and a note that rings while a SysEx dump goes out is a
            note ringing after the press. Tagged as no cue's, a second press
            before they have gone cannot drop them - the record has already
            let them go. */
        auto releases = ledger.releaseAll();
        items.insert (items.begin(), std::make_move_iterator (releases.begin()),
                      std::make_move_iterator (releases.end()));

        return dropped;
    }

    void SendQueue::forgetPort (const std::string& port)
    {
        ledger.forgetPort (port);
    }

    std::deque<Outgoing> SendQueue::takeAll()
    {
        std::deque<Outgoing> out;
        out.swap (items);
        return out;
    }

    std::deque<Outgoing> SendQueue::takeAllForClose (const std::function<bool (const std::string&)>& reaches)
    {
        auto out = takeAll();

        /*  AFTER WHAT IS WAITING, not ahead of it as the double Esc puts them:
            nothing is dropped here, so a note-on still queued leaves at the
            close, and its note-off has to follow it. */
        for (const auto& message : out)
            if (message.cue && reaches (message.port))
                ledger.observe (message);

        for (auto& release : ledger.releaseAll())
            out.push_back (std::move (release));

        return out;
    }
}
