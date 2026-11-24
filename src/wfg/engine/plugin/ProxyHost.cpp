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

#include <wfg/engine/plugin/ProxyHost.h>
#include <wfg/engine/plugin/ChildLaunch.h>
#include <wfg/engine/plugin/PluginMeasure.h>
#include <wfg/engine/plugin/ProcessUtil.h>
#include <wfg/engine/app/RealtimeRights.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace wfg::plugin
{
    std::int64_t proxyFloorFor (int sampleRate, int blockSize) noexcept
    {
        if (sampleRate <= 0 || blockSize <= 0)
            return ProxyLane::defaultDeadlineMicroseconds;

        const auto blockPeriodUs = static_cast<std::int64_t> (blockSize) * 1000000 / sampleRate;
        return std::max<std::int64_t> (1, std::min<std::int64_t> (ProxyLane::defaultDeadlineMicroseconds,
                                                                  blockPeriodUs / 4));
    }

    std::int64_t proxyDeadlineFor (int sampleRate, int blockSize, std::int64_t requestedMicroseconds) noexcept
    {
        if (requestedMicroseconds > 0)
            return requestedMicroseconds;

        const auto floor = proxyFloorFor (sampleRate, blockSize);

        if (sampleRate <= 0 || blockSize <= 0)
            return floor;

        /*  A SHARE OF THE BLOCK PERIOD (namespace draft §139, BHB), and never
            less than the old rule gave. */
        const auto blockPeriodUs = static_cast<double> (blockSize) * 1000000.0 / static_cast<double> (sampleRate);
        return std::max (floor, static_cast<std::int64_t> (blockPeriodUs * ProxyLane::laneCapFraction));
    }

    //==============================================================================
    struct ProxyHost::Impl
    {
        enum class State { unloaded, loading, loaded, missing, failed };

        Impl (ProxySpec specToUse, std::vector<ProxyLane*> lanesToUse, PluginTable* tableToUse)
            : spec (std::move (specToUse)), lanes (std::move (lanesToUse)), table (tableToUse)
        {
            deadlineUs = proxyDeadlineFor (spec.sampleRate, spec.maxSamples, spec.deadlineMicroseconds);

            //  A limit asked for is fixed: its floor is itself, and no callback's end cuts it.
            floorUs = spec.deadlineMicroseconds > 0 ? deadlineUs
                                                    : proxyFloorFor (spec.sampleRate, spec.maxSamples);
        }

        //======================================================================
        static const char* wordFor (State state) noexcept
        {
            switch (state)
            {
                case State::loading: return "loading";
                case State::loaded:  return "loaded";
                case State::missing: return "missing";
                case State::failed:  return "failed";
                case State::unloaded: break;
            }

            return "unloaded";
        }

        PluginTable::Status statusNow() const
        {
            PluginTable::Status status;
            status.state = wordFor (state);
            status.problem = problem;
            /*  WHAT GO.DOT ADDS, SAID WITH WHAT THE PLUGIN DECLARES (namespace
                draft §149, BJX): the latest of this child's lanes, so the
                foot's words, the rack's budget and a timeline's line-up have
                one figure. */
            status.latencySamples = latencySamples + laterTold;
            status.paramCount = paramCount;
            status.stateLoadMs = stateLoadMs;
            status.stateProblem = stateProblem;
            status.inputs = inputs;
            status.outputs = outputs;
            status.layout = layout;
            status.keyInputs = keyInputs;
            status.lateBlocks = lateBlocksTold;
            status.late = lateSpell ? std::string (lateWords) : std::string();
            return status;
        }

        /*  LATE BLOCKS, SAID (namespace draft §139, BHE). Counted by the lanes
            a block at a time; told here twice a spell - when the first late
            block of one is seen, and two seconds after its last - with the
            total as it then stands. Before this the count was read by nothing
            but the eight-in-a-row rule, and a cue could crackle all evening
            with an empty log. */
        void watchLate (std::uint32_t now)
        {
            std::uint32_t missed = 0, starved = 0, moved = 0;
            auto later = 0;
            ProxyLane::Worst worst;

            for (auto* lane : lanes)
            {
                if (lane == nullptr)
                    continue;

                missed += lane->misses();
                starved += lane->starved();
                moved += lane->movedLater();
                later = std::max (later, lane->lateSamples());
                foldWorst (worst, lane->takeWorst());
            }

            foldWorst (worstSeen, worst);

            /*  PLAYED LATER, OR BACK WHERE IT WAS (§149): the delay the entry
                says moves with the lane's, the poll it is seen. */
            if (later != laterTold)
            {
                laterTold = later;
                publish();
            }

            const auto total = missed + starved;
            const auto fresh = total != lateBlocksSeen;
            lateBlocksSeen = total;

            if (fresh)
            {
                lateLastAt = now;

                if (! lateSpell)
                {
                    lateSpell = true;
                    spell = {};
                    spellBeganAt = now;
                    spellMissedBefore = missedSeen;
                    spellStarvedBefore = starvedSeen;
                    spellMovedBefore = movedSeen;
                    tellLate();
                }
            }

            missedSeen = missed;
            starvedSeen = starved;
            movedSeen = moved;

            /*  WHERE THE SPELL'S TIME WENT (namespace draft §148): the longest
                wait and the longest work of every poll from the one that saw
                its first late block - a block is counted late at its limit,
                and the child notes its times when it answers, which is after -
                to the spell's end, two seconds past its last. */
            if (lateSpell)
            {
                const auto raisedBefore = std::max (spell.pickupMicros, spell.workMicros);
                spell.pickupMicros = std::max (spell.pickupMicros, worst.pickupMicros);
                spell.workMicros = std::max (spell.workMicros, worst.workMicros);

                if (std::max (spell.pickupMicros, spell.workMicros) > raisedBefore)
                    spell.core = worst.core;

                spell.missed = missed - spellMissedBefore;
                spell.starved = starved - spellStarvedBefore;
                spell.seconds = static_cast<double> (lateLastAt - spellBeganAt) / 1000.0;
                spell.allowedMicros = allowedUs();
                spell.laterSamples = moved != spellMovedBefore ? later : 0;
            }

            if (! fresh && lateSpell && now - lateLastAt > static_cast<std::uint32_t> (lateSpellEndsAfterMs))
                endLateSpell();
        }

        /*  What a plugin has of a buffer, for the note: to the callback's
            end (§149, BJY) - six tenths of the block period, less whatever
            came before it in the callback - or the limit fixed by hand. */
        std::int64_t allowedUs() const noexcept
        {
            if (spec.deadlineMicroseconds > 0 || spec.sampleRate <= 0 || spec.maxSamples <= 0)
                return deadlineUs;

            return std::max (deadlineUs, static_cast<std::int64_t> (
                                             ProxyLane::callbackFraction * 1000000.0 * static_cast<double> (spec.maxSamples)
                                               / static_cast<double> (spec.sampleRate)));
        }

        static void foldWorst (ProxyLane::Worst& into, const ProxyLane::Worst& from) noexcept
        {
            const auto before = std::max (into.pickupMicros, into.workMicros);
            into.pickupMicros = std::max (into.pickupMicros, from.pickupMicros);
            into.workMicros = std::max (into.workMicros, from.workMicros);

            if (std::max (into.pickupMicros, into.workMicros) > before)
                into.core = from.core;
        }

        void endLateSpell()
        {
            if (! lateSpell)
                return;

            lateSpell = false;
            lastSpell = spell;
            tellLate();

            if (lateNote)
                lateNote (ProxyHost::lateNoteOf (spec.name.empty() ? spec.identifier : spec.name, spec.pluginId, spell,
                                                 app::coreKindOf (spell.core)));

            /*  WHAT THE SPELL TAUGHT (namespace draft §149, BJZ). The plugin's
                own work, with the margin a measure is planned at, was longer
                than a buffer leaves it: that becomes its measure where it is
                more than what is kept, and the next build starts this plugin
                as late as it ended up needing. A spell whose time was all in
                the waiting - the machine's - teaches nothing; the lane gives
                that back at its next reset (BKD). The wait is not compared
                with the work: a plugin played later has its next request
                queued behind the one it is at, so its waits are as long as
                its work and say nothing of whose the time was (the driver
                tests/blackbox/later.py found that the first rule never
                taught a served show anything). */
            if (! spec.engineFolder.empty()
                  && static_cast<double> (spell.workMicros) * measure::margin > static_cast<double> (allowedUs()))
            {
                PluginMeasure learnt;
                learnt.workMicros = spell.workMicros;
                learnt.blockSize = spec.maxSamples;
                learnt.sampleRate = spec.sampleRate;
                learnt.by = "show";
                measure::keep (spec.engineFolder, spec.identifier, learnt, true);
            }
        }

        void tellLate()
        {
            lateBlocksTold = lateBlocksSeen;
            publish();

            if (late)
            {
                const auto words = lateSpell ? std::string (lateWords) : std::string();
                late (spec.pluginId, lateBlocksTold, words);

                for (const auto& id : spec.alsoIds)
                    late (id, lateBlocksTold, words);
            }
        }

        /** Writes the table and tells the tree, when something a reader could see moved. */
        void publish()
        {
            if (table == nullptr)
                return;

            const auto status = statusNow();
            auto moved = table->set (spec.pluginId, status);

            /*  And every other entry it stands for, each the same: one child,
                one truth about it (Phase 9b). */
            for (const auto& id : spec.alsoIds)
                moved = table->set (id, status) || moved;

            if (moved && changed)
                changed();
        }

        //======================================================================
        bool makeRegion (std::string& why)
        {
            const juce::File folder { juce::String (spec.regionFolder) };
            folder.createDirectory();

            /*  Named by the entry and by this process, so two engines on one
                machine - a show and a test - never share a file. */
            regionFile = folder.getChildFile ("proxy-" + juce::String (spec.pluginId) + juce::String (spec.regionTag)
                                              + "-" + juce::String (process::currentId()) + ".shm");

            spec.queueSlots = std::clamp (spec.queueSlots, 0, region::queueSlotsMost);
            const auto bytes = region::regionBytes (spec.channels, spec.maxSamples, spec.lanes, spec.queueSlots);

            {
                juce::FileOutputStream out (regionFile);

                if (! out.openedOk())
                {
                    why = "could not create the shared region at " + regionFile.getFullPathName().toStdString();
                    return false;
                }

                out.setPosition (0);
                out.truncate();

                std::vector<char> zeros (std::min<std::size_t> (bytes, 1u << 20), 0);

                for (std::size_t written = 0; written < bytes;)
                {
                    const auto chunk = std::min (zeros.size(), bytes - written);
                    out.write (zeros.data(), chunk);
                    written += chunk;
                }
            }

            mapping = std::make_unique<juce::MemoryMappedFile> (regionFile, juce::MemoryMappedFile::readWrite, false);

            if (mapping->getData() == nullptr || mapping->getSize() < bytes)
            {
                why = "could not map the shared region at " + regionFile.getFullPathName().toStdString();
                mapping.reset();
                regionFile.deleteFile();
                return false;
            }

            /*  LAID OUT ONCE, HERE. The atomics are constructed in place over
                the zeroed file; the child never constructs, it reads. */
            auto* base = mapping->getData();
            header = new (base) region::Header {};
            header->magic.store (region::magic, std::memory_order_relaxed);
            header->version.store (region::version, std::memory_order_relaxed);
            header->channels.store (static_cast<std::uint32_t> (spec.channels), std::memory_order_relaxed);
            header->maxSamples.store (static_cast<std::uint32_t> (spec.maxSamples), std::memory_order_relaxed);
            header->lanes.store (static_cast<std::uint32_t> (spec.lanes), std::memory_order_relaxed);
            header->queueSlots.store (static_cast<std::uint32_t> (spec.queueSlots), std::memory_order_relaxed);
            header->sampleRate.store (static_cast<std::uint32_t> (spec.sampleRate), std::memory_order_relaxed);
            header->blockSize.store (static_cast<std::uint32_t> (spec.maxSamples), std::memory_order_relaxed);
            header->layoutHash.store (region::layoutHashFor (spec.channels, spec.maxSamples, spec.lanes, spec.queueSlots),
                                      std::memory_order_release);

            for (int i = 0; i < spec.lanes; ++i)
            {
                auto* lane = new (region::laneAt (base, spec.channels, spec.maxSamples, i, spec.queueSlots)) region::Lane {};

                for (auto& value : lane->params)
                    value.store (region::useBaseline, std::memory_order_relaxed);
            }

            for (std::size_t i = 0; i < lanes.size() && i < static_cast<std::size_t> (spec.lanes); ++i)
            {
                if (lanes[i] == nullptr)
                    continue;

                auto* lane = region::laneAt (base, spec.channels, spec.maxSamples, static_cast<int> (i), spec.queueSlots);
                lanes[i]->setAllowance (deadlineUs, floorUs);

                //  As late as the measure says from the start, and more at once where that is not enough (§149).
                lanes[i]->setLate (spec.queueSlots > 0 ? spec.lateSamples : 0, spec.queueSlots > 0);
                lanes[i]->setCallEnabled (false);
                lanes[i]->bind (header, lane, region::audioOf (lane), spec.channels, spec.maxSamples);
            }

            return true;
        }

        void clearChildFields()
        {
            if (header == nullptr)
                return;

            header->childShouldExit.store (0, std::memory_order_relaxed);
            header->childReady.store (0, std::memory_order_relaxed);
            header->childFailed.store (0, std::memory_order_relaxed);
            header->catalogueReady.store (0, std::memory_order_relaxed);
            header->inWorkgroup.store (0, std::memory_order_relaxed);
            std::memset (header->problem, 0, sizeof (header->problem));
        }

        bool launch (std::string& why)
        {
            endChild();
            clearChildFields();

            for (auto* lane : lanes)
                if (lane != nullptr)
                {
                    lane->setCallEnabled (false);
                    lane->clearMisses();
                    lane->forgetState();
                }

            juce::StringArray command;
            command.add (spec.launch.executable.empty()
                             ? juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName()
                             : juce::String (spec.launch.executable));

            for (const auto& word : spec.launch.leadingArgs)
                command.add (juce::String (word));

            command.add ("--region=" + regionFile.getFullPathName());
            command.add ("--plugin=" + juce::String (spec.identifier));
            command.add ("--instances=" + juce::String (spec.lanes));
            command.add ("--channels=" + juce::String (spec.channels));
            command.add ("--parent-pid=" + juce::String (process::currentId()));

            if (! spec.presetPath.empty())
                command.add ("--preset=" + juce::String (spec.presetPath));

            /*  The description beside the region, for the child to make the
                plugin from; and where its catalogue goes, beside both. */
            if (! spec.descriptionXml.empty())
            {
                descriptionFile = regionFile.withFileExtension ("plugin.xml");
                descriptionFile.replaceWithText (juce::String (spec.descriptionXml), false, false, "\n");
                command.add ("--description=" + descriptionFile.getFullPathName());
            }

            catalogueFile = regionFile.withFileExtension ("catalogue.json");
            catalogueFile.deleteFile();
            catalogueRead = false;
            command.add ("--catalogue-file=" + catalogueFile.getFullPathName());

            /*  No pipes for its output: nothing reads them, and a full pipe
                is what hangs a child that prints. */
            std::vector<std::string> words;

            for (const auto& word : command)
                words.push_back (word.toStdString());

            child = std::make_unique<ChildLaunch>();

            if (! child->start (words))
            {
                child.reset();
                why = "could not start the plugin host process " + command[0].toStdString();
                return false;
            }

            launchedAt = juce::Time::getMillisecondCounter();
            state = State::loading;
            problem.clear();
            publish();
            return true;
        }

        /** Tells it to leave, gives it a moment, and ends what is still there. */
        void endChild()
        {
            if (child == nullptr)
                return;

            if (header != nullptr)
                header->childShouldExit.store (1, std::memory_order_release);

            for (int waited = 0; waited < 40 && child->isRunning(); ++waited)
                juce::Thread::sleep (25);

            if (child->isRunning())
                child->kill();

            child.reset();
        }

        //======================================================================
        void fail (const std::string& sentence)
        {
            for (auto* lane : lanes)
                if (lane != nullptr)
                    lane->setCallEnabled (false);

            //  A spell of late blocks ends in the failure, which says the rest.
            endLateSpell();

            /*  A child that stopped answering is put down now, so the restart
                launches into a clean region rather than beside a hung twin -
                and put down at once, not asked: this runs on the message
                thread, and a second spent waiting politely on a process that
                is not answering would be a second of no arms and no window. */
            if (child != nullptr)
            {
                child->kill();
                child.reset();
            }

            const auto now = juce::Time::getMillisecondCounter();
            failures.erase (std::remove_if (failures.begin(), failures.end(),
                                            [now] (std::uint32_t at) { return now - at > static_cast<std::uint32_t> (failureWindowMs); }),
                            failures.end());
            failures.push_back (now);

            state = State::failed;
            problem = sentence;

            if (failures.size() == 1)
            {
                restartDueAt = std::max<std::uint32_t> (1, now + static_cast<std::uint32_t> (restartDelayMs));
                problem += "; " + (spec.silentWords.empty() ? std::string ("every voice using it is silent until it is back")
                                                            : spec.silentWords)
                         + "; restarting in two seconds";
            }
            else
            {
                restartDueAt = 0;
                problem += "; " + (spec.silentWords.empty() ? std::string ("every voice using it is silent until it is back")
                                                            : spec.silentWords)
                         + "; failed again inside a minute, so it stays down until plugin.restart";
            }

            publish();

            if (failed)
            {
                failed (spec.pluginId, problem);

                for (const auto& id : spec.alsoIds)
                    failed (id, problem);
            }
        }

        void becomeLoaded()
        {
            latencySamples = static_cast<int> (header->latencySamples.load (std::memory_order_relaxed));
            reportedLatency = latencySamples;
            inputs = static_cast<int> (header->inputs.load (std::memory_order_relaxed));
            outputs = static_cast<int> (header->outputs.load (std::memory_order_relaxed));
            keyInputs = static_cast<int> (std::min<std::uint32_t> (header->keyInputs.load (std::memory_order_relaxed),
                                                                   static_cast<std::uint32_t> (region::keyChannelsMax)));
            header->layout[region::problemChars - 1] = 0;
            layout = header->layout;
            laneLatency.assign (lanes.size(), 0);
            paramCount = static_cast<int> (std::min<std::uint32_t> (header->paramCount.load (std::memory_order_relaxed),
                                                                    static_cast<std::uint32_t> (region::maxParams)));

            /*  EACH LANE GIVEN BACK THE STATE IT HELD (CU), counted before it
                is called again: a voice that was sounding stays silent until
                the new child holds its cue's state, then fades back in where
                the cue has got to. */
            for (auto* lane : lanes)
                if (lane != nullptr)
                {
                    lane->clearMisses();
                    lane->restoreState();
                    lane->setCallEnabled (true);
                }

            state = State::loaded;
            problem.clear();
            publish();
        }

        void poll()
        {
            if (header == nullptr)
                return;

            const auto now = juce::Time::getMillisecondCounter();

            if (state == State::loading)
            {
                if (header->childFailed.load (std::memory_order_acquire) != 0)
                {
                    header->problem[region::problemChars - 1] = 0;
                    const std::string why (header->problem);
                    fail (why.empty() ? "the plugin host could not bring the plugin up" : why);
                    return;
                }

                if (header->childReady.load (std::memory_order_acquire) != 0)
                {
                    becomeLoaded();
                    return;
                }

                if (child == nullptr || ! child->isRunning())
                {
                    fail ("the plugin host process died while loading");
                    return;
                }

                if (now - launchedAt > static_cast<std::uint32_t> (readyTimeoutMs))
                    fail ("the plugin host did not come up in five seconds");

                return;
            }

            if (state == State::loaded)
            {
                /*  THE CATALOGUE, read once when the child says it is there,
                    into the machine's cache - which the tree compares at every
                    publish, so the param nodes follow without anyone marking
                    it stale from this thread. */
                if (! catalogueRead && spec.catalogues != nullptr
                     && header->catalogueReady.load (std::memory_order_acquire) != 0)
                {
                    catalogueRead = true;
                    Catalogue catalogue;
                    std::string why;

                    if (catalogueFile.existsAsFile()
                         && Catalogue::fromJson (catalogueFile.loadFileAsString().toStdString(), catalogue, why)
                         && catalogue.identifier == spec.identifier)
                        spec.catalogues->put (catalogue);
                }

                auto anyEnabled = false;

                for (std::size_t i = 0; i < lanes.size(); ++i)
                {
                    auto* lane = lanes[i];

                    if (lane == nullptr)
                        continue;

                    anyEnabled = anyEnabled || lane->isEnabled();

                    /*  EIGHT IN A ROW, AND FOR A TENTH OF A SECOND (BHD): the
                        run timed from the poll that first saw it. */
                    const auto lateInARow = lane->consecutiveMisses();

                    if (i >= lateSince.size())
                        lateSince.resize (lanes.size(), 0);

                    if (lateInARow == 0)
                        lateSince[i] = 0;
                    else if (lateSince[i] == 0)
                        lateSince[i] = std::max<std::uint32_t> (1, now);

                    if (lateInARow >= static_cast<std::uint32_t> (ProxyLane::missesBeforeFailure)
                         && now - lateSince[i] >= static_cast<std::uint32_t> (failureAfterMs))
                    {
                        fail ("the plugin stopped answering: " + std::to_string (ProxyLane::missesBeforeFailure)
                              + " blocks late in a row on "
                              + (i < spec.laneWords.size() ? spec.laneWords[i]
                                                           : "voice " + std::to_string (i + 1)));
                        return;
                    }
                }

                header->wantSpin.store (anyEnabled ? 1u : 0u, std::memory_order_relaxed);
                watchLate (now);

                if (child == nullptr || ! child->isRunning())
                {
                    fail ("the plugin host process died");
                    return;
                }

                /*  A CUE'S WHOLE STATE onto each voice that is asked for one
                    (the author's decision of 2026-09-25), one in flight a
                    lane, the child's answer read back: how long it took, and
                    why it could not. A child that has been loading one for
                    five seconds is hung, and failed like any other. */
                for (std::size_t index = 0; index < lanes.size(); ++index)
                {
                    auto* lane = lanes[index];

                    if (lane == nullptr)
                        continue;

                    const auto news = lane->serviceState (now);

                    if (news.late)
                    {
                        fail ("a cue's state has been loading for more than five seconds: the plugin host is stuck");
                        return;
                    }

                    /*  AND A RESET (namespace draft §139, BHC), which the
                        lane waits for without calling the child: one owed
                        that long is the same child, stuck. */
                    if (lane->resetOverdue (now))
                    {
                        fail ("a plugin's reset has been running for more than five seconds: the plugin host is stuck");
                        return;
                    }

                    if (news.arrived)
                    {
                        stateLoadMs = news.loadMs;
                        stateProblem = news.failed ? news.problem : std::string();

                        /*  THE LATENCY, the largest any voice's instance
                            declares since its state (2026-09-26): a state can
                            move a look-ahead. */
                        if (index < laneLatency.size())
                            laneLatency[index] = news.latencySamples;

                        latencySamples = reportedLatency;

                        for (const auto each : laneLatency)
                            latencySamples = std::max (latencySamples, each);

                        publish();
                    }
                }

                return;
            }

            if (state == State::failed && restartDueAt != 0 && now - restartDueAt < 0x7fffffffu)
            {
                restartDueAt = 0;
                std::string why;

                if (! launch (why))
                {
                    state = State::failed;
                    problem = why;
                    publish();
                }
            }
        }

        //======================================================================
        void stop()
        {
            endChild();

            for (auto* lane : lanes)
                if (lane != nullptr)
                {
                    lane->setCallEnabled (false);
                    lane->unbind();
                }

            header = nullptr;
            mapping.reset();
            regionFile.deleteFile();
            descriptionFile.deleteFile();
            catalogueFile.deleteFile();

            if (state != State::unloaded)
            {
                state = State::unloaded;
                problem.clear();
                publish();
            }
        }

        //======================================================================
        ProxySpec spec;
        std::vector<ProxyLane*> lanes;
        PluginTable* table = nullptr;
        FailureHandler failed;
        ChangeHandler changed;
        LateHandler late;

        /** Late blocks as the lanes count them, as last told, and the spell in progress (BHE). */
        std::uint32_t lateBlocksSeen = 0;
        std::uint32_t lateBlocksTold = 0;
        std::uint32_t lateLastAt = 0;
        bool lateSpell = false;

        /*  The spell in progress, as it will be said when it ends, and the one
            before it (namespace draft §148); the two counts apart as last
            seen, and as they stood when the spell began. */
        LateNoteHandler lateNote;
        ProxyHost::LateSpell spell, lastSpell;
        std::uint32_t spellBeganAt = 0;
        std::uint32_t missedSeen = 0, starvedSeen = 0;
        std::uint32_t spellMissedBefore = 0, spellStarvedBefore = 0;

        /** How often the lanes have been given more, as last seen and when the spell began; and the delay last said (§149). */
        std::uint32_t movedSeen = 0, spellMovedBefore = 0;
        int laterTold = 0;

        /** The longest wait and work of any lane since `takeWorstSeen` last asked. */
        ProxyLane::Worst worstSeen;

        juce::File regionFile;
        juce::File descriptionFile;
        juce::File catalogueFile;
        bool catalogueRead = false;
        std::unique_ptr<juce::MemoryMappedFile> mapping;
        region::Header* header = nullptr;
        std::unique_ptr<ChildLaunch> child;

        State state = State::unloaded;
        std::string problem;
        int latencySamples = 0;
        int reportedLatency = 0;
        std::vector<int> laneLatency;
        std::vector<std::uint32_t> lateSince;      ///< when each lane's run of late blocks was first seen, or nought
        int paramCount = 0;
        int inputs = 0;
        int outputs = 0;
        int keyInputs = 0;
        std::string layout;
        double stateLoadMs = 0.0;
        std::string stateProblem;
        std::int64_t deadlineUs = ProxyLane::defaultDeadlineMicroseconds;
        std::int64_t floorUs = ProxyLane::defaultDeadlineMicroseconds;

        std::uint32_t launchedAt = 0;
        std::uint32_t restartDueAt = 0;
        std::vector<std::uint32_t> failures;
    };

    //==============================================================================
    ProxyHost::ProxyHost (ProxySpec spec, std::vector<ProxyLane*> lanes, PluginTable* table)
        : impl (std::make_unique<Impl> (std::move (spec), std::move (lanes), table))
    {
    }

    ProxyHost::~ProxyHost()
    {
        stop();
    }

    void ProxyHost::onFailed (FailureHandler handler)   { impl->failed = std::move (handler); }
    void ProxyHost::onLate (LateHandler handler)        { impl->late = std::move (handler); }
    void ProxyHost::onLateNote (LateNoteHandler handler) { impl->lateNote = std::move (handler); }

    ProxyHost::LateSpell ProxyHost::lastLateSpell() const { return impl->lastSpell; }

    ProxyLane::Worst ProxyHost::takeWorstSeen()
    {
        const auto worst = impl->worstSeen;
        impl->worstSeen = {};
        return worst;
    }

    /*  Whole numbers put together by hand: a note reads the same whatever the
        machine's locale, where a printf would say "0,8 ms" on a French one. */
    std::string ProxyHost::lateNoteOf (const std::string& name, const std::string& pluginId, const LateSpell& spell,
                                       const std::string& coreKind)
    {
        const auto fixed = [] (std::uint64_t thousandths, int decimals)
        {
            //  `thousandths` of the unit, said to one or two places.
            const auto whole = thousandths / 1000u;
            const auto rest = decimals == 1 ? (thousandths % 1000u) / 100u : (thousandths % 1000u) / 10u;
            auto text = std::to_string (whole) + ".";

            if (decimals == 2 && rest < 10u)
                text += "0";

            return text + std::to_string (rest);
        };

        const auto ms = [&fixed] (std::uint64_t micros) { return fixed (micros, 2) + " ms"; };
        const auto seconds = static_cast<std::uint64_t> (std::max (0.0, spell.seconds) * 1000.0 + 0.5);

        auto note = "plugin " + name + " (" + pluginId + "): " + std::to_string (spell.missed) + " late";

        if (spell.starved > 0)
            note += ", " + std::to_string (spell.starved) + " cut short by the callback's end";

        note += ", in " + fixed (seconds, 1) + " s; longest wait for its thread " + ms (spell.pickupMicros)
              + ", longest in the plugin " + ms (spell.workMicros)
              + ", against " + ms (static_cast<std::uint64_t> (std::max<std::int64_t> (0, spell.allowedMicros))) + " allowed";

        if (! coreKind.empty())
            note += "; its thread was on " + std::string (coreKind == "efficiency" ? "an " : "a ") + coreKind + " core";

        if (spell.laterSamples > 0)
            note += "; played " + std::to_string (spell.laterSamples) + " samples later from now";

        return note;
    }
    void ProxyHost::onChanged (ChangeHandler handler)   { impl->changed = std::move (handler); }

    bool ProxyHost::start (std::string& problem)
    {
        problem.clear();

        if (impl->spec.lanes <= 0 || impl->spec.channels <= 0 || impl->spec.maxSamples <= 0)
        {
            problem = "a proxy needs at least one lane, one channel and a block size";
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        /*  MISSING IS ITS OWN WORD (§17.2): the show names a plugin this
            machine's scan does not know. No child is launched for it; the
            entry says so, and a cue that switches it in is silent (CU). Asked
            once more first, since a scan may have found it since. */
        if (impl->spec.descriptionXml.empty() && impl->spec.describe
              && ! Catalogue::isTestIdentifier (impl->spec.identifier))
            impl->spec.descriptionXml = impl->spec.describe (impl->spec.identifier);

        if (impl->spec.descriptionXml.empty() && ! Catalogue::isTestIdentifier (impl->spec.identifier))
        {
            problem = "this machine's scan does not know " + impl->spec.identifier
                        + "; scan for it in Show settings, Plugins, or install it";
            impl->state = Impl::State::missing;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        if (impl->header == nullptr && ! impl->makeRegion (problem))
        {
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        if (! impl->launch (problem))
        {
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        return true;
    }

    void ProxyHost::poll()
    {
        impl->poll();
    }

    bool ProxyHost::restart (std::string& problem)
    {
        problem.clear();

        if (impl->header == nullptr)
            return start (problem);

        /*  Asked for by name: the count of failures that gates the automatic
            restart starts again. */
        impl->failures.clear();
        impl->restartDueAt = 0;

        if (! impl->launch (problem))
        {
            impl->state = Impl::State::failed;
            impl->problem = problem;
            impl->publish();
            return false;
        }

        return true;
    }

    void ProxyHost::stop()
    {
        impl->stop();
    }

    //==============================================================================
    const std::string& ProxyHost::pluginId() const noexcept     { return impl->spec.pluginId; }

    bool ProxyHost::serves (const std::string& id) const noexcept
    {
        return id == impl->spec.pluginId
                 || std::find (impl->spec.alsoIds.begin(), impl->spec.alsoIds.end(), id) != impl->spec.alsoIds.end();
    }
    void ProxyHost::alsoServe (const std::string& id)
    {
        if (id.empty() || serves (id))
            return;

        impl->spec.alsoIds.push_back (id);
        impl->publish();
    }

    PluginTable::Status ProxyHost::status() const               { return impl->statusNow(); }
    std::string ProxyHost::regionPath() const                   { return impl->regionFile.getFullPathName().toStdString(); }
    bool ProxyHost::childIsRunning() const                      { return impl->child != nullptr && impl->child->isRunning(); }
    std::int64_t ProxyHost::childPid() const noexcept           { return impl->child != nullptr ? impl->child->pid() : 0; }
    std::int64_t ProxyHost::deadlineMicroseconds() const noexcept { return impl->deadlineUs; }
    region::Header* ProxyHost::header() noexcept                { return impl->header; }

    void ProxyHost::killChild()
    {
        if (impl->child != nullptr)
            impl->child->kill();
    }
}
