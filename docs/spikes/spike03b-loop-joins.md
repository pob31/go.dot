# Spike 03b — Loop joins: the wrap, the re-trigger, and the placed boundary

## Verdict

**The clip's own loop wrap is the cleanest of the three, at every configuration, and a
looping range is left alone.** That is M12 answered, and it answers in the design's favour:
PR 3.8 already arms every range clip looping, and nothing more has to be built to carry a
range from one pass to the next.

Three findings.

**1. The wrap wins everywhere, by between five and twenty thousand times.** `join_error_samples
= 0` at every block size at both rates — the second pass lands on exactly the sample that
continues the first, nothing dropped and nothing repeated. The damage around the join, measured
as the summed squared deviation from the reference, is smaller than a placed boundary's at
**every one of the ten configurations**, by a factor between 5.5 (48 kHz, 1024 frames) and
23 000 (96 kHz, 128 frames). At 96 kHz up to 256 frames there is no damaged sample at all: the
render and the reference agree to within 16-bit quantisation noise straight through the wrap.

**2. `LaunchHandle::setLooping` on a clip armed not-looping does nothing**, at every block size
and both rates. The clip stops at the end of its first pass and does not come back. This is
what the sources said would happen and it is now measured: `SlotControlNode` captures a stop
duration when the graph is *built* — the clip's length in beats when `isLooping()` is false —
and queues that stop every block, ahead of the wrap. `setLooping` is a rebuild-free seqlock
store and cannot remove a duration that was already captured.

**This is why "every range clip is armed looping" is a mechanism and not a setting.** A range
clip armed the other way could never be made to loop, and the failure would be silence at the
end of the first pass rather than an error.

**3. A placed cross-slot boundary costs a fixed ~25–33 samples of loud damage, and it is
Tracktion's own stop fade.** The position is perfect — `join_error_samples = 0`, so the
incoming range starts on exactly the right sample — but the outgoing range is taken down with
a one-sided decay: 25 samples at 96 kHz and 26–33 at 48 kHz, **independent of block size**, one
of which is actually silent, with a worst deviation of 0.49 against an amplitude of 0.5. That
is `SlotControlNode::processStop`'s `lastSampleFadeLength = std::min (numFrames, 40u)`, which
spike 03 identified and quoted. It is a fade where the reference expects material, not a hole.

Small — 0.26 ms at 96 kHz — and it is the price of every boundary *between* ranges, which is
unavoidable and is M13's to bound. It is not a price a *looping* range has to pay, which is the
whole of what M12 was asked.

## Criterion

Not one of PRD §6.1's seven. This is **M12 of the Phase 3 plan**:

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

Added on top of spike 03, and each one earned:

- the **longest unbroken run** of damaged samples, rather than first-to-last: a span counted
  from the first damaged sample to the last is inflated by isolated quantisation stragglers,
  and comparing two inflated numbers compares two amounts of inflation;
- how many of the damaged samples were **silent** while the reference was not, which is what
  tells a gap from a fade from wrong material;
- the **energy** — summed squared deviation over the window — which is what the verdict
  compares.

**Energy, and not length, and this rig showed why.** At 48 kHz with 512-frame blocks the wrap's
longest damaged run is 55 samples and a placed boundary's is 33, so by length the placed
boundary wins — while the wrap's worst deviation is 0.027 and the placed boundary's is 0.49,
eighteen times louder. A longer, quieter blemish is not worse than a shorter, louder one, and
length alone cannot say so. Both are still reported, because a single number nobody can
decompose is a number nobody can argue with.

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

### Damage energy — summed squared deviation over ±4096 samples of the join

| block | 48 kHz wrap | 48 kHz placed | 96 kHz wrap | 96 kHz placed |
|---|---|---|---|---|
| 64   | **0.00017** | 1.067 | **0.000043** | 0.071 |
| 128  | **0.00017** | 1.061 | **0.000043** | 0.998 |
| 256  | **0.049**   | 1.088 | **0.000043** | 0.950 |
| 512  | **0.144**   | 1.088 | **0.024**    | 0.950 |
| 1024 | **0.144**   | 0.795 | **0.072**    | 0.950 |

### Longest unbroken damaged run, in samples (worst deviation, against amplitude 0.5)

| block | 48 kHz wrap | 48 kHz placed | 96 kHz wrap | 96 kHz placed |
|---|---|---|---|---|
| 64   | **0** (0.013) | 26 (0.498) | **0** (0.0065) | 25 (0.101) |
| 128  | **0** (0.013) | 26 (0.496) | **0** (0.0065) | 25 (0.500) |
| 256  | 38 (0.026) | 33 (0.496) | **0** (0.0065) | 25 (0.487) |
| 512  | 55 (0.027) | 33 (0.496) | **0** (0.013)  | 25 (0.487) |
| 1024 | 55 (0.027) | 33 (0.311) | **0** (0.014)  | 25 (0.487) |

`setLooping` reports *it stopped at the end of the first pass and did not come back* in all ten
cells.

Silent samples inside the damage: **1** for every placed boundary, at every configuration; 0, 2
or 6 for a wrap. Neither join is a hole.

## What was learned

**The wrap is not merely acceptable, it is the best join available in this engine**, and at
96 kHz up to 256 frames it is not a join at all — the render is the reference. That settles the
plan's fallback, "loops become placed same-slot `play` per pass", as unnecessary, and it means
an ambience bed looping for four hours costs Go.dot nothing per pass: no command, no placed
instant, no run record, and no artefact.

**The damage that does exist at 48 kHz with large blocks is quiet and worth a listen.** 55
samples at 5% of full scale is 1.1 ms of very slightly wrong audio at 512 and 1024 frames —
inaudible on a bed, possibly not on a transient. The hardware checklist already carries "a loop
boundary *listened to* at 128 and 256 frames"; this adds 512 at 48 kHz to it, and says the
other cells need no ear.

**A placed boundary's cost is a constant, which is good news for PR 3.9.** It does not grow
with the block size, because it is not a scheduling error: it is Tracktion's fixed 40-sample
stop decay, capped at the block length. So a range playlist's boundaries cost 25–33 samples
each however the rig is set up, and M13's bound — *damaged span ≤ block + 40 samples* — is met
with room to spare before PR 3.9 has written a line.

## Status

Answered. `spikes/spike03b_loop_joins/` stays in the tree until Phase 3 closes, like the other
seven, and is deleted with them.
