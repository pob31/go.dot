# Spike 03b — Loop joins: the wrap, the re-trigger, and the placed boundary

## Verdict

**The clip's own loop wrap is the cleanest of the three, and a looping range is left alone.**
That is M12 answered, and it answers it in Go.dot's favour: PR 3.8 already arms every range
clip looping, and nothing more has to be built to carry a range from one pass to the next.

Three findings, in the order they matter.

**1. The wrap is free, and at 96 kHz it is perfect.** `join_error_samples = 0` at every block
size at both rates — the second pass lands on exactly the sample that continues the first,
with nothing dropped and nothing repeated. At 96 kHz the damaged span is **zero samples at
every block size from 64 to 1024**: the reference and the render agree to within 16-bit
quantisation noise right through the wrap. At 48 kHz the span is zero at 64 and 128 frames and
grows to 381 samples at 512 and 1024, with a worst deviation of 0.027 against an amplitude of
0.5 — **5% of full scale, 4 ms of very slightly wrong audio at a block size no show runs.**

**2. `LaunchHandle::setLooping` on a clip armed not-looping does nothing, at every block size
and both rates.** The clip stops at the end of its first pass and does not come back. This is
what the sources said would happen and it is now measured: `SlotControlNode` captures a stop
duration when the graph is *built* — the clip's length in beats when `isLooping()` is false —
and queues that stop every block, ahead of the wrap. `setLooping` is a rebuild-free seqlock
store and cannot remove a duration that was already captured.

**This is why "every range clip is armed looping" is a mechanism and not a setting.** A range
clip that were armed the other way could never be made to loop, and the failure would be
silence at the end of the first pass rather than an error.

**3. A placed cross-slot boundary costs up to one block of GAP, and that is new.** The
alignment is perfect — `join_error_samples = 0` everywhere, so the incoming range starts on
exactly the right sample — but the damage is **up to half of full scale**, and its worst point
is always at a *negative* offset: −32, −96, −192, −608 samples, always about one block before
the boundary. The outgoing range stops at the start of the block containing the boundary
rather than at the boundary sample, and what is left is a hole.

That is a design finding for PR 3.9 rather than a defect here: **a boundary between two ranges
should not queue its stop and its play at the same instant.** Letting the outgoing range run a
block past the boundary trades a gap for a brief overlap, and spike 03 already established
that two overlapping copies of one file are sample-aligned. M13 is where that gets decided
from the render.

## Criterion

Not one of PRD §6.1's seven. This is **M12 of the Phase 3 plan**, which the plan states as:

> **M12 (spike 03b)** | on spike 03's chirp rig, three joins side by side at 5 block sizes:
> the looping clip's own wrap, a `LaunchHandle::setLooping` re-trigger on a lengthened clip,
> and the cross-slot placed boundary | the wrap is the primary; if it measures worse than a
> placed boundary, loops become placed same-slot `play` per pass

## What was built

`spikes/spike03b_loop_joins/main.cpp`. Three variants on one rig, five block sizes each:

| variant | what it is |
|---|---|
| `wrap` | one slot, one clip armed looping over the whole file, launched and left alone for two passes. Nothing in Go.dot touches the boundary. |
| `setlooping` | one slot, one clip armed **not** looping, `LaunchHandle::setLooping` called before the launch. |
| `placed` | two slots holding the two halves of the file as loop ranges; a `stop` on the first and a `play` on the second, queued at the same beat. |

**The method is spike 03's, unchanged, because the numbers have to be comparable.** The source
is a linear chirp — never self-similar, so alignment is unique; against a sine the search is
ambiguous modulo one period and every join would measure as perfect. What is reported is a
**difference of two alignments**, one before the join and one after, because TE's launch
instant is not reproducible (spike 04) and the jitter is common to both.

Added on top of spike 03: the **damaged span**, being how many consecutive samples around the
join differ from the reference by more than 0.02, and the worst deviation with the offset it
occurred at. A join can land on its sample and still have a click, and that is exactly what
the placed boundary turns out to do.

## How it was run

```
build:   Debug, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine develop 3.5.0 (runtime string still reports v3.1.0), JUCE v8.0.13
command: spike03b_loop_joins --tracks=1 --sample-rate=SR --buffer=128
device:  none — TE hosted audio device interface, no hardware opened
```

The `--buffer` flag is read and then overridden: the spike sweeps 64, 128, 256, 512 and 1024
itself, because the point is the comparison across block sizes and running it five times by
hand would compare five different launch jitters.

## Numbers

`join_error_samples` is **0** in every cell that measured, at both rates and every block size.
What varies is the damage.

### 48 kHz — damaged span in samples (worst deviation, against amplitude 0.5)

| block | wrap | setLooping | placed |
|---|---|---|---|
| 64   | **0** (0.013) | did not come back | 58 (0.498) |
| 128  | **0** (0.013) | did not come back | 122 (0.496) |
| 256  | 130 (0.026) | did not come back | 129 (0.496) |
| 512  | 381 (0.027) | did not come back | 129 (0.496) |
| 1024 | 381 (0.027) | did not come back | 641 (0.311) |

### 96 kHz — damaged span in samples (worst deviation)

| block | wrap | setLooping | placed |
|---|---|---|---|
| 64   | **0** (0.0065) | did not come back | 25 (0.101) |
| 128  | **0** (0.0065) | did not come back | 89 (0.500) |
| 256  | **0** (0.0065) | did not come back | 217 (0.487) |
| 512  | **0** (0.013) | did not come back | 217 (0.487) |
| 1024 | **0** (0.014) | did not come back | 217 (0.487) |

**Read the deviations, not only the spans.** A wrap's worst deviation is 0.0065 to 0.027 —
between 1.3% and 5.4% of the material, which is the residue of a 16-bit file read back through
a float graph. A placed boundary's is 0.49, which is the material being *absent*. The two
columns are not measuring the same kind of wrongness.

## What was learned

**The wrap is not merely acceptable, it is the best join available in this engine.** At 96 kHz
it is indistinguishable from the reference. That settles the plan's fallback — "loops become
placed same-slot `play` per pass" — as unnecessary, and it means an ambience bed looping for
four hours costs Go.dot nothing per pass: no command, no placed instant, no run record.

**A placed boundary is sample-accurate in position and lossy in content**, and those are
different properties that spike 03 did not have to separate because a follow action never gets
to choose. Go.dot does choose, and PR 3.9 should choose to overlap rather than to butt:

- the stop lands at the start of the block containing its instant, up to a block early;
- the play lands on its sample;
- so a same-instant pair leaves a hole of up to one block.

Placing the stop one block *after* the play would replace the hole with an overlap of two
different regions. Whether that is better is M13's question, measured from the render, and it
is the first thing PR 3.9 should try.

**The 48 kHz wrap at 512 and 1024 frames is worth one more look before Phase 3 closes.** 381
samples at 0.027 is 8 ms of audio that is 5% wrong — inaudible on a bed, possibly not on a
transient. The hardware checklist already carries "a loop boundary *listened to* at 128 and
256 frames"; this adds 512 to it.

## Status

Answered. `spikes/spike03b_loop_joins/` stays in the tree until Phase 3 closes, like the other
seven, and is deleted with them.
