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

        remember (results);

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

        remember (results);

        {
            const std::lock_guard<std::mutex> lock { mailbox };

            for (auto& result : results)
                finished.push_back (std::move (result));
        }

        anyFinished.store (true, std::memory_order_release);
    }

    void PortBinder::remember (const std::vector<PortBound>& results)
    {
        for (const auto& result : results)
        {
            if (result.gone)
                lastBound.erase (result.id);
            else
                lastBound[result.id] = result.binding.bound;
        }
    }

    //==========================================================================
    std::vector<std::string> PortBinder::portsToRetry (const std::vector<PortWish>& applied,
                                                       const std::map<std::string, bool>& bound,
                                                       const std::vector<Device>& inputs,
                                                       const std::vector<Device>& outputs)
    {
        const auto there = [] (const std::vector<Device>& devices, const std::string& name, const std::string& id)
        {
            return std::any_of (devices.begin(), devices.end(), [&name, &id] (const Device& device)
            {
                return (! id.empty() && device.identifier == id) || (! name.empty() && device.name == name);
            });
        };

        std::vector<std::string> out;

        for (const auto& wish : applied)
        {
            const auto namesInput = ! wish.inputDevice.empty() || ! wish.inputDeviceId.empty();
            const auto namesOutput = ! wish.outputDevice.empty() || ! wish.outputDeviceId.empty();

            if (wish.id.empty() || (! namesInput && ! namesOutput))
                continue;

            const auto found = bound.find (wish.id);
            const auto isBound = found != bound.end() && found->second;

            if (! isBound)
            {
                out.push_back (wish.id);
                continue;
            }

            const auto inputGone = namesInput && ! there (inputs, wish.inputDevice, wish.inputDeviceId);
            const auto outputGone = namesOutput && ! there (outputs, wish.outputDevice, wish.outputDeviceId);

            if (inputGone || outputGone)
                out.push_back (wish.id);
        }

        return out;
    }

    void PortBinder::rescan()
    {
        DeviceLists found { MidiInputs::availableDevices(), MidiSender::availableDevices() };
        std::vector<PortBound> results;

        for (const auto& id : portsToRetry (applied, lastBound, found.inputs, found.outputs))
            for (const auto& wish : applied)
                if (wish.id == id)
                {
                    release (id);
                    results.push_back (bindOne (wish));
                }

        remember (results);

        {
            const std::lock_guard<std::mutex> lock { mailbox };

            for (auto& result : results)
                finished.push_back (std::move (result));

            scanned = std::move (found);
        }

        if (! results.empty())
            anyFinished.store (true, std::memory_order_release);

        anyScanned.store (true, std::memory_order_release);
    }

    std::optional<PortBinder::DeviceLists> PortBinder::takeDevices()
    {
        if (! anyScanned.exchange (false, std::memory_order_acq_rel))
            return std::nullopt;

        const std::lock_guard<std::mutex> lock { mailbox };
        return std::exchange (scanned, std::nullopt);
    }

    std::vector<PortBound> PortBinder::take()
    {
        if (! anyFinished.exchange (false, std::memory_order_acq_rel))
            return {};

        const std::lock_guard<std::mutex> lock { mailbox };
        return std::exchange (finished, {});
    }
}
