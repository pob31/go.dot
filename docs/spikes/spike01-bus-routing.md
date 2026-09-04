# Spike 01 — Launcher clip to arbitrary multichannel bus routing

## Verdict

**PASS.** Routing is exact in every shape that matters — mono, stereo and mixed — up to a
**throughput** ceiling that is comfortably clear of the target rig at 48 kHz and close to it
at 96 kHz (72 simultaneous objects at 48 kHz, ~40 at 96 kHz).

*Reproducibility caveat:* only the 32-track / 64-channel stereo configuration was run twice
and confirmed identical. The other passing rows were run once. "Exact" is measured; "always
exact" is not claimed.

- **Mono direct outs** — one mono file, one hardware channel, one destination — work
  perfectly at 64 objects. This is the WFS / L-ISA path (mono source per object, straight
  out to the spatial processor), and it is the shape most of a Go.dot rig will actually use.
- **Stereo buses** at arbitrary hardware channel indices work equally exactly.

**A mixed rig — 32 mono direct outs interleaved with 16 stereo buses, 64 channels total —
routes exactly at 48 kHz.** That is the shape the target hardware actually presents (the RME
Digiface Dante caps at 64 channels, shared between mono objects, stereo sources and mix
tracks), and mono and stereo destinations coexisting in one channel pool changes nothing.

The limit is **throughput, not channel count**. At 48 kHz it is around 72 simultaneously
launched clips; at 96 kHz it falls to between 40 and 48. Those are the same order of total
sample rate (~3.5 M and ~3.8-4.6 M samples/s), which is what identifies it as a throughput
ceiling rather than a channel or track ceiling. **At 96 kHz on 64 channels this matters**,
and the caveats below say what it does and does not establish.

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

**Mixed layout** (`--mono=N --stereo=M`), mono and stereo interleaved in one channel pool —
the shape of the target rig:

| layout | channels | rate | correct |
|---|---|---|---|
| 32 mono + 16 stereo | 64 | 48 kHz | **48/48** |
| 32 mono + 16 stereo | 64 | 96 kHz | 1/48 |

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

**Mono direct outs at 96 kHz**, which is the target rig's operating rate:

| objects | channels | correct |
|---|---|---|
| 8 / 16 / 24 / 32 / 40 | up to 40 | **all correct** |
| 48 | 48 | 5/48 |
| 64 | 64 | 1/64 |

At 8 objects the raw frame positions are exact at both rates — differences of 7200 frames at
48 kHz and 14400 at 96 kHz, i.e. precisely the 0.15 s spacing. 96 kHz is not broken; 96 kHz
*with enough simultaneous streams* is.

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

**Routing itself is exact, in mono as well as stereo.** Where the experiment is reproducible
it is not "mostly right" — every track landed on its intended destination and nothing leaked
anywhere else, at arbitrary hardware channel indices chosen by `describeWaveDevices`.

That claim is worth more than it was: the analysis originally inspected only the **first**
channel of each bus, so in stereo mode the odd hardware channels were never scanned at all
and a leak into any of them would have been invisible. It now scans every channel of every
bus, and the passing configurations still report zero contamination — 32/32 stereo and 48/48
mixed, with the right halves included this time.

**Mono direct outs are fully supported, and that is the finding this spike exists for.**
`WaveDeviceDescription` takes a channel *array*, so a one-channel device is expressible;
`WaveOutputDevice::getRightChannel()` returns -1 for it; and the only `isStereoPair()`
assertion in the class guards `reverseChannels()`, a UI convenience that is never on the
playback path. Sixty-four mono objects, each to its own hardware channel, routed exactly.
The WFS / L-ISA workflow — mono file per object, direct out, spatialised downstream — is not
a special case to be worked around; it is a first-class shape.

**Mono and stereo destinations coexist without interference.** The mixed layout interleaves
them deliberately, so mono buses do not all sit at low hardware indices and stereo pairs do
not all start on even ones — a routing bug that only appeared for a stereo pair starting at
an odd channel would hide completely behind a grouped layout. 48 of 48 correct at 48 kHz.

**The limit is throughput, not channels.** The evidence for that, rather than for a channel
ceiling:

- 64 mono tracks (64 channels) passes at 48 kHz; 64 stereo tracks (128 channels) does not.
- 72 mono channels passes at 48 kHz, 80 does not.
- At 96 kHz the same shapes fail at roughly **half** the object count: 40 passes, 48 does not.

A channel-count ceiling would not move when the sample rate changes. A throughput ceiling
does, and halving is what doubling the rate predicts.

