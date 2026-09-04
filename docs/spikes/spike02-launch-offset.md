# Spike 02 — Launcher start at an arbitrary in-file offset

## Verdict

**PASS**, and the §6.1 wording needs amending: the capability is not merely present, it is
**sub-sample accurate**. Offsets that land on a whole sample are placed **exactly**; one
that does not is placed within **0.16 of a sample** — about thirty times better than the
five-sample tolerance Tracktion holds itself to elsewhere.

The item's premise — *"if absent, this is the one genuine gap"* — is false. Nothing here
gates the polyphony model. What the spike found instead is a **cost**, and that cost is
what load-to-time (§3.13) has to be designed around.

## Criterion

PRD §6.1 item 2, verbatim:

> 2. **Launcher start at an arbitrary in-file offset** — load-to-time depends on it; if
>    absent, this is the one genuine gap.

## What was built

`spikes/spike02_launch_offset/main.cpp`.

Five tracks, each with the same source file in a launcher slot, each given a **different**
`Clip::setOffset`, each routed to its own stereo output bus, and **all launched at the same
`MonotonicBeat` in a single run**.

The source is a transient file: silence apart from one non-zero sample at 2.0 s, so its
position in the output is unambiguous to the sample.

That single-run design is not incidental. Spike #4 established that TE's launch *instant*
is not reproducible between runs, so measuring an absolute landing position would measure
that jitter rather than the offset. Launching every offset together makes the jitter
**common to all of them**, and it cancels in the differences:

```
measured(o) - measured(0)  ==  -o * sampleRate      (exactly)
```

The offset ladder is `0, 0.25, 0.1234567, 0.5, 0.75` seconds. The odd one earns its place:
at 48 kHz it is 5925.9216 samples and at 96 kHz 11851.8432 — not an integer sample count at
either rate, let alone a whole block or beat. An implementation that quietly rounds to a
block boundary passes a ladder of round numbers and fails that one. (1/3 s would *not* have
tested anything: at 48 kHz it is exactly 16000 samples.)

## How it was run

```
build:   Debug, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine v3.1.0 (runtime string), JUCE v8.0.6
command: spike02_launch_offset --tracks=5 --sample-rate=SR --buffer=B
device:  none — TE hosted audio device interface, no hardware opened
```

## Numbers

| offset (s) | error @ 48 kHz / 128 | error @ 96 kHz / 64 |
|---|---|---|
| 0.0 (reference) | 0 | 0 |
| 0.25 | **0** | **0** |
| 0.1234567 | **−0.0784** | **−0.1568** |
| 0.5 | **0** | **0** |
| 0.75 | **0** | **0** |

Errors are in samples. `max_abs_error_samples` = 0.0784 at 48 kHz, 0.1568 at 96 kHz.

| cost | value |
|---|---|
| `rebuilds.setOffset_live` | **5** |
| `rebuilds.nudge` | **0** |

## What was learned

**The offset is honoured exactly, and the residual error is not sample rounding.** Note the
ratio: 0.0784 samples at 48 kHz and 0.1568 at 96 kHz is *exactly* 2×, which means both are
the same **1.63 µs** of time error. A sample-domain rounding would have given the same
error in *samples* at both rates, not the same error in *seconds*. This is the sub-sample
residue of TE converting the offset into the beat domain (`Clip::getOffsetInBeats()`, which
is what `EditNodeBuilder` passes on the launcher path) and back. It is well under one
sample and it does not accumulate — the three round offsets are exact.

For Go.dot that means **load-to-time can place a cue at an arbitrary point in a file and
trust the result**. §6.1's "one genuine gap" is not a gap.

**The cost, which is the actual finding.** Setting an offset on a *live* graph rebuilt it —
`rebuilds.setOffset_live = 5`, and that 5 is one rebuild per output device, this spike
having five buses. `IDs::offset` is in `Edit::TreeWatcher`'s restart list
(`tracktion_Edit.cpp:147-150`), so this is by design and not a quirk of the measurement.
`LaunchHandle::nudge` by contrast cost **zero** rebuilds.

So there are two ways to re-point a clip, and they are not interchangeable:

| | rebuilds | when it is usable |
|---|---|---|
| `Clip::setOffset` | yes, one per output device | **before** the cue is armed — in prepare (§3.12), never mid-show |
| `LaunchHandle::nudge` | no | on live, already-playing material |

Spike #4 measured what a rebuild costs already-playing material, and the answer there was
"nothing measurable" — but that was a rebuild-free run. This is the case where a rebuild
actually happens, and prepare/commit is where it belongs: §3.12's "claim happens in
prepare" now has a second, mechanical reason to exist.

**Incidentally confirmed:** the per-output-device multiplication of the rebuild counter,
which `SpikeHarness.h` warns about, is real and was observed cleanly here (5 buses → 5).
Any spike reading that counter must use deltas, never absolutes.

## Consequences for the PRD

- **§6.1 item 2** — replace the premise. Proposed: *"Launcher start at an arbitrary in-file
  offset — present and sub-sample accurate (`Clip::setOffset`); the spike measures the
  graph-rebuild cost of setting it at runtime."*
- **§3.25, "Residual risks, stated"** — the sentence *"if the launcher cannot start a clip
  at an arbitrary in-file offset, load-to-time has a real gap"* should go. The residual risk
  is not the capability but its cost: setting an offset during playback rebuilds the graph,
  so load-to-time must set offsets **in prepare**, or use `nudge`.
- **§3.13, state solver / load-to-time** — worth stating the mechanism it is allowed to use:
  offsets set during prepare, `nudge` for anything already playing.
- **§3.12, prepare / commit** — a mechanical justification for the prepare step, beyond
  anticipation: it is the only place an offset can be set without rebuilding the graph.

## Open questions for the author

1. Loop-relative offset semantics are **not** covered by this spike. `ClipPosition`'s
   documentation (`tracktion_EditTime.h:121-128`) says the offset is relative to the loop
   start rather than the file start once a clip loops, which matters for §3.24's in-cue
   loops. It needs either a follow-up here or folding into spike #3.
2. Whether `nudge`'s beat-domain argument is precise enough for load-to-time's needs is a
   design question, not a measurement: it takes a `BeatDuration`, so its resolution depends
   on the tempo Go.dot fixes for the show (§3.25 says "set a fixed tempo and ignore it").
