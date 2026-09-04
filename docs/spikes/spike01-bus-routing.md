# Spike 01 — Launcher clip to arbitrary multichannel bus routing

## Verdict

**PASS up to 64 output channels**, exactly and reproducibly: every track reached its own
stereo bus, at arbitrary hardware channel indices, with no leakage onto any other bus. That
covers MADI scale, which is the largest channel count the author's rig presents.

Above that there is an edge, and it is reported rather than rounded off: at **96 channels**
the matrix is correct but no longer reproducible run to run, and at **128 channels** it does
not hold at all. The cause of the 128-channel case is **not isolated** and may well be this
spike's own device configuration rather than Tracktion.

Alongside the measurement there is a **hard constraint that no experiment can move**, and it
is the more consequential half of this spike for the PRD.

## Criterion

PRD §6.1 item 1, verbatim:

> 1. Launcher clip → arbitrary multichannel bus routing at target channel counts.

"Target channel counts" is specified nowhere — PRD §6.2 leaves file playback channel counts
open — so the spike takes the count on argv and reports behaviour at each rather than
picking one.

## What was built

`spikes/spike01_bus_routing/main.cpp`.

*N* tracks, each with a clip in a launcher slot, each routed to its own stereo bus carved
from the hosted device by `EngineBehaviour::describeWaveDevices`, all launched at the same
`MonotonicBeat`.

**Each track's source has its single non-zero sample at a different time** — track *i* at
`0.5 + 0.15i` seconds. The transient time is therefore that track's **identity**: a bus
carrying track *i*'s audio has its transient at track *i*'s time and nowhere else. That
makes the routing matrix exact rather than inferred — no FFT, no amplitude coding, no
thresholding a spectrum. A bus is scored `correct`, `misrouted`, `contaminated` (its own
transient plus somebody else's) or `silent`, and those four are counted separately because
the distinction is what makes a failure diagnosable.

## How it was run

```
build:   Debug, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine v3.1.0 (runtime string), JUCE v8.0.6
command: spike01_bus_routing --tracks=N --sample-rate=48000 --buffer=128
device:  none — TE hosted audio device interface, no hardware opened
```

## Numbers

| tracks | output channels | correct | misrouted | contaminated | silent | reproducible |
|---|---|---|---|---|---|---|
| 8  | 16  | **8/8**   | 0 | 0 | 0 | yes |
| 16 | 32  | **16/16** | 0 | 0 | 0 | yes |
| 32 | 64  | **32/32** | 0 | 0 | 0 | yes (2 runs identical) |
| 40 | 80  | **40/40** | 0 | 0 | 0 | not repeated |
| 48 | 96  | 36 then 48 | 12 then 0 | 0 | 0 | **no** |
| 64 | 128 | 1 | 63 | 0 | 0 | consistently broken |

## What was learned

**Routing itself is exact.** Where the experiment is reproducible, it is not "mostly right"
— every track landed on its intended stereo bus and nothing leaked anywhere else, at
arbitrary hardware channel indices chosen by `describeWaveDevices`. Sixty-four output
channels is MADI scale and it is clean.

**The 96-channel instability is the same finding spike #4 made.** Two identical runs gave 36
and 48 correct. Spike #4 established that TE's audio-file streaming is asynchronous, so
under enough throughput demand what is ready when a block is processed depends on real time.
Forty-eight simultaneous streaming clips is enough throughput to trip it. Larger buffers did
not help, which is consistent with a threading effect rather than an underrun.

**The 128-channel failure is different in character** — consistently 1 correct out of 64,
both runs, rather than varying — which is the signature of something structural rather than
of load. It has not been isolated, and it may be this spike's hosted-device setup rather
than Tracktion's routing. It is recorded as an unexplored edge, not as a defect in TE.

**The hard constraint, which matters more than the numbers.**
`tracktion_EditNodeBuilder.cpp:90-93` is

```cpp
constexpr int getTrackNumChannels()  { return 2; }
```

Not a default, not a setting — `constexpr`. A Tracktion track carries **two channels**, and
`RackInstance::getNumOutputChannelsGivenInputs` returns 2 as well. So "arbitrary
multichannel routing" in Tracktion means *N stereo buses at arbitrary hardware indices*, and
never *one track carrying six channels*.

For Go.dot that settles a question of shape: **a cue wider than stereo cannot be one track.**
It is *N* tracks launched together, which means the §3.9c allocator hands out slots in
groups, and a "cue" and a "slot" stop being one-to-one — §3.25 currently says the launcher
slots **are** the exclusive resources, "same object, no translation". For a stereo cue that
stays true. For a 5.1 or 8-channel cue it does not.

## Consequences for the PRD

- **§3.25, "Cues are launcher clips in pre-allocated slots on a fixed track set"** — add the
  stereo ceiling as a stated property, with the `constexpr` cited. The polyphony ceiling is
  currently described only in *count* (*N* tracks = *N* simultaneous cues); it also has a
  *width*, and a cue wider than stereo consumes more than one slot.
- **§3.9b / §3.9c** — "the launcher slots **are** the exclusive resources … same object, no
  translation" needs qualifying for wide cues: one cue may claim a *group* of slots, which
  the allocator must claim and release atomically or risk a half-armed cue.
- **§6.2** — "file playback channel counts" is open; it can now be narrowed from
  open-in-general to open-with-a-known-constraint.
- Measured capability worth recording: **64 output channels verified exact**, which is what
  the author's MADI rig presents.

## Open questions for the author

1. **The >2-channel cue policy.** This is the decision the constraint forces and it is
   explicitly yours (devplan:17). A 5.1 or 8-channel file cue becomes 3–4 tracks launched
   together; §3.9b's *(proposed)* "stereo cue → two mono slots" generalises to it, but
   whether wide cues are supported at all in v1, and whether they are one cue object over a
   slot group or several cues the operator must keep in step, is a product decision.
2. **What "target channel counts" means.** 64 is verified. Whether v1 needs to go beyond
   MADI decides whether the 96- and 128-channel edges above are worth chasing.
3. The 128-channel breakdown deserves isolation before it is quoted as a Tracktion limit —
   it may be mine.
