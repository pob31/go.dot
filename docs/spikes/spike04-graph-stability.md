# Spike 04 — Graph stability under sustained launching

## Verdict

**PASS** on the half that gates the polyphony model: sustained launching into a fixed
track set causes **no audio-graph rebuild at all**, at every track count, sample rate and
buffer size measured, in both Debug and Release. The second half — no crossfade tax on
already-playing material — is **PASS at 48 kHz** and **undetermined at 96 kHz**, where the
experiment stops being reproducible for reasons that have nothing to do with launching.
The spike reports that as `HARNESS-ERROR`, not as a failure, and the reasoning is recorded
below rather than smoothed over.

PRD §9.2 says *"The polyphony model stands unless #2 or #4 fails."* Spike #2's premise
turned out to be false in the other direction (the launcher **does** honour an arbitrary
in-file offset — see `spike02-launch-offset.md` when written), so this spike is the whole
gate. **The §3.25 model stands.**

## Criterion

PRD §6.1 item 4, verbatim:

> 4. **Graph stability under sustained launching with a fixed track set** — no rebuild, no
>    crossfade tax on already-playing material.

## What was built

`spikes/spike04_graph_stability/main.cpp`, on `spikes/SpikeHarness.h`.

- A fixed track set of *N* tracks, built once and never structurally edited afterwards.
- **Track 0 is a witness**: one sustained full-scale sine on the *timeline*, playing
  continuously, never touched again, routed to its own stereo output bus (channels 0–1).
- **Tracks 1..N-1 are the storm**: `--slots` launcher slots each, holding short clips,
  launched `--launches` times spread evenly across the run, routed to a second bus (2–3).
- Rebuilds counted through `EditNodeBuilder::insertOptionalLastStageNode`, a public static
  hook TE calls on every graph build. Baseline marked after the witness has settled;
  only the delta is reported.
- The whole thing runs on TE's `HostedAudioDeviceInterface` via `EnginePlayer`, so it
  opens **no audio hardware** and advances the graph exactly `blockSize` samples per call.

Each configuration runs the experiment **three** times: storm, quiet, and a **second
storm** as the control. See "the instrument" below for why the third run exists and why it
is a storm rather than a quiet one.

## How it was run

```
build:   Debug and Release, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine v3.1.0 (the string v3.2.0 reports at runtime), JUCE v8.0.13
command: spike04_graph_stability --tracks=N --sample-rate=SR --buffer=B [--validate-instrument]
device:  none — TE hosted audio device interface, no hardware opened
```

## Numbers

`rebuilds.delta` is the gating observable. Debug unless stated.

| tracks | sample rate | buffer | `rebuilds.delta` | `control_max_abs_diff` | `witness_max_abs_diff` | verdict |
|---|---|---|---|---|---|---|
| 8  | 48 000 | 32  | **0** | −inf | −inf | PASS |
| 16 | 48 000 | 64  | **0** | −inf | −inf | PASS |
| 64 | 48 000 | 256 | **0** | −inf | −inf | PASS |
| 32 | 96 000 | 128 | **0** | not reproducible | — | undetermined |
| 8  | 96 000 | 64  | **0** | not reproducible | — | undetermined |

`−inf` means **bit-identical** — not "below a threshold", but not one differing sample.

Supporting observations, all configurations:

- `storm_peak_ch0 = 0 dBFS`, `storm_peak_ch2 ≈ 35 dBFS`, `quiet_peak_ch2 = −inf` — the
  witness bus carries only the witness and the storm bus carries only the storm, so the
  comparison is measuring what it claims to.
- `instrument_produces_nonzero = 1` under `--validate-instrument`.
- `xruns = 0` at 128 and 256 frames in Debug; non-zero at 32 frames, and non-zero at the
  96 kHz / 32-track point even in Release (`block_us.max = 31 611`, a single long stall
  consistent with the streaming explanation above).

## What was learned

**The model holds, and it holds strongly.** `rebuilds.delta` was **0 in every single run**,
including 64 tracks with 64 launches. PRD §3.25's "Launching a clip into an existing slot
does not change the playback graph" is not merely true, it is true with no observed
exception across a 4×4×2 grid. Phase 2 can be designed on it.

**The rebuild counter is trustworthy, and was made to prove it.** A zero from an instrument
that cannot produce a non-zero is not evidence. `--validate-instrument` deliberately sets
`Clip::setOffset`, which is in `Edit::TreeWatcher`'s restart list
(`tracktion_Edit.cpp:147-150`), and confirms the counter moves. It does.

**Three things about TE that Phase 1 and 2 need, none of them in the documentation:**

1. `LaunchHandle::play({})` is **not** an "immediately" shorthand. The empty optional
   reaches code that dereferences it — in Debug,
   `optional(429): operator->() called on empty optional`; in Release, undefined. A caller
   must supply a `MonotonicBeat` obtained from the playback context's sync point, which
   exists only once the transport is rolling and a block has been processed.
2. **Launch timing is not reproducible run-to-run.** The sync point is influenced by
   transport start rather than purely by processed blocks, so two identical runs in one
   process start a launched clip a few samples apart. For sustained material that is a
   phase shift. Quantising to a whole beat narrows the window; it does not close it. This
   matters directly for PRD §3.4's "launches quantise to the tick" and for §3.13's
   load-to-time: **Go.dot cannot rely on TE's launch instant being deterministic** and must
   own that timing itself.
3. Changing a track's output device is a structural edit and *does* rebuild the graph. The
   fixed-track-set discipline therefore extends to routing, not just to track count.

