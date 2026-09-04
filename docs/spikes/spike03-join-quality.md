# Spike 03 — Follow-action join quality

## Verdict

Two questions, two different answers.

**Sample-accurate: YES.** `join_error_samples = 0` at every configuration that measured
cleanly. Two clips that are the two halves of one file reconstruct that file *exactly* across
a follow-action boundary — residual 8 × 10⁻¹², which is 24-bit quantisation noise.

**Crossfade at the boundary without a custom clip: NO**, and not "not exposed" — what exists
is actively the wrong shape. Tracktion applies a **one-sided decay of the outgoing clip that
overwrites the incoming audio**. It is not a crossfade and cannot be configured into one.

And a third thing, which nobody asked for and which matters most in practice: **the audio
artefact at the join is buffer-size dependent**, because the clip switch happens on a block
boundary while the *position* stays sample-accurate. At 256 frames it is 168 samples — 3.5 ms
of wrong audio at every range boundary.

## Criterion

PRD §6.1 item 3, verbatim:

> 3. Follow-action join quality: sample-accurate? crossfade at the boundary achievable
>    without a custom clip?

## What was built

`spikes/spike03_join_quality/main.cpp`.

Clip A is `[0, L/2)` of a source file; clip B is `[L/2, L)` of the **same** file, reached with
`Clip::setOffset`. A carries a `FollowAction::trackNext` to B. If the join is sample-accurate
the output reconstructs the original file, so the measurement compares against material that
already exists rather than against a theory of where the join should be.

**The source is a chirp, not a sine, and that is load-bearing.** Alignment is found by
searching for the offset that best matches the reference. Against a pure sine that search is
ambiguous modulo one period — at 1 kHz / 48 kHz any error above 24 samples aliases into a
small one, and the spike would report near-perfect joins however badly Tracktion performed. A
linear chirp is never self-similar, so alignment is unique across the whole search range.

The measurement is deliberately a **difference of two alignments** — one before the join, one
after — because spike #4 established that TE's launch instant is not reproducible. The launch
jitter is common to both and cancels.

Both alignments are integer sample offsets, so this rig resolves to whole samples and
**makes no sub-sample claim**. (Spike #2's report records what happens when that line gets
crossed.)

## How it was run

```
build:   Debug, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine v3.1.0 (runtime string), JUCE v8.0.6
command: spike03_join_quality --tracks=1 --sample-rate=SR --buffer=B
device:  none — TE hosted audio device interface, no hardware opened
```

Every configuration runs twice; the two runs must be bit-identical or the spike reports
`HARNESS-ERROR` and no verdict.

## Numbers

| sample rate | buffer | `join_error_samples` | artifact length | artifact |
|---|---|---|---|---|
| 48 kHz | 32  | **0** | 32 samples  | 0.67 ms |
| 48 kHz | 64  | — | — | control failed, no verdict |
| 48 kHz | 128 | **0** | 40 samples  | 0.83 ms |
| 48 kHz | 256 | **0** | 168 samples | 3.50 ms |
| 96 kHz | 128 | **0** | 40 samples  | 0.42 ms |

Reconstruction residual either side of the join: `8.12 × 10⁻¹²` before, `8.22 × 10⁻¹²` after.
Peak deviation inside the artefact: 0.20 (−13.9 dBFS) at 48 kHz, 0.10 at 96 kHz.

## What was learned

**The join is sample-accurate, and that is a strong result.** Not "within tolerance" — the
second half lands on exactly the sample that continues the first. PRD §3.24's "sample-accurate
loop points" is safe.

**There is no crossfade, and the mechanism is worth quoting** because it is not simply a
missing feature. `SlotControlNode::processStop` (`tracktion_SlotControlNode.cpp`):

```cpp
if (const auto lastSampleFadeLength = std::min (numFrames, 40u); lastSampleFadeLength > 0)
    for (uint32_t i = 0; i < lastSampleFadeLength; ++i)
    {
        auto alpha = i / (float) lastSampleFadeLength;
        dest[i] = lastSample * (1.0f - alpha);      // assigns, does not mix
    }
```

That is a linear ramp from the outgoing clip's **last sample** down to zero, at most 40
samples long, **assigned into the destination**. Three consequences:

1. It is **one-sided**. Nothing of the incoming clip is faded in; the incoming samples in that
   window are *overwritten* by the decaying tail of the outgoing one.
