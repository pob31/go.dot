# The hardware checklist — PR 2.7

*Written 2026-09-06, from the Windows box. Everything below needs a machine with an audio
interface, which is why it is a checklist and not a test.*

## What is already known, and how little it is

PR 2.7's device layer is built and tested, and the parts that do not need a particular
interface are in the unit suite (`tests/DeviceTests.cpp`). What ran on the Windows dev box:

- `wfg devices` lists five devices across four types (Windows Audio, Exclusive, Low Latency,
  DirectSound) with their channel counts, rates and buffer sizes.
- `DeviceAudioDriver` opened `Speakers (Cirrus Logic XU)` and **was granted 48 000 Hz / 480
  frames when it asked for 256** — which is the whole reason PRD §6.2 says the rate is observed
  and never set, demonstrated on the first device anybody tried.
- The device callback drove `AudioHost::processBlock` and advanced Go.dot's sample clock by
  exactly ten blocks in ten callbacks.
- `wfg serve --device="…" --device-type="Windows Audio" --buffer=480` brought a show up on it.

**Nothing has been listened to.** Every statement above is about counters and return values.

**And the channel count means less than it looks** (author, 2026-09-06). That built-in interface
will never actually carry six channels: most of them are virtual, or exist only over HDMI. So
what the enumeration and the open prove is that the *plumbing* works — a name is found, a device
opens, a rate is read back, a callback arrives and moves the clock. They prove nothing about
routing to real outputs, which is what the Digiface is for.

That is worth saying plainly because `wfg devices` cannot tell the difference. A driver reports
what it reports; a channel that is virtual and a channel with an XLR behind it look identical
from here, and no amount of care in this layer will change that. **The only instrument that can
tell them apart is somebody listening**, which is the whole content of the list below.

## Two bugs it found, both in the seam rather than in either side

Recorded because they will look like Go.dot bugs to the next person who reads the code.

**spatcore's `DeviceHost` assumes an initialised manager.** `openNamedDevice` sets the device
type and then names the device; on a `juce::AudioDeviceManager` that has never been
`initialise`d, the second half answers *"No such device"* for a device the enumeration listed
by that exact name a moment earlier. spatcore's own consumers restore from saved state on
launch and never take this path. Go.dot initialises first.

**It also names the device as an INPUT, unconditionally.** `setDeviceAllChannels` sets
`inputDeviceName` and `outputDeviceName` to the same string, which is right for the RME-class
interfaces spatcore was written for and wrong for anything that only plays: this machine's
speakers have no inputs at all, so naming them as one put the lookup in an empty list.

The first fix was "never ask for inputs", justified as what a playback engine wants. **That was
wrong** (author, 2026-09-06): the rack will have inputs, and on Windows most users will be on
ASIO, where there is one device for both directions and no separate selection to make — so
refusing inputs there would be refusing half of the only device on offer. Go.dot now names a
device as an input **when it has inputs**, which is true of an interface and false of a pair of
speakers and needs no flag to decide.

The *policy* is still spatcore's and is the part worth reusing: explicit masks with both
`useDefault…Channels` flags cleared, because while either is set JUCE throws the caller's mask
away.

## To do on a machine with a real interface

### On the Windows box, with the Digiface Dante attached

**ASIO is the case that matters here.** Most Windows users will be on it, it presents one device
for both directions, and it is the only path on that platform with a channel count and a latency
worth having. Everything below assumes it; if the Digiface only shows under Windows Audio, the
build has no ASIO and item 1 is the whole of the answer.

1. `wfg devices` — confirm the Digiface appears **under ASIO**, with its real channel count and
   its inputs. If it does not appear there at all, `WFG_ASIO_SDK` has not been pointed at the
   SDK and the build is WASAPI/DirectSound only (author decision I, 2026-09-05). Confirm the
   input count is non-zero and that Go.dot opened them — this is the first device that will
   exercise the input half at all.
2. `wfg serve <bundle> --device="<Digiface>" --sample-rate=48000 --buffer=128` and **listen**.
   A media cue should be audible, in the right channels, at the right level.
3. **The rate refusal.** Set the Dante clock domain to 44 100 and start with
   `--sample-rate=48000`. It must refuse with a message naming both numbers, rather than
   running every cue 8.8% out. This is the safe reading of §6.2 and the decision is still
   yours: refuse, warn, or resample.
4. `/godot/engine/latenessMax` over a few minutes at buffer 128, then at 64. The unit suite
   asserts nothing tight here because CI runners are shared; this is the machine where the
   number means something.
5. `/godot/engine/rtViolations` stays 0 with audio running through a real driver — the
   allocation counter is compiled in, but nothing has yet watched it under a device interrupt
   rather than the hosted pump.

### On the M4 Pro

6. The same four, on CoreAudio.
7. **`enableAudioWorkgroup (true)` at buffer 32**, which the Phase 2 plan lists as an author
   action. Go.dot does not call it yet; the question is whether it is needed before the buffer
   goes that low.
8. Confirm that **one** device is enough there for now. macOS lets an application assign several
   peripherals at once, the way QLab does, and Go.dot deliberately does not: `--device=` names
   one and opens it (author, 2026-09-06 — *"for the moment, let's focus on a single one"*).

   Worth knowing what is being deferred rather than merely that it is. An aggregate device is
   CoreAudio's own answer and costs Go.dot nothing — it appears as one device and this layer
   cannot tell. What Go.dot does not do is what QLab does: hold several *separate* devices open
   and route between them, which needs a clock master, a decision about what happens when one of
   them drifts, and a device column in `Bus` that the document does not have. None of that is
   hard; all of it is a design nobody has needed yet.

## What PR 2.7 deliberately does not have

- **`audio.deviceStarted` and the tick-clock rebase.** The command is specified in the
  namespace draft §11.4 with a `switchSample` argument, and `DeviceAudioDriver::switchSample()`
  reports the sample where the device's clock took over — but nothing rebases yet. It matters
  only for a device that starts, stops and restarts mid-show, which is M8's subject.
- **M8, the mid-show rate change.** The plan is explicit that what Tracktion does to in-flight
  clips under one is to be *measured and written down, not designed*, and that needs the
  Dante: it is the only interface here that can change its rate underneath a running process.
- **Device hot-swap.** `TeSession` generations exist in the plan for this; nothing swaps yet.

Each is a real gap rather than an oversight, and each needs the hardware in the room.