**The instrument, and the one caveat.** The witness metric was wrong twice before it was
right, and both failures produced confident, plausible, false results:

- *First*, witness and storm shared one output bus, so the comparison measured "the storm
  is audible" and reported a 15.9 dBFS FAIL. Fixed by giving each its own bus.
- *Second*, the witness was itself a launched clip, so its start jittered per finding 2
  above and two identical runs differed by ~6 dBFS — the maximum possible difference
  between two full-scale sines, and indistinguishable from a real crossfade tax. Fixed by
  putting the witness on the timeline, where its start is fixed by construction.

The **control run** is what caught the second one, and it is why the spike runs three times
instead of two. It also had to be fixed once itself: the control was originally *quiet vs
quiet*, which answers "is this reproducible?" for the wrong run. The storm run is the one
under load, and load is what makes TE's file streaming non-reproducible — so a quiet
control came back clean at 96 kHz / 64 frames while the storm run was not, and the spike
reported another confident FAIL. The control is now **storm vs storm**, so both comparisons
share the same load and anything left over is attributable to launching.

**At 96 kHz the experiment stops being reproducible, and the spike says so.** With the
storm-vs-storm control in place, 96 kHz configurations report `HARNESS-ERROR` (exit 3)
rather than a verdict — repeatably, three runs out of three. 48 kHz configurations are
bit-identical across all three runs at every track count measured.

My first reading was that Debug's `-O0` was starving the file reader. A Release run
disproved that: the nondeterminism survives optimisation. The cause is that `EnginePlayer`
is deterministic in *graph processing* — it advances exactly `blockSize` samples per call —
but TE's audio-file streaming runs on **background threads**, so how much data is ready
when a block is processed depends on real time once the throughput demand is high enough.
Doubling the sample rate doubles that demand.

Two consequences worth carrying forward, neither of them a verdict on §6.1 #4:

- **The crossfade-tax half is confirmed at 48 kHz and undetermined at 96 kHz.** Not failed:
  undetermined. Closing it needs the file reader pinned or the material pre-loaded.
- **Deterministic replay (devplan Phase 12) cannot be built on `EnginePlayer` alone.**
  Block-accurate processing does not make a render reproducible while file I/O is async.

What this does **not** cast doubt on is the gating observable. `rebuilds.delta` was 0 in
that configuration too, in both Debug and Release, and the rebuild counter does not depend
on audio content at all.

## The machine these numbers came from

Every measurement in this report was taken on:

```
Lenovo 21Q8CTO1WW laptop
Intel Core Ultra 7 255H - 16 cores, HYBRID (performance + efficiency cores)
31.5 GB RAM, Windows 11
Background load at time of measurement: ~18%, with Firefox and two VS Code windows resident
```

**This matters for anything that depends on sustained throughput or timing reproducibility.**
A hybrid-core mobile CPU migrates threads between performance and efficiency cores under
scheduler and thermal pressure, and a measurement thread moved to an E-core mid-run produces
exactly the kind of *intermittent* non-reproducibility seen here. It also explains why
real-time pacing did not help: pacing controls when work is submitted, not which core runs it.

So the throughput and reproducibility limits below are **properties of this machine under this
load**, not established properties of Tracktion Engine. They are recorded because they are what
was measured, and flagged because they are the findings most likely to move on different
hardware. Sample-accuracy, routing correctness and API behaviour do not depend on any of this
and are not affected.

A cross-check on a Mac mini M4 Pro - desktop thermals, different scheduler, and the macOS
platform that CI currently only *builds* on - is the cheapest way to separate the two.

## Consequences for the PRD

- **§3.25, "Cues are launcher clips in pre-allocated slots on a fixed track set"** —
  confirmed. Worth adding *how* it is provable, since Phase 2 will be designed against it:
  the rebuild hook, `Edit::TreeWatcher`'s trigger list, and
  `TransportControl::ReallocationInhibitor`.
- **§3.25, one Edit / one transport** — no amendment needed from this spike, and nothing
  here bears on §6.1's "multiple active Edits summed by the DeviceManager". An earlier
  version of this spike advertised a `--edits=K` flag for that item; it was parsed, echoed
  into the report, and honoured by no code at all, so a sweep would have recorded
  `edits=8` against a single-Edit run. The flag has been removed. The item is untouched
  and fully open.
- **§3.4 and §3.13** — add finding 2 above. TE's launch instant is not deterministic, so
  the tick-quantised GO that §3.4 describes has to be enforced on Go.dot's side.
- **§3.9c** — the fixed-track-set discipline covers **output routing** too, not only the
  track count. Worth one sentence where the allocator is described.

## Open questions for the author

1. **Default fixed track count** (devplan:49) — still open, deliberately. This spike found
   no ceiling: 64 tracks behaved exactly as 8 did, with zero rebuilds. The number is a
   product decision about polyphony, not something these measurements settle.
2. The 96 kHz nondeterminism is a harness limit, not an engine finding, but it points at a
   Phase 12 question worth raising early: deterministic replay will need the file reader
   pinned or pre-loaded, because block-accurate processing alone does not make a render
   reproducible while file I/O is asynchronous. Worth deciding whether closing that is
   Phase 0 work or Phase 12's.
3. §6.1's unnumbered "multiple active Edits summed by the DeviceManager" item is **not**
   addressed here — see the note above about the removed flag. Whether it folds into a
   spike or gets its own is your call per `spikes/CMakeLists.txt:68-73`.
