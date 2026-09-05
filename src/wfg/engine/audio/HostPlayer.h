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

#pragma once

/*
    The cue layer's Player, over a real Tracktion graph.

    THE THREAD SEAM, and the whole reason this class exists rather than the
    Runner calling AudioHost directly.

    Arming writes a Tracktion ValueTree - it points a clip at a file, which
    rebuilds the playback graph - and every one of those writes asserts the
    message thread. GO does not: it is two atomic stores under a spin mutex the
    audio thread only try-locks. So the two halves of what the cue layer asks
    for run on different threads, and this is the only place that knows it.

    requestArm therefore RETURNS IMMEDIATELY and posts the work. It does not
    wait for the disk either: assigning a voice is quick and getting a file
    mapped into the audio cache is not, so the two are reported separately -
    `audio.armed` says the voice is yours, and isArmReady answers, once a tick,
    whether the sound would actually come out. A launch placed in the gap plays
    silence for as long as the disk takes with the run reporting itself as
    playing, which is the worst shape a failure can have.

    IT SUBMITS RATHER THAN WRITING. When the posted work completes it hands the
    result back as a command, on whatever thread it finished on, and the tick
    thread applies it in order like anything a client sent. That is what keeps
    the model single-writer, and what puts the arm in the log where a replay can
    reproduce it.
*/

#include <wfg/engine/audio/AudioHost.h>
#include <wfg/engine/cue/Runner.h>

#include <juce_events/juce_events.h>

#include <mutex>
#include <vector>

namespace wfg
{
    class Engine;
}

namespace wfg::audio
{
    class HostPlayer final : public cue::Player,
                             private juce::Timer
    {
    public:
        /*  Neither reference may outlive the player. `engine` is submitted to
            from the message thread, which EventQueue allows from any thread but
            the audio one. */
        HostPlayer (AudioHost& host, Engine& engine);
        ~HostPlayer() override;

        //======================================================================
        int trackCount() const override;
        int blockSize() const override;
        int channelsPerTrack() const override;
        std::int64_t samplesElapsed() const override;

        void requestArm (const cue::ArmRequest&) override;

        bool launchAtSample (int track, std::int64_t sample) override;
        bool stop (int track) override;
        bool isPlaying (int track) const override;
        bool isArmReady (int track) const override;

        /*  Performs every queued arm. MESSAGE THREAD - it writes a Tracktion
            ValueTree, which every one of those writes asserts.

            Public, and called by a timer this class starts, because
            JUCE_MODAL_LOOPS_PERMITTED is 0 in this project - a modal loop in a
            show engine is a hang - so a posted lambda would be work nothing
            could drive or observe. A test drives it directly; a show lets the
            timer do it. */
        void serviceArms();

    private:
        void timerCallback() override;

        AudioHost& audioHost;
        Engine& engine;

        /*  The channels a track carries, learned when the Edit was built. Held
            rather than asked for because a cue's routing is resolved against it
            on the tick thread, and reaching into the Edit from there is exactly
            what this class exists to prevent. */
        int trackChannels = 2;

        std::mutex queueMutex;
        std::vector<cue::ArmRequest> queued;
    };
}