2. It is **not configurable** — 40 is a literal, and clip fade nodes are skipped entirely for
   launcher clips (`tracktion_EditNodeBuilder.cpp:637`), so the usual fade controls do not
   reach this path.
3. It is a **click suppressor, not a musical fade**. Its job is to avoid a step discontinuity
   when a clip stops, and it does that well.

So §6.1's second question is answered NO. A crossfade at a range boundary has to be built by
Go.dot: two slots playing simultaneously with a §3.10 curve on each, which is a second slot's
worth of the §3.9c allocator's budget per crossfaded boundary.

**The artefact is buffer-dependent, and this is the finding with real consequences.** The
lengths are not a fixed fade: 32, 40 and 168 samples at buffers of 32, 128 and 256. The
pattern is `(distance from the join to the next block boundary) + min(blockSize, 40)`:

- at buffer 32 the fade is truncated to the block — `min(32, 40)` = 32;
- at buffer 128 the join happened to land on a boundary — 0 + 40;
- at buffer 256 it landed 128 samples early — 128 + 40 = **168**.

The *position* stays sample-accurate throughout; it is the *audio* in that window that is
wrong. So the corrupted span scales with buffer size, and at 256 frames it is **3.5 ms**.
That is well above the threshold of audibility for a transient-rich boundary, and it is the
number Phase 3 has to design around when it decides what a range boundary sounds like.

**Two Tracktion behaviours found the hard way, both of which will bite Phase 3:**

1. **Launcher clips loop by default** (`clipA_is_looping = 1` before anything is done to it),
   and looping selects a different branch of the follow-action duration logic. The looping
   branch needs `followActionNumLoops > 0`, and with it at its default of 0 the
   `SlotControlNode` is built with **no duration at all** — so the follow action never fires
   and the clip loops for ever. The symptom is not silence, it is audio that never stops.
2. **`disableLooping()` overwrites the clip's offset.** `AudioClipBase.cpp:916` is
   `pos.offset = toDuration (loopStart);`. Calling it after `setOffset` silently throws the
   offset away, and the clip plays from the start of the file. This cost most of the spike:
   clip B played source position 0 instead of 2 s, nothing aligned, and the spike reported
   "the follow action did not fire" — when it had fired perfectly. What gave it away was
   dumping the audio and seeing **four seconds of continuous output for two two-second
   clips**. Disable looping *first*, then set the offset.

**One configuration has no verdict.** At 48 kHz / 64 frames the two control runs differ, so
the spike reports `HARNESS-ERROR` rather than a number — the same non-reproducibility spikes
#1 and #4 found under load.

## Consequences for the PRD

- **§3.24, "Joins"** — *"Sample-accurate loop points, with an optional short crossfade at the
  join (proposed)"*. The first half is **confirmed**. The second is **not obtainable from
  Tracktion's launcher path**: what exists is a ≤40-sample one-sided decay that overwrites the
  incoming audio. A crossfade must be built from two simultaneous slots plus Go.dot-owned
  gain, so the *(proposed)* crossfade is now a **cost** decision — a slot and a curve per
  crossfaded boundary — not a capability question.
- **§3.24 / §3.25, ranges** — add the buffer-dependent artefact. A range boundary is
  sample-accurate in *position* but corrupts up to one block plus 40 samples of *audio*.
  Anything that depends on a clean boundary must either accept that or own the transition.
- **§3.9c, the allocator** — if crossfaded joins are wanted, a range with *N* crossfaded
  boundaries needs a second slot live across each one. That is allocator budget the current
  model does not account for.

## Open questions for the author

1. **Is the 3.5 ms artefact at 256 frames acceptable, or does it set a buffer-size ceiling?**
   It shrinks with the buffer (0.67 ms at 32 frames), so the choice interacts with the latency
   budget rather than being free. This is a product decision about what a range boundary is
   allowed to sound like.
2. **Are crossfaded joins worth a slot each?** §3.24 marks them *(proposed)*; they are
   achievable but only by spending allocator budget. Withdraw, or accept the cost?
3. **Is the artefact audible on real material?** Everything here is measured against a chirp,
   which is the right instrument for *detecting* it and the wrong one for judging it. Worth a
   listen on real programme material before deciding question 1.
