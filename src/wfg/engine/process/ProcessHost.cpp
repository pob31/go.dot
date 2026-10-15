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

#include <wfg/engine/process/ProcessHost.h>
#include <wfg/engine/process/PdInstance.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace wfg::process
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        std::string textOf (const Atoms& atoms)
        {
            std::string out;
            for (const auto& atom : atoms)
            {
                if (! out.empty())
                    out += ' ';
                out += atom.isNumber ? osc::formatDouble (atom.number) : atom.word;
            }
            return out;
        }

        std::chrono::microseconds left (Clock::time_point deadline)
        {
            const auto now = Clock::now();
            return now >= deadline ? std::chrono::microseconds (0)
                                   : std::chrono::duration_cast<std::chrono::microseconds> (deadline - now);
        }
    }

    ProcessHost::ProcessHost (Settings settingsToUse)
        : settings (std::move (settingsToUse))
    {
    }

    ProcessHost::~ProcessHost()
    {
        for (auto& slot : slots)
            if (slot->instance != nullptr && (slot->busy || (slot->sharesLock && stuck)))
                slot->instance->leaveUnfreed();
        slots.clear();
    }

    bool ProcessHost::sharesLockFor (const std::string& language)
    {
        return language.empty();
    }

    std::unique_ptr<ProcessInstance> ProcessHost::makeInstance (const Slot& slot) const
    {
        if (slot.language.empty())
        {
            PdInstance::Settings pd;
            pd.patchFile = (std::filesystem::path (settings.cacheFolder) / (slot.cue + ".pd")).string();
            pd.searchPaths = settings.searchPaths;
            return std::make_unique<PdInstance> (pd);
        }
        return nullptr;
    }

    void ProcessHost::remember (std::vector<Input>& pending, Input input)
    {
        //  MIDI keeps every message: a note's on and its off are both the news.
        if (input.to == midiName)
        {
            pending.push_back (std::move (input));
            return;
        }

        for (auto& waiting : pending)
            if (waiting.to == input.to)
            {
                waiting = std::move (input);
                return;
            }
        pending.push_back (std::move (input));
    }

    void ProcessHost::stepped (Slot& slot)
    {
        if (slot.phase == Phase::needsMake)
            slot.phase = slot.instance->made() ? Phase::needsOpen : Phase::needsMake;
        else if (slot.phase == Phase::needsOpen)
            slot.phase = Phase::running;

        //  What Pd could not open is said where a [print] would say it.
        if (slot.phase == Phase::running && ! slot.instance->opened())
            if (auto problem = slot.instance->problem(); ! problem.empty())
                slot.result.printed.push_back (std::move (problem));
    }

    void ProcessHost::collect (Slot& slot)
    {
        auto outbox = slot.instance->takeOutbox();
        for (auto& sent : outbox.sent)
        {
            if (sent.to != midiName)
            {
                //  A send to `out` is named by its address, its first word.
                const auto& name = sent.to == "out" ? sent.selector : sent.to;
                auto atoms = sent.atoms;
                if (sent.to != "out" && sent.selector != "float" && sent.selector != "symbol"
                     && sent.selector != "list" && sent.selector != "bang")
                    atoms.insert (atoms.begin(), Atom::of (sent.selector));
                slot.portValues[name] = std::move (atoms);
            }
            slot.result.sent.push_back (std::move (sent));
        }
        for (auto& line : outbox.printed)
            slot.result.printed.push_back (std::move (line));
        slot.result.dropped += outbox.dropped;
        slot.droppedTotal += outbox.dropped;
        slot.busy = false;
        slot.ticking = false;
        slot.lateInARow = 0;
    }

    std::vector<ProcessResult> ProcessHost::tick (const std::vector<WantedProcess>& wanted, const InputsFor& inputsFor,
                                                  std::chrono::microseconds budget, int stuckAfter)
    {
        const auto deadline = Clock::now() + budget;

        //  WHAT SHOULD BE RUNNING, against what is: a run no longer wanted, or
        //  wanted in another language, is closed - and made again in the new one.
        for (auto& slot : slots)
        {
            if (slot->phase == Phase::closing)
                continue;
            const auto want = std::find_if (wanted.begin(), wanted.end(), [&] (const auto& w) { return w.run == slot->run; });
            if (want == wanted.end() || want->language != slot->language)
                slot->phase = Phase::closing;
        }
        for (const auto& want : wanted)
        {
            auto found = std::find_if (slots.begin(), slots.end(), [&] (const auto& s)
            {
                return s->run == want.run && s->phase != Phase::closing;
            });
            if (found == slots.end())
            {
                auto slot = std::make_unique<Slot>();
                slot->run = want.run;
                slot->cue = want.cue;
                slot->language = want.language;
                slot->sharesLock = sharesLockFor (want.language);
                slot->wanted = want.patch;
                slot->result.run = want.run;
                slots.push_back (std::move (slot));
            }
            else
            {
                (*found)->wanted = want.patch;
            }
        }

        for (auto& slot : slots)
        {
            slot->result = ProcessResult {};
            slot->result.run = slot->run;
        }

        //  1. WHAT CAME BACK SINCE THE LAST TICK, and what is late again.
        for (auto& slot : slots)
        {
            //  A stuck patch never comes back; a stuck script, interrupted, does.
            if (! slot->busy || (slot->stuck && slot->sharesLock))
                continue;

            if (slot->instance->finished (std::chrono::microseconds (0)))
            {
                const auto wasStep = ! slot->ticking;
                collect (*slot);
                if (wasStep)
                    stepped (*slot);
            }
            else if (! slot->stuck)
            {
                //  A step counts as a tick does: a patch whose [loadbang] never
                //  ends holds Pd's shared lock as it opens, and is stuck too.
                ++slot->lateTicks;
                if (++slot->lateInARow >= stuckAfter
                     && Clock::now() - slot->posted >= std::chrono::milliseconds (20) * stuckAfter)
                {
                    slot->stuck = true;
                    slot->result.failure = "process-stuck";
                    if (slot->sharesLock)
                        stuck = true;
                    else
                        slot->instance->interrupt();
                }
            }
        }

        //  2. THE QUIET POINT, for Pd: made, opened, opened again, closed. A
        //  script, sharing no lock, steps whenever it is not mid-job.
        const auto pdQuiet = ! stuck && std::none_of (slots.begin(), slots.end(), [] (const auto& s)
        {
            return s->sharesLock && s->busy;
        });

        std::vector<Slot*> stepping;

        {
            for (auto& slot : slots)
            {
                if (slot->sharesLock ? ! pdQuiet : slot->busy)
                    continue;

                if (slot->phase == Phase::closing)
                {
                    if (slot->instance != nullptr && slot->instance->made())
                    {
                        slot->instance->close();
                        slot->posted = Clock::now();
                        slot->busy = true;
                        stepping.push_back (slot.get());
                    }
                    continue;
                }

                if (slot->failed)
                    continue;

                if (slot->phase == Phase::needsMake)
                {
                    slot->instance = makeInstance (*slot);
                    if (slot->instance == nullptr)
                    {
                        slot->failed = true;
                        slot->result.printed.push_back ("Go.dot does not run " + slot->language);
                        continue;
                    }
                    slot->instance->make();
                    slot->posted = Clock::now();
                    slot->busy = true;
                    stepping.push_back (slot.get());
                }
                else if (slot->phase == Phase::needsOpen
                          || (slot->phase == Phase::running && slot->wanted != slot->text))
                {
                    slot->text = slot->wanted;
                    slot->instance->open (slot->text);
                    slot->rowsHanded.clear();
                    slot->portValues.clear();
                    slot->posted = Clock::now();
                    slot->busy = true;
                    stepping.push_back (slot.get());
                }
            }

            for (auto* slot : stepping)
            {
                if (! slot->instance->finished (left (deadline)))
                    continue;

                collect (*slot);
                stepped (*slot);
            }
        }
        if (stuck)
        {
            //  NOTHING IS OPENED OR CLOSED AGAIN IN PD (ACK): a patch's run that
            //  would need it fails, and an instance no longer wanted is let go unfreed.
            for (auto& slot : slots)
            {
                if (! slot->sharesLock || slot->stuck || slot->failed)
                    continue;

                if (slot->phase == Phase::needsMake || slot->phase == Phase::needsOpen)
                    slot->result.failure = "pd-held";
                if (slot->phase == Phase::closing && slot->instance != nullptr)
                    slot->instance->leaveUnfreed();
            }
        }

        //  A patch's step still out - this tick's or an earlier one's - may be
        //  holding, or waiting for, Pd's shared lock: a patch's tick posted now
        //  would wait behind it. A script's tick never would.
        const auto stepsOut = std::any_of (slots.begin(), slots.end(), [] (const auto& s)
        {
            return s->sharesLock && s->busy && ! s->ticking;
        });

        //  3. THE TICK, unless a step is still holding Pd's shared lock.
        std::vector<Slot*> ticking;

        for (auto& slot : slots)
        {
            if (slot->phase != Phase::running || slot->failed || slot->stuck)
                continue;

            auto inputs = inputsFor ? inputsFor (slot->run, slot->instance->receives()) : TickInputs {};
            for (auto& heard : inputs.heard)
                remember (slot->pending, std::move (heard));
            for (auto& row : inputs.rows)
            {
                auto handed = slot->rowsHanded.find (row.to);
                if (handed != slot->rowsHanded.end() && handed->second == row.atoms)
                    continue;
                slot->rowsHanded[row.to] = row.atoms;
                remember (slot->pending, std::move (row));
            }

            if (slot->busy || (slot->sharesLock && stepsOut))
                continue;

            //  What it is handed at a name it hears, for the ports readout.
            const auto hears = slot->instance->receives();
            for (const auto& input : slot->pending)
                if (std::find (hears.begin(), hears.end(), input.to) != hears.end())
                    slot->portValues[input.to] = input.atoms;

            if (slot->instance->tick (std::exchange (slot->pending, {})))
            {
                slot->posted = Clock::now();
                slot->busy = true;
                slot->ticking = true;
                ticking.push_back (slot.get());
            }
        }

        for (auto* slot : ticking)
        {
            if (slot->instance->finished (left (deadline)))
            {
                collect (*slot);
            }
            else
            {
                ++slot->lateTicks;
                ++slot->lateInARow;
            }
        }

        //  WHAT EACH RUN SAYS, and the closed ones let go.
        std::vector<ProcessResult> results;

        for (auto& slot : slots)
        {
            if (slot->phase == Phase::closing)
                continue;

            auto& result = slot->result;
            result.lateTicks = slot->lateTicks;

            std::size_t lines = 0;
            for (const auto& [name, atoms] : slot->portValues)
            {
                if (lines++ == 64)
                    break;
                result.ports += name + (atoms.empty() ? std::string {} : " " + textOf (atoms)) + "\n";
            }
            result.droppedTotal = slot->droppedTotal;
            result.state = slot->stuck ? "stuck"
                         : slot->phase != Phase::running ? "starting"
                         : slot->busy ? "late"
                                      : "running";
            if (! result.failure.empty())
                slot->failed = true;
            results.push_back (std::move (result));
        }

        slots.erase (std::remove_if (slots.begin(), slots.end(), [this] (const auto& slot)
        {
            if (slot->phase != Phase::closing)
                return false;
            if (slot->instance == nullptr)
                return true;
            if (slot->sharesLock && (stuck || slot->stuck))
            {
                slot->instance->leaveUnfreed();
                return true;
            }
            //  A stuck script still ending its job: its own thread frees it.
            if (! slot->sharesLock && slot->stuck && slot->busy)
            {
                slot->instance->leaveUnfreed();
                return true;
            }
            return ! slot->busy;
        }), slots.end());

        return results;
    }
}
