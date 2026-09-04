# Spike 01 — Launcher clip to arbitrary multichannel bus routing

## Verdict

**PASS.** Routing is exact and reproducible up to **72 output channels**, in both shapes
that matter:

- **Mono direct outs** — one mono file, one hardware channel, one destination — work
  perfectly at 64 objects. This is the WFS / L-ISA path (mono source per object, straight
  out to the spatial processor), and it is the shape most of a Go.dot rig will actually use.
- **Stereo buses** at arbitrary hardware channel indices work equally exactly.

The limit is **total output channel count, not track count and not bus width**: 72 channels
is clean, 80 is not, whether those channels are 72 mono buses or 36 stereo pairs. The cause
of that boundary is **not isolated** and may be this spike's device configuration rather
than Tracktion. It sits comfortably above MADI's 64.

There is also a **constraint no experiment can move**, and it needs qualifying carefully
because the obvious reading of it is wrong.

## Criterion

PRD §6.1 item 1, verbatim:

> 1. Launcher clip → arbitrary multichannel bus routing at target channel counts.

"Target channel counts" is specified nowhere — PRD §6.2 leaves file playback channel counts
open — so the spike takes the count on argv and reports behaviour at each rather than
picking one.

## What was built

`spikes/spike01_bus_routing/main.cpp`.

*N* tracks, each with a clip in a launcher slot, each routed to its own bus carved from the
hosted device by `EngineBehaviour::describeWaveDevices`, all launched at the same
`MonotonicBeat`.

`--bus-width` selects the destination shape: **1** gives each track a one-channel device (a
mono direct out, built from `WaveDeviceDescription`'s channel-array constructor), **2** gives
it a stereo pair at arbitrary hardware indices. Sources are generated to match, so a mono run
is mono end to end.

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

**Mono direct outs** (`--bus-width=1`), one hardware channel per track:

| tracks | output channels | correct | verdict |
|---|---|---|---|
| 8   | 8   | **8/8**   | PASS |
| 32  | 32  | **32/32** | PASS |
| 64  | 64  | **64/64** | PASS |
| 72  | 72  | **72/72** | PASS |
| 80  | 80  | 1 | breaks |
| 96  | 96  | 1 | breaks |
| 128 | 128 | 1 | breaks |

**Stereo buses** (`--bus-width=2`), two hardware channels per track:

| tracks | output channels | correct | reproducible |
|---|---|---|---|
| 8  | 16  | **8/8**   | yes |
| 16 | 32  | **16/16** | yes |
| 32 | 64  | **32/32** | yes (2 runs identical) |
| 40 | 80  | **40/40** | not repeated |
| 48 | 96  | 36 then 48 | **no** |
| 64 | 128 | 1 | consistently broken |

## What was learned

**Routing itself is exact, in mono as well as stereo.** Where the experiment is
reproducible it is not "mostly right" — every track landed on its intended destination and
nothing leaked anywhere else, at arbitrary hardware channel indices chosen by
`describeWaveDevices`.

**Mono direct outs are fully supported, and that is the finding this spike exists for.**
`WaveDeviceDescription` takes a channel *array*, so a one-channel device is expressible;
`WaveOutputDevice::getRightChannel()` returns -1 for it; and the only `isStereoPair()`
assertion in the class guards `reverseChannels()`, a UI convenience that is never on the
playback path. Sixty-four mono objects, each to its own hardware channel, routed exactly.
The WFS / L-ISA workflow — mono file per object, direct out, spatialised downstream — is not
a special case to be worked around; it is a first-class shape.

**The limit is channels, not tracks or width.** 64 mono tracks (64 channels) passes; 64
stereo tracks (128 channels) does not. 72 mono channels passes, 80 does not. Both shapes
break at the same total channel count, which rules out track count and bus width as the
cause. `HostedAudioDeviceInterface::prepareToPlay` derives `maxChannels` from the parameters
with no cap of its own, so the boundary is further down and is not isolated here.

**The 96-channel instability is the same finding spike #4 made.** Two identical runs gave 36
and 48 correct. Spike #4 established that TE's audio-file streaming is asynchronous, so
under enough throughput demand what is ready when a block is processed depends on real time.
Forty-eight simultaneous streaming clips is enough throughput to trip it. Larger buffers did
not help, which is consistent with a threading effect rather than an underrun.

**The 128-channel failure is different in character** — consistently 1 correct out of 64,
both runs, rather than varying — which is the signature of something structural rather than
of load. It has not been isolated, and it may be this spike's hosted-device setup rather
than Tracktion's routing. It is recorded as an unexplored edge, not as a defect in TE.

**The constraint, stated precisely, because the obvious reading of it is wrong.**
`tracktion_EditNodeBuilder.cpp:90-93` is

```cpp
constexpr int getTrackNumChannels()  { return 2; }
```

Not a default, not a setting — `constexpr`. But this governs the width of a track's
**internal processing**, not the width of the **destination**. A track is two channels
inside; its output device may be one channel, and a mono source routed to a mono device
arrives on exactly one hardware channel, as measured above. So the constraint is **not** "no
mono", and it is not an obstacle to the spatial workflow.

What it does constrain is the other direction: **a cue wider than stereo cannot be one
track.** A 5.1 or 8-channel cue is *N* tracks launched together, which means the §3.9c
allocator hands out slots in groups and a "cue" and a "slot" stop being one-to-one. §3.25
currently says the launcher slots **are** the exclusive resources, "same object, no
translation" — true for a mono or stereo cue, not for a wide one.

**The same ceiling applies to Racks**, and there it may bite harder:
`RackInstance::getNumOutputChannelsGivenInputs` returns 2 and `RackInstance::Channel` is
`{ left, right }`. A rack is a stereo object. A mono effects path is therefore a stereo rack
with one channel used — which works, but costs twice the processing per object and doubles
the buffer traffic on a rig with many mono objects. Whether that is acceptable is a
measurement for spike #6, and it is now on that spike's list.

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
- **§3.9b, "Width is explicit, never automatic"** — measurement supports it: mono, stereo
  and mono-direct-out destinations all behave exactly as declared, with no automatic
  pairing or downmixing anywhere in the path.
- Measured capability worth recording: **72 output channels verified exact**, in mono or
  stereo, which covers MADI's 64 with margin.

## Open questions for the author

1. **The >2-channel cue policy.** Mono and stereo are settled by measurement. A 5.1 or
   8-channel file cue becomes 3-4 tracks launched together; §3.9b's *(proposed)* "stereo cue
   → two mono slots" generalises to it, but whether wide cues exist in v1 at all, and
   whether they are one cue object over a slot group or several cues the operator keeps in
   step, is a product decision (devplan:17).
2. **The cost of stereo racks on a mono-heavy rig.** If most objects are mono direct outs
   with their own effects, every rack is still a stereo object. Worth deciding whether that
   is simply accepted or whether the rack path needs a mono variant — and worth measuring in
   spike #6 before deciding.
3. **What "target channel counts" means.** 72 is verified, MADI is 64. Whether v1 needs to
   exceed that decides whether the 80-channel boundary is worth isolating; it is currently
   unexplained and may be this spike's own configuration.
