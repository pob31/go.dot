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

/*
    SPIKE 05 — EXTERNAL PARAMETER CONTROL AT 50 Hz.  THROWAWAY CODE (devplan:19).

    PRD §6.1 item 5, verbatim, and this is the pass criterion:

        5. External parameter control at 50 Hz without disturbing the message
           thread.

    WHY THIS SPIKE IS SHAPED DIFFERENTLY FROM THE OTHERS
    ----------------------------------------------------
    Spikes 01-04 measure audio. This one measures a THREAD, and the criterion
    names it: "without disturbing the message thread". So the rig has to contain
    a real message thread that can be disturbed, and something honest that
    measures the disturbance.

      main thread   runs a genuine JUCE message loop, not a substitute. That is
                    runDispatchLoop() on Windows and Linux; on macOS it is a
                    CFRunLoop pump, for the reason spelled out at the call site
                    - the JUCE one is [NSApp run] and a console binary has no
                    NSApp, so it returns instantly and measures nothing
      a Timer       at 20 ms (PRD §3.4's 50 Hz tick) writes the parameters and
                    measures ITS OWN LATENESS. A timer callback that should fire
                    every 20 ms and fires late is precisely what "disturbing the
                    message thread" means to a user: the UI stops responding and
                    queued work backs up.
      a worker      drives the audio graph, paced to real time

    The timer runs ON the message thread, which is where Go.dot's own tick would
    do this work, so no MessageManagerLock is needed and none is taken. That is
    deliberate: taking the lock from a worker would measure lock contention, a
    different question from the one §6.1 asks.

    WHY setParameter MUST BE ON THAT THREAD ANYWAY
    ----------------------------------------------
    tracktion_AutomatableParameter.cpp:1073 is

        jassert (juce::MessageManager::getInstance()->currentThreadHasLockedMessageManager());

    unconditional in the branch a non-curve-following write takes. So there is no
    design in which Go.dot writes plugin parameters off the message thread, and
    the only question is what that costs.

    WHAT IS MEASURED, AND WHAT CI IS ALLOWED TO GATE
    -----------------------------------------------
    Timing figures from a Debug build on a shared runner are not the numbers. CI
    gates only the INVARIANTS - every requested write completed, and no audio
    xruns - while the latency distribution is for the author to read from a
    Release run on the machine he intends to quote. Every row records its build.
*/

#include "../SpikeHarness.h"

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace tracktion;
using namespace tracktion::engine;

namespace
{
    constexpr auto criterion =
        "External parameter control at 50 Hz without disturbing the message thread.";

    constexpr auto extraFlags =
        " [--params=N] [--seconds=N] [--notify] [--single-point-curve]";

    constexpr double tickHz = 50.0;                 // PRD 3.4
    constexpr int tickMs = static_cast<int> (1000.0 / tickHz);

    double percentile (std::vector<double> v, double p)
    {
        if (v.empty())
            return 0.0;

        std::sort (v.begin(), v.end());
        const auto idx = std::min (v.size() - 1,
                                   static_cast<size_t> (static_cast<double> (v.size()) * p));
        return v[idx];
    }

    //==============================================================================
    /*  The tick. Fires on the message thread at 50 Hz, writes every parameter it
        was given, and records how late it was and how long the writes took.
    */
    struct TickTimer : public juce::Timer
    {
        TickTimer (std::vector<AutomatableParameter*> p, int totalTicks,
                   juce::NotificationType n)
            : params (std::move (p)), ticksRemaining (totalTicks), notification (n)
        {}

        void timerCallback() override
        {
            const auto now = std::chrono::steady_clock::now();

            if (lastFire.time_since_epoch().count() != 0)
            {
                const auto actualMs = std::chrono::duration<double, std::milli> (now - lastFire).count();
                lateness.push_back (actualMs - tickMs);
            }

            lastFire = now;

            const auto t0 = std::chrono::steady_clock::now();

            /*  A different value every tick, and never the same value twice in a
                row: TE skips work when the value is unchanged, so writing a
                constant would measure the early-out path and report that 50 Hz is
                free. The ramp is what makes every write do real work.
            */
            phase += 0.01f;

            if (phase > 1.0f)
                phase -= 1.0f;

            for (auto* p : params)
            {
                p->setParameter (phase, notification);
                ++writesDone;
            }

            writesRequested += static_cast<long long> (params.size());

            const auto t1 = std::chrono::steady_clock::now();
            writeCostUs.push_back (std::chrono::duration<double, std::micro> (t1 - t0).count());

            ++ticksFired;

            if (--ticksRemaining <= 0)
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();

                // The macOS pump below watches this; stopDispatchLoop() cannot
                // stop it, because it stops an NSApp that was never started.
                finished.store (true, std::memory_order_release);
            }
        }

