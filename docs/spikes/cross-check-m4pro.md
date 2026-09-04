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

The first is **fixed**; the second is **diagnosed and left alone** for the author.

### 1. Spike 05 measured nothing on macOS, and reported PASS anyway — FIXED

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

**The fix, in two halves.**

*Make it tick.* `runDispatchLoopUntil()` would have worked — it pumps CFRunLoop rather than
`NSApp` — but it is behind `JUCE_MODAL_LOOPS_PERMITTED`, which this project sets to 0
deliberately (`cmake/WfgThirdParty.cmake:174`, "a modal loop in a show engine is a hang"), and
that decision is not a spike's to reverse. So on macOS the spike now pumps the same CFRunLoop
in the same mode, which is not a substitute for the JUCE message loop but *is* the JUCE
message loop on this platform: JUCE registers its message queue as a CFRunLoop source in
`kCFRunLoopCommonModes`, so `juce::Timer` callbacks arrive through exactly that pump. Windows
and Linux are untouched.

*Make silence fail.* A run with `ticks_fired == 0` now exits 3 (HARNESS-ERROR), not 0. The
harness already draws that distinction — a spike that could not tick has said nothing about
the engine, which is not the same as an engine that misbehaved — and `run-spikes.sh` states
the identical principle one level up for the case where it finds no executables.

**Spike 05's open question is now answered**, and the numbers are in
[spike05-param-50hz.md](spike05-param-50hz.md): 512 parameters cost **14.5%** of a 20 ms tick
on this machine against 12.9% on Windows, at ~5.7 µs per write against ~4 µs. The conclusion
holds with slightly less margin.

It also retires a claim. Spike 05's report called the lateness floor "Windows, not us"; macOS
shows the same floor — 0.61 ms p50 and 3.03 ms p99 at 8 parameters — so it is `juce::Timer`
granularity in general, not a platform property. That correction is now in the report.

**One consequence to expect.** Now that it runs, this spike fails at buffers 32 and 64, and at
96 kHz more broadly, on its `xruns == 0` invariant — usually by a single block in a 5-second
run. Across the full grid that is 35 of 64 runs, in both build types. The gate was left
strict: those are extremely tight buffers and straining there is the expected result, the same
region spike #4 records this machine struggling in. It is a statement about those buffer
sizes, not about parameter control, and never once about writes failing to land.

### 2. Tracktion's node IDs are not really hashed — DIAGNOSED, not fixed

24 occurrences in the Debug sweep, all in spike05, all identical:

```
JUCE Assertion failure in tracktion_NodePlayerUtilities.h:122
```

Line 122 is `jassert (areNodeIDsUnique (nodeGraph->orderedNodes, true))`. It is a `jassert`, so
Release compiles it out; that is why the Release sweep is silent.

**Why Windows could never have shown it.** JUCE routes assertions through
`Logger::outputDebugString`, which is `OutputDebugString()` on Windows — visible only to an
attached debugger, invisible when running the sweep from a terminal — and `fputs(…, stderr)`
on macOS. It was almost certainly firing on Windows all along with nobody in a position to see
it.

**What actually collides.** Tracktion ships its own diagnostic for this (the `JUCE_DEBUG`
block at `tracktion_NodePlayerUtilities.h:52`), which prints the offending IDs and node types.
Exactly **one** ID collides, and always between **two `ArrangerLauncherSwitchingNode`
instances** — one per audio track. It is fully deterministic (five identical runs), unrelated
to `--validate-instrument`, and unique to spike05: spike01 and spike04 at the same track
counts collide zero times.

**Why, and it is not a rare accident.** `ArrangerLauncherSwitchingNode::getNodeProperties()`
computes `nodeID = hash (seed, track->itemID)` and then folds in each input
(`tracktion_ArrangerLauncherSwitchingNode.cpp:41` and `:55`). Follow the two pieces:

- `std::hash<EditItemID>` is the **identity function** — `return (size_t) e.getRawID();`
  (`tracktion_EditItem.h:168`). The raw track ID enters unmixed.
- `hash_combine` is `seed ^= std::hash<T>()(v) + 0x9e3779b9 + (seed * 65537u) + (seed / 3u);`
  (`tracktion_Hash.h:58`). With a constant seed the tail is a constant, so the whole thing
  collapses to roughly `seed ⊕ (rawTrackID + C)`.

The observed IDs say the same thing out loud — at 6, 7 and 8 tracks they are
`…447948628`, `…447948635` and `…447948622`, identical in their first seventeen digits. These
are near-sequential clustered integers, not spread hashes. So a collision between two tracks
is an arithmetic near-coincidence rather than a 1-in-2⁶⁴ event, which is exactly why it is
deterministic and why it depends on track count in a way that looks arbitrary: 6, 7, 8 and 32
collide while 4, 9, 16 and 24 do not.

**Why it is worth caring about, beyond tidiness.** The same file uses that ID to find a node's
previous incarnation across a graph rebuild — `findNodeWithID<ArrangerLauncherSwitchingNode>
(*oldGraph, props.nodeID)` at `:88`. If two tracks share an ID, a rebuild can match the wrong
one. Spike #4's entire finding is that rebuilds must not disturb already-playing material, so
this is adjacent to something the polyphony model depends on. Nothing observed here shows it
actually happening — spike #4 reported zero rebuilds and a bit-identical witness in all 64
runs — but the mechanism is there.

This looks like an upstream Tracktion issue rather than a Go.dot one, and it is left
unfixed deliberately. Reproduce with:

```
cmake --build --preset spikes-debug
./build/spikes/spikes/Debug/spike05_param_50hz --tracks=8 --sample-rate=48000 \
    --buffer=128 --seconds=1 --validate-instrument 2>&1 >/dev/null | grep -B1 Assertion
```

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