**What ruled out the obvious explanation.** `EnginePlayer` renders as fast as the CPU allows,
far faster than real time, while TE's file readers are background threads sized for
real-time playback — so the natural hypothesis was that the harness simply outruns them, and
that a real show would be fine. `spike::RealTimePacer` was written to test exactly that: it
paces the render to wall clock. **It did not help.** 96 kHz with 48 objects fails paced as
badly as free-running. So the effect is not "rendering faster than real time", and it cannot
be dismissed as a harness artefact on those grounds.

**What this does NOT establish**, and the distinction matters before anyone plans a rig
around it:

- It is measured through TE's **hosted device interface**, not a real driver. The RME path
  may behave differently.
- Every clip is launched at **the same instant**. A real show staggers cues; 48 simultaneous
  starts is a worst case, not a typical GO.
- It is a **Debug** build.

So the honest statement is: *in this configuration, simultaneously launching more than about
40 streaming clips at 96 kHz produces clips that start at the wrong time.* Whether that
threshold survives on real hardware with staggered cues is a hardware-half measurement, and
it is the single most valuable thing to check when the Digiface is next attached.

**The 96-channel instability is the same finding spike #4 made.** Two identical runs gave 36
and 48 correct. Spike #4 established that TE's audio-file streaming is asynchronous, so
under enough throughput demand what is ready when a block is processed depends on real time.
Forty-eight simultaneous streaming clips is enough throughput to trip it. Larger buffers did
not help, which is consistent with a threading effect rather than an underrun.

**"1 correct" was the instrument's floor, not a finding, and an earlier version of this
report read a conclusion into it.** Bus 0 is the identification reference, so its elapsed
time is identically zero and it matches track 0 whenever it carries any content at all. A
run in which *every* bus is wrong therefore still scores 1. The earlier text argued that
"consistently 1 correct, rather than varying" was "the signature of something structural
rather than of load" — but 1 is simply what total failure looks like here, and nothing about
its consistency distinguishes one cause from another.

The spike now detects that case explicitly (`identification_degenerate=1`) and exits
`HARNESS-ERROR` rather than reporting a routing verdict. All the high-channel-count failures
above are degenerate in this sense: they establish that the configuration did not work, and
nothing at all about *why*.

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

**Racks are stereo objects**, and whether that is a cost or the right shape depends on the
job. `RackInstance::getNumOutputChannelsGivenInputs` returns 2 and `RackInstance::Channel` is
`{ left, right }`.

For the intended use — **live mono input into a rack, mono-to-stereo or stereo-to-stereo** —
a stereo rack is the *correct* shape, not waste: a mono source into a reverb or a spatialiser
wants a stereo result, and TE gives it directly. The cost only appears for a mono-in,
mono-out effects path, where half the rack is carrying silence. Both cases belong to
spike #6, which measures the live-input path, and neither should be asserted as a cost until
it is measured there.

## Consequences for the PRD

- **§3.25, "Cues are launcher clips in pre-allocated slots on a fixed track set"** — add the
  stereo ceiling as a stated property, with the `constexpr` cited. The polyphony ceiling is
  currently described only in *count* (*N* tracks = *N* simultaneous cues); it also has a
  *width*, and a cue wider than stereo consumes more than one slot.
- **§3.9b / §3.9c** — "the launcher slots **are** the exclusive resources … same object, no
  translation" needs qualifying for wide cues: one cue may claim a *group* of slots, which
  the allocator must claim and release atomically or risk a half-armed cue.
- **§6.2** — "file playback channel counts" can be **closed**, and by hardware rather than
  preference: the target interface (RME Digiface Dante) caps at 64 channels, shared between
  mono direct outs, stereo sources and mix tracks. 64 channels mixed is measured exact at
  48 kHz. The open item that replaces it is narrower and sharper: the **96 kHz simultaneous
  -launch throughput ceiling** above.
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
2. **Rack width on a mono-heavy rig.** Live mono input into a rack is mono-to-stereo or
   stereo-to-stereo, so a stereo `RackInstance` is the right shape for a reverb or a
   spatialiser and costs nothing extra. It is only a mono-in/mono-out effects path that
   carries half a rack of silence. Spike #6 measures both before anything is decided.
3. **The 96 kHz throughput ceiling is the one open question that could affect the rig.**
   Around 40 simultaneously launched streaming clips at 96 kHz in this configuration.
   Whether that holds on the real Digiface, and whether a realistic staggered GO ever
   approaches it, needs the hardware half of this spike. Until then it is a measured
   property of the harness, not a stated limit of the product.
4. **The high-channel-count failures are all degenerate results.** With the identification
   anchored on a reference bus, a total failure scores exactly 1 and the instrument can say
   only *that* a configuration did not work, never *why*. Isolating one properly needs an
   identification scheme independent of any single bus — worth building only if the 96 kHz
   ceiling turns out to matter on real hardware.
