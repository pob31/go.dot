# Cross-check — Mac mini M4 Pro

The spikes README asked for the machine-dependent half of the Phase 0 results to be re-run on
a Mac mini M4 Pro, on the grounds that a hybrid-core mobile CPU under scheduler and thermal
pressure was the most likely cause of the intermittent non-reproducibility seen at 96 kHz.
This is that run, and it answers the question: **the non-reproducibility is the machine.**

## What was run

| | |
|---|---|
| machine | Mac mini M4 Pro (`Mac16,11`), 12 cores — 8 performance + 4 efficiency, 24 GB |
| OS | macOS 15.7.9 (24G830), ordinary desktop background load |
| compiler | AppleClang 17.0.0.17000604 |
| engine | Tracktion `develop` 3.5.0 (`v3.2.0-404-gb88a6ee5191`), JUCE 8.0.13 (`8.0.13-7-g37c894f83d`) |
| commit | `9cef82e` |
| grid | the `--full` sweep: 7 spikes × {48k, 96k} × {32, 64, 128, 256} × {8, 16, 32, 64} |

**Both Debug and Release**, 224 runs each:

```
Release   224 run(s), exit 0     7:45      224/224 PASS
Debug     224 run(s), exit 0     1:05:18   224/224 PASS
```

Debug matters. The Windows baseline for spikes #1, #3 and #4 was a **Debug** build —
spike01's report lists "It is a **Debug** build" among the things its ceiling does not
establish. Quoting a Release Mac run against a Debug Windows run would have moved two
variables at once and proved nothing about hardware. The Debug sweep is the like-for-like
comparison; the Release sweep is this machine's quotable numbers.

## The headline: all three machine-dependent findings disappear

Every Windows result that the README flagged as machine-dependent fails to reproduce here —
**in Debug, the same build configuration that produced them.**

| | Windows / Core Ultra 7 255H | M4 Pro, Debug | M4 Pro, Release |
|---|---|---|---|
| **#1** 64 stereo tracks (128 ch) | **1 correct** — total failure | **64/64 correct**, all 8 rate×buffer points | **64/64 correct** |
| **#1** mixed 64-ch @96k | **1/48** | 64/64 | 64/64 |
| **#3** control run @48k/64 | **failed, no verdict** — "the two control runs differ" | `control_max_abs_diff=0`, all 32 | `=0`, all 32 |
| **#4** reproducibility @96k | **"not reproducible, three runs out of three"** | **bit-identical, all 32 configs** | **bit-identical, all 32** |

`rebuilds.delta = 0` at every one of the 64 spike04 runs, as on Windows.

For #4 this is unusually clean evidence: `control_max_abs_diff_dbfs` and
`witness_max_abs_diff_dbfs` both report `-999` — the harness's bit-identical sentinel
([spike04 main.cpp:304](../../spikes/spike04_graph_stability/main.cpp)) — at **all 32
configurations in both build types**, including the two 96 kHz points Windows could not
reproduce three times out of three.

**What this establishes, and what it does not.** It establishes that the instability is not a
property of Tracktion Engine, which is what the polyphony model needed to know. It does not
establish a new ceiling: the track ladder stops at 64, and 64 was clean everywhere, so this
run found no limit at all rather than a higher one. Finding the M4 Pro's actual ceiling needs
a ladder that goes past 64.

## Portable findings: unchanged, as predicted

| | result |
|---|---|
| **#2** offsets | `rebuilds.setOffset_live=5`, `rebuilds.nudge=0`; error 0 at integer offsets, 0.0784 (48k) / 0.1568 (96k) for 0.1234567 s — identical to baseline |
| **#3** join | `join_error_samples=0` in all 64 runs; artefact 32 / 40 / 168 samples at buffers 32 / 128 / 256 — matches the baseline model exactly |
| **#6** PDC | `pdc_disable_works=1` and `nopdc.file_path_shift_samples=0` in all 64 runs; default shift 250 ms; `reported_latency` 12000 @48k, 24000 @96k; mono→stereo bus 0.5 / 0.5 |

Spike #6 produced byte-identical conclusions on both platforms, which is what a spike with no
timing figures in it should do.

## What moved

**#7 proxy plugin — slower per call, better behaved in the tail.**

| | Windows | M4 Pro (Release) |
|---|---|---|
| round trip p50 | **0.9 µs** | **1.8 – 5.2 µs** |
| round trip p99 | 12.1 µs | 10.1 – 16.4 µs |
| round trip max | **236.5 µs** | **23 – 97 µs** |
| misses (child alive, 500 µs deadline) | 0 / 2250 | 0 in 31 of 32 configs |

