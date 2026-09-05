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

#include <wfg/engine/cue/Runner.h>

#include <wfg/engine/Engine.h>

#include <algorithm>

namespace wfg::cue
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        std::string kindOfCue (const juce::ValueTree& cue)
        {
            const auto element = cue.getType().toString();

            if (element == "Cue")   return "memo";
            if (element == "Group") return "group";
            if (element == "Media") return "media";

            return {};
        }

        std::string mediaFileOf (const juce::ValueTree& cue)
        {
            return cue[juce::Identifier ("file")].toString().toStdString();
        }

        std::vector<osc::Value> one (const std::string& text)
        {
            return { osc::Value::string (text) };
        }

        std::vector<osc::Value> two (const std::string& text, std::int32_t number)
        {
            return { osc::Value::string (text), osc::Value::int32 (number) };
        }
    }

    //==============================================================================
    int launchLatencyTicks (int blockSize, int samplesPerTick) noexcept
    {
        if (blockSize <= 0 || samplesPerTick <= 0)
            return 0;

        /*  ceil ((3 * blockSize - 1) / samplesPerTick), in integers, plus one
            tick of guard. See the derivation in the header. */
        const auto needed = 3 * static_cast<std::int64_t> (blockSize) - 1;
        const auto ticks = (needed + samplesPerTick - 1) / samplesPerTick;

        return 1 + static_cast<int> (ticks);
    }

    //==============================================================================
    Runner::Runner (const doc::ShowDocument& documentToRead, RunTable& runsToDrive,
                    doc::IdRegistry& runIds, Focus& focusToUse)
        : document (documentToRead), runs (runsToDrive), ids (runIds), focus (focusToUse)
    {
    }

    int Runner::latencyTicks() const noexcept
    {
        if (audio == nullptr)
            return 0;

        return launchLatencyTicks (audio->blockSize(), samplesPerTick);
    }

    //==============================================================================
    std::string Runner::arm (Engine& engine, const std::string& cueId, const std::string& runId)
    {
        return armInternal (engine, cueId, runId, false);
    }

    std::string Runner::fire (Engine& engine, const std::string& cueId, const std::string& runId)
    {
        return armInternal (engine, cueId, runId, true);
    }

    std::string Runner::armInternal (Engine& engine, const std::string& cueId,
                                     const std::string& runId, bool fireAtOnce)
    {
        const auto cue = document.findById (cueId);

        if (! cue.isValid())
            return {};

        const auto kind = kindOfCue (cue);

        /*  A memo is a line in the book and a group has no runner until Phase 3.
            Both are legal things to press GO on; neither makes a run. */
        if (kind != "media")
            return {};

        if (const auto* live = runs.liveRunOf (cueId))
        {
            /*  ARMED IS NOT RUNNING, and the difference is the whole point of
                arming ahead. A cue armed when it reached standby is sitting on
                a reserved voice with its media ready and no sound coming out;
                GO is what turns that into a launch. Treating it as "already
                running" would have made the fast path - the one the design
                exists for - the one where GO does nothing at all.

                DECISION B, 2026-09-05, is about a cue that is actually
                SOUNDING: the GO is applied, standby has advanced, and the
                playing instance carries on untouched. No restart, and no second
                instance stacked on another voice. */
            if (fireAtOnce && live->state == runState::armed)
                if (auto* armed = runs.find (live->id))
                    armed->launchRequested = true;

            return live->id;
        }

        auto id = runId;

        if (id.empty())
            id = ids.generate();

        runs.create (id, cueId, kind);
        auto* run = runs.find (id);

        if (run == nullptr)
            return {};

        run->launchRequested = fireAtOnce;

        /*  NO AUDIO SIDE IS A COMPLETE CONFIGURATION, not a failure. A show
            replayed has no Player and must still create the run, advance
            standby and write the same log - only the sound is missing. */
        if (audio == nullptr)
            return id;

        const auto file = mediaFileOf (cue);

        if (file.empty())
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (id),
                             osc::Value::string (runError::mediaMissing) });
            return id;
        }

        /*  The lowest free voice, and a playing one is never stolen. Lowest
            rather than round-robin so that a show replayed puts the same cue on
            the same track and two logs of one session compare line for line. */
        const auto track = runs.lowestFreeTrack (audio->trackCount());

        if (track < 0)
        {
            engine.submit (origin::engine, "run.failed",
                           { osc::Value::string (id),
                             osc::Value::string (runError::noTrack) });
            return id;
        }

        /*  Reserved from here, so a second arm on the same tick cannot pick the
            same voice. The audio side confirms with audio.armed once the graph
            and the disk are ready; until then the run is armed and silent. */
        run->track = track;

        audio->requestArm (id, track, file);
        return id;
    }

    //==============================================================================
    void Runner::beforeTick (Engine& engine, std::int64_t tick)
    {
        if (audio == nullptr)
            return;

        launchIfDue (engine, tick);
        observeEdges (engine);
    }

    void Runner::launchIfDue (Engine& engine, std::int64_t tick)
    {
        juce::ignoreUnused (tick);

        const auto ticksAhead = latencyTicks();

        if (ticksAhead <= 0 || samplesPerTick <= 0)
            return;

        const auto now = audio->samplesElapsed();
        const auto blockSize = static_cast<std::int64_t> (audio->blockSize());

        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || ! run->launchRequested || run->isFinished())
                continue;

            /*  Not until the audio side says the voice is ready. A launch
                placed before the disk has answered plays silence for as long as
                the disk takes, with the run reporting itself as playing
                throughout - the worst shape a failure can have. */
            if (! run->armConfirmed || run->track < 0)
                continue;

            /*  THE LAUNCH INSTANT. Placed a whole number of ticks ahead of the
                tick being processed, so it is a pure function of the tick index
                and the schedule - which is what makes it reproduce on replay
                rather than depending on when this loop happened to run. */
            const auto target = now + static_cast<std::int64_t> (ticksAhead) * samplesPerTick;

            /*  The guarantee the arithmetic exists to provide, checked rather
                than assumed. A tick thread that overslept would otherwise place
                a launch too close and buy a hole in the cue. */
            if (target - now < 2 * blockSize)
                continue;

            if (audio->launchAtSample (run->track, target))
            {
                run->launchRequested = false;
                run->launchedAtSample = target;

                engine.submit (origin::engine, "run.started", one (run->id));
            }
        }
    }

    void Runner::observeEdges (Engine& engine)
    {
        for (const auto& snapshot : runs.all())
        {
            auto* run = runs.find (snapshot.id);

            if (run == nullptr || run->track < 0 || run->isFinished())
                continue;

            const auto playing = audio->isPlaying (run->track);

            /*  A run that was sounding and is not any more has ended. The
                launcher clip stops itself at the end of its length, which is
                the file's, so this is the ordinary way a cue finishes as well
                as how a stop is noticed. */
            if (run->sawPlaying && ! playing)
            {
                engine.submit (origin::engine, "run.ended", one (run->id));
                run->sawPlaying = false;
                continue;
            }

            if (playing)
                run->sawPlaying = true;
        }
    }

    //==============================================================================
    void registerGoCommands (CommandRegistry& registry, Engine& engine, Runner& runner,
                             doc::ShowDocument& document, Focus& focus,
                             doc::IdRegistry& runIds)
    {
        juce::ignoreUnused (runIds);

        const auto withRun = [] (std::vector<osc::Value> args, std::size_t index,
                                 const std::string& id)
        {
            if (args.size() > index)
                args[index] = osc::Value::string (id);
            else
                args.push_back (osc::Value::string (id));

            return args;
        };

        //----------------------------------------------------------------------
        /*  ARMING LIVES HERE, WITH THE RUNNER, because it is an ACTION and not
            a report: it reserves a voice and asks the audio side for media.
            RunCommands holds only what the machine says happened, which is what
            keeps that file applicable on a machine with no sound card.

            It is a command in its own right and not an internal step, because
            §4.11 says every gesture-reachable action is one - a surface that
            wants a cue ready before the operator's hand moves has to be able to
            ask. */
        registry.add ({ "audio.arm",
                        "Reserves a voice for a cue and makes its media ready, without playing"
                        " it. What standby does ahead of GO.",
                        { { "cue", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();
                            const auto cue = document.findById (cueId);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            /*  A cue that plays nothing cannot be made ready to
                                play. Arming a memo would create a run that could
                                never leave `armed`, which looks like progress
                                and is not. */
                            if (cue.getType().toString() != "Media")
                                return Outcome::rejected (reason::typeMismatch);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.arm (engine, cueId, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "go",
                        "Fires the focused list's standby cue and moves standby to the next one.",
                        { { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, &focus, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto list = focus.list (document);

                            if (! list.isValid())
                                return Outcome::rejected (reason::notInList);

                            const auto listId = list[idProperty].toString().toStdString();
                            const auto standby = list[juce::Identifier ("standby")]
                                                     .toString().toStdString();

                            /*  GO with nothing in standby is applied and does
                                nothing. An operator at the end of a list has
                                not made a mistake, and an R record every time
                                would bury the rejections that matter. */
                            if (standby.empty())
                                return Outcome::ok (args);

                            /*  STANDBY MOVES FIRST, and unconditionally (§3.5).
                                Whether the cue makes a sound, fails to find a
                                voice, or is a memo, the pointer has advanced -
                                which is what lets an operator press GO down a
                                list at speed without waiting to see what each
                                one did. */
                            const auto next = nextOf (list, standby);
                            document.setAttribute (standbyAddressOf (listId), next);

                            const auto id = args.empty() ? std::string {}
                                                         : args[0].getString();

                            return Outcome::ok (withRun (args, 0,
                                                         runner.fire (engine, standby, id)));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "cue.fire",
                        "Fires a named cue without touching standby - what a button on a surface"
                        " does.",
                        { { "cue", 's', false }, { "run", 's', true } },
                        true,
                        [&engine, &runner, &document, withRun]
                        (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = args[0].getString();

                            if (! document.findById (cueId).isValid())
                                return Outcome::rejected (reason::unknownId);

                            const auto id = args.size() > 1 ? args[1].getString()
                                                            : std::string {};

                            return Outcome::ok (withRun (args, 1,
                                                         runner.fire (engine, cueId, id)));
                        } });
    }
}