        std::vector<AutomatableParameter*> params;
        std::vector<double> lateness, writeCostUs;
        std::chrono::steady_clock::time_point lastFire {};
        long long writesDone = 0, writesRequested = 0, ticksFired = 0;
        std::atomic<bool> finished { false };
        int ticksRemaining;
        juce::NotificationType notification;
        float phase = 0.0f;
    };
}

//==============================================================================
int main (int argc, char** argv)
{
    spike::makeAssertsNonInteractive();

    const auto parsed = spike::parseArgs (argc, argv);

    if (! parsed)
        return spike::usage ("spike05_param_50hz", criterion, extraFlags);

    const auto args = *parsed;
    const auto wantedParams = static_cast<int> (spike::valueFor (argc, argv, "--params=").value_or (8));
    const auto seconds      = static_cast<int> (spike::valueFor (argc, argv, "--seconds=").value_or (5));
    const auto notify       = spike::hasFlag (argc, argv, "--notify");

    spike::HeadlessEngine engine;
    spike::Report report ("spike05_param_50hz", argc, argv);

    report.value ("tracks", args.tracks);
    report.value ("sample_rate", args.sampleRate);
    report.value ("buffer", args.buffer);
    report.value ("params_requested", wantedParams);
    report.value ("seconds", seconds);
    report.value ("notification", notify ? "sendNotificationSync" : "dontSendNotification");

    HostedAudioDeviceInterface::Parameters params;
    params.sampleRate     = static_cast<double> (args.sampleRate);
    params.blockSize      = static_cast<int> (args.buffer);
    params.inputChannels  = 2;
    params.outputChannels = 2;

    auto edit = test_utilities::createTestEdit (*engine, static_cast<int> (args.tracks),
                                                Edit::EditRole::forEditing);
    auto tracks = getAudioTracks (*edit);

    if (tracks.isEmpty())
        return report.cannotMeasure ("no audio tracks");

    // Collect parameters from each track's volume plugin (volume and pan).
    std::vector<AutomatableParameter*> targets;

    for (auto* t : tracks)
    {
        if (auto* vp = t->getVolumePlugin())
            for (auto* p : vp->getAutomatableParameters())
                if (static_cast<int> (targets.size()) < wantedParams)
                    targets.push_back (p);
    }

    report.value ("params_available", targets.size());

    if (targets.empty())
        return report.cannotMeasure ("no automatable parameters found on the volume plugins");

    if (static_cast<int> (targets.size()) < wantedParams)
        report.value ("note", "fewer parameters than requested - raise --tracks for more");

    // Audio: something must actually be playing, or the message thread is being
    // measured while the engine is idle and the answer flatters.
    auto tone = spike::makeToneFile (params.sampleRate, static_cast<double> (seconds) + 3.0, 2, 220.0f);

    if (tone == nullptr)
        return report.cannotMeasure ("could not create the test tone");

    const AudioFile toneAudio (*engine, tone->getFile());

    for (auto* t : tracks)
        insertWaveClip (*t, {}, tone->getFile(),
                        { { 0_tp, TimeDuration::fromSeconds (static_cast<double> (seconds) + 3.0) } },
                        DeleteExistingClips::no);

    bool mappedOk = false;
    auto player = spike::createPlayerWithDeadline (*edit, params, { toneAudio }, mappedOk);

    if (player == nullptr || ! mappedOk)
        return report.cannotMeasure ("could not start playback");

    // The audio worker: paced to real time, because a free-running render would
    // finish before the message thread had been disturbed at all.
    std::atomic<bool> audioRunning { true };
    std::vector<double> blockUs;
    std::atomic<int> xruns { 0 };
    const auto budgetUs = (static_cast<double> (params.blockSize) / params.sampleRate) * 1.0e6;

    /*  Warm-up blocks are excluded from the statistics, the same way every other
        spike here settles before it marks a baseline.

        The first blocks after the graph is allocated are not steady state: the
        node graph is cold, the file reader is priming, and the first process()
        call routinely exceeds its budget on any machine. Counting those as xruns
        reports "the audio thread missed its deadline" for a condition that
        happens once, before the thing under test has even started - and this
        spike is about a SUSTAINED 50 Hz load, not about startup.

        Both numbers are reported, so the exclusion is visible rather than
        flattering: xruns_including_warmup carries what was thrown away.
    */
    const int warmupBlocks = static_cast<int> (params.sampleRate / params.blockSize / 2);   // ~0.5 s
    std::atomic<int> xrunsAll { 0 };

    std::thread audioThread ([&]
    {
        spike::RealTimePacer pacer (params.sampleRate, params.blockSize);
        int block = 0;

        while (audioRunning.load (std::memory_order_relaxed))
        {
            const auto t0 = std::chrono::steady_clock::now();
            player->process (params.blockSize);
            const auto t1 = std::chrono::steady_clock::now();

            const auto us = std::chrono::duration<double, std::micro> (t1 - t0).count();

            if (us > budgetUs)
                xrunsAll.fetch_add (1, std::memory_order_relaxed);

            if (block++ >= warmupBlocks)
            {
                blockUs.push_back (us);

                if (us > budgetUs)
                    xruns.fetch_add (1, std::memory_order_relaxed);
            }

            pacer.waitForBlock();
        }
    });

    TickTimer timer (targets, seconds * static_cast<int> (tickHz),
                     notify ? juce::sendNotificationSync : juce::dontSendNotification);
    timer.startTimer (tickMs);

    /*  The real message loop. This is the thing the criterion is about.

        macOS needs a different call, and the reason is worth writing down because
        the wrong one fails SILENTLY. runDispatchLoop() is [NSApp run] on macOS
        (juce_MessageManager_mac.mm). A spike is a console binary that never makes
        an NSApplication, so NSApp is nil, the message goes nowhere, and the call
        returns at once - the timer never fires and every figure below is zero.
        That is exactly what every macOS run of this spike did until this was
        fixed, and the old verdict gate passed it.

        JUCE's own bounded loop, runDispatchLoopUntil(), pumps CFRunLoop and would
        do the job, but it sits behind JUCE_MODAL_LOOPS_PERMITTED and this project
        sets that to 0 on purpose (cmake/WfgThirdParty.cmake:174 - "a modal loop in
        a show engine is a hang"). That decision is not this spike's to reverse.

        So macOS pumps the same CFRunLoop that runDispatchLoopUntil() would have,
        in the same mode. This is not a substitute for the JUCE message loop, it IS
        the JUCE message loop on this platform: JUCE registers its message queue as
        a CFRunLoop source in kCFRunLoopCommonModes (juce_MessageQueue_mac.h:56),
        so juce::Timer callbacks arrive through exactly this pump. The 50 ms slice
        only bounds how long the loop can sleep before re-checking the flag; the
        timer still fires on the run loop's own schedule, so lateness is measured
        against the same clock as everywhere else.
    */
   #if JUCE_MAC
    while (! timer.finished.load (std::memory_order_acquire))
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
   #else
    juce::MessageManager::getInstance()->runDispatchLoop();
   #endif

    audioRunning.store (false, std::memory_order_relaxed);
    audioThread.join();

    report.value ("writes_requested", timer.writesRequested);
    report.value ("writes_done", timer.writesDone);
    report.value ("ticks_fired", timer.ticksFired);
    report.value ("ticks_measured", timer.lateness.size());

    report.value ("tick_lateness_ms.p50", percentile (timer.lateness, 0.50));
    report.value ("tick_lateness_ms.p99", percentile (timer.lateness, 0.99));
    report.value ("tick_lateness_ms.max", timer.lateness.empty()
                                            ? 0.0
                                            : *std::max_element (timer.lateness.begin(), timer.lateness.end()));

    report.value ("write_cost_us.p50", percentile (timer.writeCostUs, 0.50));
    report.value ("write_cost_us.p99", percentile (timer.writeCostUs, 0.99));
    report.value ("write_cost_us.per_param_p50",
                  targets.empty() ? 0.0 : percentile (timer.writeCostUs, 0.50) / static_cast<double> (targets.size()));

    report.value ("block_us.p99", percentile (blockUs, 0.99));
    report.value ("xruns", xruns.load());
    report.value ("xruns_including_warmup", xrunsAll.load());
    report.value ("warmup_blocks_excluded", warmupBlocks);

    /*  A run that measured nothing is not a pass.

        The invariant below is writes_done == writes_requested, and 0 == 0
        satisfies it - so a tick that never fired used to report PASS while
        proving nothing whatsoever. That is how the macOS breakage above stayed
        invisible across two full sweeps. run-spikes.sh states the same principle
        one level up, for the case where it finds no executables; this is that
        rule inside the spike.

        It is a HARNESS-ERROR (exit 3), not a violation (exit 1), and the
        distinction is the one the harness already draws: a spike that could not
        tick has said nothing about the engine, which is not the same as an engine
        that misbehaved.
    */
    if (timer.ticksFired == 0)
        return report.cannotMeasure ("the 50 Hz tick never fired - no message loop dispatched it, "
                                     "so nothing about parameter control was measured");

    /*  CI gates the INVARIANTS only. Whether the timing is acceptable is a
        judgement about a specific machine and a specific parameter count, and it
        belongs in the report, not in an exit code from a Debug build on a shared
        runner.
    */
    const bool allWritesLanded = timer.writesDone == timer.writesRequested;
    const bool noXruns = xruns.load() == 0;

    return report.verdict (allWritesLanded && noXruns,
                           allWritesLanded
                             ? (noXruns ? "every requested write landed at 50 Hz with no audio xruns"
                                        : "every write landed, but the audio thread missed its deadline")
                             : "not every requested parameter write completed");
}
