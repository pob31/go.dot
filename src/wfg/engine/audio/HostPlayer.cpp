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

#include <wfg/engine/audio/HostPlayer.h>

#include <wfg/engine/Engine.h>

namespace wfg::audio
{
    HostPlayer::HostPlayer (AudioHost& hostToDrive, Engine& engineToReportTo)
        : audioHost (hostToDrive), engine (engineToReportTo)
    {
        trackChannels = audioHost.editChannelsPerTrack();

        /*  A TIMER RATHER THAN callAsync, and the reason is a project decision
            rather than a preference: JUCE_MODAL_LOOPS_PERMITTED is 0 here,
            because a modal loop in a show engine is a hang. A posted lambda
            would then be work whose completion nothing could drive or observe -
            not by a test, and not by anything that wanted to know whether the
            arm had happened yet.

            So the queue is explicit and serviceArms() is a real function.
            Started here because a HostPlayer is built on the message thread,
            which is the one Tracktion's ValueTree writes assert. */
        startTimer (10);
    }

    HostPlayer::~HostPlayer()
    {
        stopTimer();
    }

    //==============================================================================
    int HostPlayer::trackCount() const          { return audioHost.trackCount(); }
    int HostPlayer::blockSize() const           { return audioHost.settings().blockSize; }
    int HostPlayer::channelsPerTrack() const    { return trackChannels; }

    std::int64_t HostPlayer::samplesElapsed() const
    {
        return audioHost.clock().samplesElapsed();
    }

    //==============================================================================
    void HostPlayer::requestArm (const cue::ArmRequest& request)
    {
        /*  The tick thread, taking a lock - which is allowed here and forbidden
            three functions down. PRD §4.2 is about the AUDIO thread, and §4.1
            is about the GO path; an arm is neither. It happens while the
            operator is reading the next line, and the lock is held for a
            push_back. */
        const std::lock_guard<std::mutex> lock { queueMutex };
        queued.push_back (request);
    }

    void HostPlayer::serviceArms()
    {
        std::vector<cue::ArmRequest> work;
        std::vector<StateWanted> states;

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            work.swap (queued);
            states.swap (statesQueued);
        }

        for (const auto& wanted : states)
            if (auto* lane = audioHost.proxyLane (wanted.track, wanted.slot))
                lane->wantState (wanted.path);

