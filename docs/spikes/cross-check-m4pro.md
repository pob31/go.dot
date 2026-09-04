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

**Why, and it is not a rare accident.** `hash_combine` is
`seed ^= std::hash<T>()(v) + 0x9e3779b9 + (seed * 65537u) + (seed / 3u);`
(`tracktion_Hash.h:58`). With the seed fixed that reduces to `seed ⊕ (h(v) + K)` — affine in
the value rather than a hash of it. On a bit-flip test about 2 of 64 output bits change per
flipped bit of the *value* (ideal 32) against about 24 per flipped bit of the *seed*, so the
precise defect is that **the value argument is barely mixed**, not that the function fails to
avalanche generally.

Two things it is *not*, both of which an earlier draft of this section got wrong.
`std::hash<EditItemID>` being the identity (`tracktion_EditItem.h:168`) is a co-cause, not a
defect — libstdc++ and libc++ both do this for integral types, and a combine is expected to do
the mixing itself. And clustering alone is not the mechanism: the base step
`hash (7653239033668669842, track->itemID)` is **injective**, so bases cluster (our eight tracks
span 48) but can never collide by themselves.

The collision is manufactured one fold later, at
`tracktion_ArrangerLauncherSwitchingNode.cpp:55`, where the child `SummingNode` ID is folded in.
A difference in a value survives the next combine multiplied by roughly 65537 and can be
annihilated by an opposite difference arriving from the child chain. That reconstructs exactly,
and unlike the sweep it follows from the formula alone — inverting the fold gives the child IDs
needed to reach the observed value, and both check out:

```
hash_combine (4306145609409080913, 173965249108218) ==   # track 1010
hash_combine (4306145609409080925, 173965248321758) ==   # track 1022
                                    16973511083447948622
```

**Why it is worth caring about, beyond tidiness — and it is worse than "the wrong node".**
`prepareToPlay` uses that ID to find a node's previous incarnation across a graph rebuild
(`findNodeWithID<ArrangerLauncherSwitchingNode> (*oldGraph, props.nodeID)` at `:88`). With two
same-type duplicates, **both** new nodes resolve to the *same* old node, and the adoption at
`:90-105` is `shared_ptr` assignment rather than copy. The two live nodes therefore end up
sharing one `SampleFader`, one `ActiveNoteList` and one `activeNode` atomic — re-established at
every subsequent rebuild. The faders are guarded by a channel-count check; `arrangerActiveNoteList`,
`activeNode` and `midiSourceID` are adopted with no guard at all. Two ALSNs on different tracks
have no dependency edge, so they can be scheduled on different threads in the same block.

`tracktion_WaveNode.cpp:1727` already defends against exactly this, which suggests the hazard
was recognised once: `if (other.editItemID != editItemID) return;` — an identity re-check after
the ID lookup. `ArrangerLauncherSwitchingNode` has no equivalent, nor does
`tracktion_LoopingMidiNode.cpp:1469`; `tracktion_PluginNode.cpp:332` guards on shape, not identity.

ALSN is where this was caught, not the population: the assertion covers every node in
`orderedNodes`, and the same combine feeds `SummingNode`, `ConnectedNode`, `InsertSendNode` and
`LatencyNode`. Nothing observed here shows harm actually occurring — spike #4 reported zero
rebuilds and a bit-identical witness in all 64 runs, so this rig never exercises the rebuild
path — but the mechanism is there.

**What the sweep does and does not say.** "24 of 63 track counts" is an *assertion* rate, not a
harm rate: `areNodeIDsUnique` fires on duplicates across all node types while `findNodeWithID`
is `dynamic_cast`-filtered, so cross-type duplicates are inert. It is also Debug-only — in
Release the collisions happen silently — and only the instrumented runs were confirmed down to
the colliding pair. The rig allocates IDs on a regular lattice, which sits on a resonance
between the track-ID and clip-ID strides; jittering the strides in a model drops the rate to a
few per cent rather than to zero.

**Not reported upstream, and not a regression.** A search of Tracktion's issues and PRs finds
nothing for the collision, the assertion, or the hash. The one issue in the same area is
[#367](https://github.com/Tracktion/tracktion_engine/issues/367), *"Launcher clips click on
every playback-graph rebuild — node state transfer never engages inside SlotControlNode"*, and
it is a **different root cause**: there the child nodes were *excluded* from the flat
`sortedNodes` list, so `findNodeWithID` found nothing; here they are present but ambiguous.
Its fix (`d760ce8c1bd`, exposing `SlotControlNode` children as internal nodes) **is** in our
pin. Since that fix changes what `getInternalNodes()` returns, and
`ArrangerLauncherSwitchingNode` folds those IDs into its own, it was a plausible culprit —
so it was checked by building against `d760ce8c1bd~1`: the collision is present there too, at
the same track counts and the same count of three. It predates that fix.

Worth noting alongside it: #367 is about damage done *when a rebuild occurs*, and spike #4
recorded zero rebuilds in all 64 runs — so nothing here has exercised that path. The two sit
next to each other in the same mechanism, and both bear on §6.1 #4's guarantee.

**Reported, not patched — and why not patching was the right call.** Tracktion do not accept
third-party pull requests ("due to copyright restrictions"); they ask for the JUCE Forum, so
this goes there as a report — drafted in
[upstream-node-id-collision.md](upstream-node-id-collision.md). A candidate fix *was* built and measured — replacing the mixing
step with a splitmix64 finaliser took the sweep from 24 of 63 track counts to **0 of 63**, left
all seven spikes unchanged (spike #4 still bit-identical, zero rebuilds), and left Tracktion's
own test suite at exactly 341/343 with the *same* two pre-existing `tracktion_ClipLauncher`
failures before and after. It was still wrong to propose, for two reasons found by review:

- **It reintroduces the same bug class elsewhere.** A finaliser applied to `seed + K + h(v)`
  depends on `seed` and `h(v)` only through their sum, so `hash (a, b) == hash (a + k, b - k)`
  whenever `std::hash` is the identity. `tracktion_MidiInputDeviceNode.h:41` is
  `hash ((size_t) midiSourceID, targetID)` with both operands small integers — those would
  collide deterministically. The `seed * 65537u` term that was removed is what separates them
  today.
- **Hash values are persisted, in one place that is not a cache.**
  `PatternGenerator::hashNotes (seq, 2)` is built on `core::hash`
  (`tracktion_Musicality.cpp:2183`) and stored in the Edit as `IDs::hash` (`:825`);
  `getAutoUpdate()` (`:2205`) compares stored against recomputed. Change the mixer and they
  never match, so chord/arp/bass/melody clips in previously-saved Edits silently stop
  auto-regenerating. That is a document-compatibility decision, and it is Tracktion's to make.

Cached renders and proxies regenerating once (`ContainerClip::getHash()` and the `WaveNode`
proxy/time-stretch keys) is the milder half, and those values were never portable anyway — the
hard-coded seeds are `std::hash<std::string_view>` outputs, so toolchain-specific.

Reproduce the collision with:

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