The p50 cross-process round trip is consistently **2–3× slower** on macOS, while the worst
case is **2–10× better**. For §3.18 that is a favourable trade — the sandbox's risk was
always the tail, not the median — but the median is the number that sets how much headroom
the deadline buys, so the "250 µs was clean on this machine" figure should be re-derived here
rather than carried over. One config (48k/64/16) recorded a single miss with a 501 µs round
trip and a 1 µs overrun.

**Throughput headroom.** Windows Release peaked at `block_us.max = 31 611` at the 96 kHz /
32-track point; the same point here is **2 048 µs**, about 15× lower.

**Xruns are concentrated at buffer 32**, and Debug at 96 kHz is where the machine finally
strains: 96k/32/64 in Debug recorded **11 931 xruns** and an 11 333 µs worst block. In
Release the same point recorded 43. Nothing here changed a verdict — spike04 gates on
rebuilds and the witness, not on xruns — but it marks where this machine's edge is.

## Two findings the Mac surfaced that are not about the Mac

### 1. Spike 05 measures nothing on macOS, and reports PASS anyway

All **64** spike05 runs — 32 Debug, 32 Release — report:

```
writes_requested=0    writes_done=0    ticks_measured=0    VERDICT: PASS
```

A run asked for 5 seconds of 50 Hz ticking returned in **0.46 s** having written nothing.

The cause is in the spike, not the engine. It drives its tick from
`juce::MessageManager::runDispatchLoop()`, which on macOS is `[NSApp run]`
([juce_MessageManager_mac.mm:335](../../ThirdParty/JUCE/modules/juce_events/native/juce_MessageManager_mac.mm)).
In a console binary with no `NSApplication`, `NSApp` is nil, the message goes nowhere, and the
call returns immediately. On Windows the same call is a real message pump, which is why the
spike worked there.

It reports PASS because its gate is `writes_done == writes_requested && xruns == 0`, and
`0 == 0 && 0 == 0` is true. `run-spikes.sh` already states this exact principle one level up —
*"A run that measures nothing is not a pass"*, guarding the case where no executable is found —
but the same hole exists inside the spike.

Two consequences worth recording:

- Spike 05's own open question — *"Does the same margin hold on macOS?"* — is **still
  unanswered**. The README's "512 params = 13% of a 20 ms tick, ~4 µs per write" is a
  Windows-only figure and should be labelled as one until the spike can run here.
- The fix has two halves: gate on `ticks_measured > 0` so a silent run fails, and drive the
  loop with `runDispatchLoopUntil()` (or initialise an `NSApplication`) so it has something to
  measure.

### 2. A Tracktion graph assertion that Windows could never have shown

24 occurrences in the Debug sweep, all in spike05, all identical:

```
JUCE Assertion failure in tracktion_NodePlayerUtilities.h:122
```

Line 122 is `jassert (areNodeIDsUnique (nodeGraph->orderedNodes, true))` — the graph spike05
builds contains **duplicate node IDs**. It is a `jassert`, so Release compiles it out; that is
why the Release sweep is silent.

This is almost certainly **not** a macOS bug. JUCE routes assertions through
`Logger::outputDebugString`, which is `OutputDebugString()` on Windows — visible only to an
attached debugger, invisible when running the sweep from a terminal — and `fputs(…, stderr)`
on macOS. The assertion was likely firing on the Windows runs the whole time with nobody in a
position to see it. Non-unique node IDs are worth a look on their own terms, independently of
which platform revealed them.

## What this run deliberately did not do

- **Audio workgroups were left at Tracktion's default (off).** The README suggests the Mac
  cross-check should enable them rather than measure the default. Measuring the default first
  is what makes this run comparable to the Windows baseline line for line — and it turns out
  to matter, because reproducibility was already perfect without them. The workgroup has no
  reproducibility problem left to fix here; `EditPlaybackContext::enableAudioWorkgroup(true)`
  is now a question about xrun headroom at buffer 32, which is the one place this machine
  strains.
- **Still the hosted device interface**, not a real driver. The RME path is untested, and
  spike06's `--use-rack` live-input half needs hardware.
- **Every clip still launches at the same instant** — a worst case, not a show.
- The sweep runs spike07 with `kill_at_block=0`, so these numbers are the child-alive path;
  the kill path comes from a dedicated invocation.
- **MTC chasing and multiple summed Edits remain unrun**, as on Windows.