        for (const auto& request : work)
        {
            /*  THE VALUETREE WRITE, on the thread Tracktion asserts. Pointing
                the clip at the file rebuilds the playback graph, which is why
                this is not on the GO path. */
            /*  RANGES AND NO RANGES ARE THE SAME CALL, and the empty list is
                the Phase 2 shape: the whole file into slot nought. With ranges
                it is a clip per range, each armed LOOPING so the launcher
                builds no stop duration for it - see setTrackRanges, where the
                reason that is the mechanism rather than a setting is written
                down. */
            std::vector<audio::AudioHost::RangeSpec> ranges;
            ranges.reserve (request.ranges.size());

            for (const auto& range : request.ranges)
                ranges.push_back ({ range.in, range.out, range.loops });

            if (! audioHost.setTrackRanges (request.track, request.mediaFile, ranges,
                                            request.startOffset))
            {
                /*  MEDIA-MISSING COVERS ALL THREE, for now: a file that is not
                    there, one that is there and is not audio, and a range that
                    is not inside it are all "this cue cannot be made ready",
                    and `lastError` carries which. A `no-slot`
                    of its own arrives with the run-level range reporting in
                    PR 3.9, where there is something to report it against. */
                engine.submit (origin::engine, "run.failed",
                               { osc::Value::string (request.runId),
                                 osc::Value::string (cue::runError::mediaMissing) });
                continue;
            }

            std::vector<std::array<double, 3>> coefficients;
            coefficients.reserve (request.routing.size());

            for (const auto& c : request.routing)
                coefficients.push_back ({ static_cast<double> (c.input),
                                          static_cast<double> (c.output),
                                          static_cast<double> (c.gain) });

            audioHost.setTrackRouting (request.track, request.levelDb, coefficients);

            /*  And the cue's EQ, snapped with the routing while the voice is
                silent, its delay lines cleared at the next block so the
                previous cue's tail is not in them (Phase 9a). */
            audioHost.snapTrackEq (request.track, request.eq);

            /*  And its inserts: each entry switched in or out with the values
                the cue sets, the instance reset before its next block (Phase
                9a, PR 9a.8). */
            for (const auto& fx : request.fx)
                audioHost.snapTrackFx (request.track, fx.slot, fx.enabled, fx.values, fx.statePath);

            /*  The voice is yours. Whether the sound would come out YET is a
                different question, asked separately through isArmReady - the
                graph is ready long before the disk is, and a launch in that gap
                plays silence with the run reporting itself as playing. */
            engine.submit (origin::engine, "audio.armed",
                           { osc::Value::string (request.runId),
                             osc::Value::int32 (request.track) });
        }
    }

    void HostPlayer::timerCallback()
    {
        serviceArms();

        /*  And the sandbox's judgement, on the same ten milliseconds: whether
            each child is up, answering, or to be marked failed (Phase 9a). */
        audioHost.pollProxies();
    }

    //==============================================================================
    int HostPlayer::slotCount() const  { return audioHost.slotCount(); }
    int HostPlayer::sampleRate() const { return audioHost.settings().sampleRate; }

    bool HostPlayer::launchAtSample (int track, int slot, std::int64_t sample)
    {
        /*  The tick thread, and the whole of what GO does to the audio side:
            one sample turned into a beat through the anchor, then two stores. */
        return audioHost.launchTrackAt (track, slot, audioHost.beatsAtSample (sample));
    }

    bool HostPlayer::stop (int track)           { return audioHost.stopTrack (track); }

    bool HostPlayer::stopAtSample (int track, int slot, std::int64_t sample)
    {
        return audioHost.stopTrackAt (track, slot, audioHost.beatsAtSample (sample));
    }

    void HostPlayer::setLevelDb (int track, double levelDb)
    {
        /*  The tick thread, fifty times a second while a fade runs. One relaxed
            atomic store; the audio side interpolates between the values. */
        if (auto* matrix = audioHost.trackMatrix (track))
            matrix->setLevelDb (static_cast<float> (levelDb));
    }

    void HostPlayer::setRouting (int track, const std::vector<cue::Coefficient>& coefficients)
    {
        auto* matrix = audioHost.trackMatrix (track);

        if (matrix == nullptr)
            return;

        /*  ONE PASS, EVERY CELL, each written once with the value it is to
            end at. Clearing the matrix and then filling it - which is what an
            arm does - would leave every target at nought in between, and a
            block processed in that window would start every smoother sliding
            towards silence and back. An arm can afford it because the voice is
            not sounding; this cannot, because the whole reason it exists is
            that the voice is.

            The search is linear in a list of a few dozen coefficients, over a
            matrix of a couple of thousand cells, on an edit rather than on a
            tick. */
        for (int input = 0; input < matrix->numInputs(); ++input)
            for (int output = 0; output < matrix->numOutputs(); ++output)
            {
                auto gain = 0.0f;

                for (const auto& coefficient : coefficients)
                    if (coefficient.input == input && coefficient.output == output)
                    {
                        gain = coefficient.gain;
                        break;
                    }

                matrix->setGain (input, output, gain);
            }
    }

    void HostPlayer::setEq (int track, const EqSettings& settings)
    {
        /*  The tick thread, on an edit: relaxed stores into the voice's EQ,
            one per number that moved, and a release per section so the
            audio thread rebuilds only those. No message thread anywhere. */
        audioHost.setTrackEq (track, settings);
    }

    void HostPlayer::setFxEnabled (int track, int slot, bool enabled)
    {
        audioHost.setTrackFxEnabled (track, slot, enabled);
    }

    void HostPlayer::setFxParameter (int track, int slot, int parameter, float normalised)
    {
        audioHost.setTrackFxParameter (track, slot, parameter, normalised);
    }

    bool HostPlayer::isPlaying (int track) const
    {
        return audioHost.trackPlayState (track).playing;
    }

    float HostPlayer::takeOutputPeak (int track)
    {
        return audioHost.takeTrackOutputPeak (track);
    }

    bool HostPlayer::isArmReady (int track) const
    {
        /*  READY IS THE DISK AND THE STATES: a cue whose plugin is still
            taking its whole state would sound through the last cue's
            (the author's decision of 2026-09-25) - so it waits, and is late
            by the load rather than wrong, and `run.late` says by how much. */
        return audioHost.isTrackSourceReady (track) && audioHost.isTrackFxSettled (track);
    }

    void HostPlayer::requestFxState (int track, int slot, const std::string& path)
    {
        /*  The tick thread: the lane counts it NOW, so a launch due this tick
            waits; the path goes to the message thread, under the same short
            lock an arm takes. */
        if (auto* lane = audioHost.proxyLane (track, slot))
            lane->expectState();

        const std::lock_guard<std::mutex> lock { queueMutex };
        statesQueued.push_back ({ track, slot, path });
    }
}
