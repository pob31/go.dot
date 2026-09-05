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

        {
            const std::lock_guard<std::mutex> lock { queueMutex };
            work.swap (queued);
        }

        for (const auto& request : work)
        {
            /*  THE VALUETREE WRITE, on the thread Tracktion asserts. Pointing
                the clip at the file rebuilds the playback graph, which is why
                this is not on the GO path. */
            if (! audioHost.setTrackSource (request.track, request.mediaFile))
            {
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
    }

    //==============================================================================
    bool HostPlayer::launchAtSample (int track, std::int64_t sample)
    {
        /*  The tick thread, and the whole of what GO does to the audio side:
            one sample turned into a beat through the anchor, then two stores. */
        return audioHost.launchTrackAt (track, audioHost.beatsAtSample (sample));
    }

    bool HostPlayer::stop (int track)           { return audioHost.stopTrack (track); }

    bool HostPlayer::stopAtSample (int track, std::int64_t sample)
    {
        return audioHost.stopTrackAt (track, audioHost.beatsAtSample (sample));
    }

    void HostPlayer::setLevelDb (int track, double levelDb)
    {
        /*  The tick thread, fifty times a second while a fade runs. One relaxed
            atomic store; the audio side interpolates between the values. */
        if (auto* matrix = audioHost.trackMatrix (track))
            matrix->setLevelDb (static_cast<float> (levelDb));
    }

    bool HostPlayer::isPlaying (int track) const
    {
        return audioHost.trackPlayState (track).playing;
    }

    bool HostPlayer::isArmReady (int track) const
    {
        return audioHost.isTrackSourceReady (track);
    }
}
