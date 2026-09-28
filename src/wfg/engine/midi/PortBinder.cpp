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

#include <wfg/engine/midi/PortBinder.h>

#include <wfg/engine/midi/MidiInputs.h>
#include <wfg/engine/midi/MidiSender.h>

#include <algorithm>
#include <utility>

namespace wfg::midi
{
    PortBinder::PortBinder (MidiInputs& inputsToUse, MidiSender& outputsToUse) noexcept
        : inputs (inputsToUse), outputs (outputsToUse)
    {
    }

    //==========================================================================
    PortBound PortBinder::bindOne (const PortWish& wish)
    {
        PortBound out;
        out.id = wish.id;

        std::vector<std::string> troubles;

        /*  THE INPUT SIDE, AND IT IS OPENED AS THE PORT: what arrives is
            stamped with the show's own word for the cable, so a trigger names
            "Lights" and goes on firing when somebody moves the interface to
            another socket. */
        if (! wish.inputDevice.empty())
        {
            std::string why;

            if (inputs.openAs (wish.inputDevice, wish.inputDeviceId, wish.id, out.inputMatched, why))
            {
                out.binding.bound = true;
            }
            else
            {
                out.inputMatched.clear();       // found and would not open: nothing to write down
                troubles.push_back (why);
            }
        }

        if (! wish.outputDevice.empty())
        {
            std::string why;

            if (outputs.bind (wish.id, wish.label, wish.outputDevice, wish.outputDeviceId,
                              out.outputMatched, why))
            {
                out.binding.bound = true;
                out.binding.deviceId = out.outputMatched;
            }
            else
            {
                out.binding.bound = false;
                out.outputMatched.clear();
                troubles.push_back (why);
            }
        }

        for (const auto& trouble : troubles)
        {
            if (! out.binding.problem.empty())
                out.binding.problem += "; ";

            out.binding.problem += trouble;
        }

        return out;
    }

    void PortBinder::release (const std::string& portId)
    {
        inputs.close (portId);
        outputs.unbind (portId);
    }

    //==========================================================================
    std::vector<PortBound> PortBinder::bindAll (const std::vector<PortWish>& wishes)
    {
        std::vector<PortBound> results;
        results.reserve (wishes.size());

        for (const auto& wish : wishes)
            if (! wish.id.empty())
                results.push_back (bindOne (wish));

        /*  REMEMBERED WITH THE IDENTIFIERS IT MATCHED, which the caller writes
            into the document before the tick thread runs. Remembered without
            them, the first show edit would read a port whose identifier had
            "changed" and close and reopen every device the show has. */
        applied = wishes;

        for (auto& wish : applied)
            for (const auto& result : results)
                if (result.id == wish.id)
                {
                    if (! result.inputMatched.empty())
                        wish.inputDeviceId = result.inputMatched;

                    if (! result.outputMatched.empty())
                        wish.outputDeviceId = result.outputMatched;
                }

        handed = applied;

        {
            const std::lock_guard<std::mutex> lock { mailbox };
            wanted = applied;
            appliedSeq = wantedSeq;
        }

        return results;
    }

    bool PortBinder::want (std::vector<PortWish> wishes)
    {
        if (wishes == handed)
            return false;

        handed = wishes;

        const std::lock_guard<std::mutex> lock { mailbox };
        wanted = std::move (wishes);
        ++wantedSeq;
        return true;
    }

    void PortBinder::rebind()
    {
        std::vector<PortWish> target;

        {
            const std::lock_guard<std::mutex> lock { mailbox };

            if (wantedSeq == appliedSeq)
                return;

            target = wanted;
            appliedSeq = wantedSeq;
        }

        const auto find = [] (const std::vector<PortWish>& in, const std::string& id)
        {
            return std::find_if (in.begin(), in.end(),
                                 [&id] (const PortWish& wish) { return wish.id == id; });
        };

        std::vector<PortBound> results;
        std::vector<const PortWish*> opening;

        //  Every port the show no longer declares, or declares on other devices: closed first.
        for (const auto& was : applied)
        {
            const auto now = find (target, was.id);

            if (now == target.end())
            {
                release (was.id);

                PortBound forgotten;
                forgotten.id = was.id;
                forgotten.gone = true;
                results.push_back (std::move (forgotten));
            }
            else if (! now->sameDevices (was))
            {
                release (was.id);
            }
        }

        //  Then opened: what changed, and what is new.
        for (const auto& wish : target)
        {
            if (wish.id.empty())
                continue;

            const auto was = find (applied, wish.id);

            if (was == applied.end() || ! was->sameDevices (wish))
                opening.push_back (&wish);
        }

        for (const auto* wish : opening)
            results.push_back (bindOne (*wish));

        applied = std::move (target);

        if (results.empty())
            return;

        {
            const std::lock_guard<std::mutex> lock { mailbox };

            for (auto& result : results)
                finished.push_back (std::move (result));
        }

        anyFinished.store (true, std::memory_order_release);
    }

    std::vector<PortBound> PortBinder::take()
    {
        if (! anyFinished.exchange (false, std::memory_order_acq_rel))
            return {};

        const std::lock_guard<std::mutex> lock { mailbox };
        return std::exchange (finished, {});
    }
}
