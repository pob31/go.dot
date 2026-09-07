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

## And one more, added by Phase 3's triggers (PR 3.7)

**A real MIDI surface, on a real cable.** `wfg serve --midi-in=<device>` opens an input and
turns matching events into `trigger.fire`; `wfg midi` lists what a machine has. Everything from
the callback inwards is tested — the conversion from `juce::MidiMessage` to the engine's own
event has cases of its own, and the matching is a pure function with a dozen more — but *opening
a device and receiving from it* is not, and cannot be: no CI runner has a MIDI interface, and
JUCE creates virtual ports on macOS and Linux only.

What to check, on the Windows box and on the M4 Pro:

1. `wfg midi` lists the surface, by a name that can be typed back into `--midi-in=`.
2. A note fires the cue a trigger names, and **the standby does not move** — which is the
   property the whole feature rests on and the one an operator would never forgive.
3. A note-on of **velocity nought** is matched by a trigger asking for `data: 0` on a `noteOn`.
   That is how a great many surfaces spell "released", JUCE reports it as a note-off by default,
   and the engine deliberately classifies by the status byte instead. It is the one behaviour
   here that a unit test asserts and only hardware can confirm somebody meant.
4. MIDI clock from a device that sends it does not cost anything measurable: twenty-four
   messages a beat arrive on the callback thread and are dropped before the matcher, and it is
   worth watching the tick lateness while one is running.
5. A cable pulled out mid-show. JUCE's input goes quiet; nothing should fall over, and the
   question is whether anything says so.

---

## MIDI cues, out — added at Phase 3's close-out (2026-09-07)

The other direction, and the one with a measured reason to be listened to rather than asserted.
`wfg serve --midi-out=<port name>=<device>` binds a `<Port>` the show declares to a cable this
machine has; a MIDI cue names the port by identifier and `MidiSender` puts the bytes on the wire
from a thread of its own.

Every event type PRD §3.10 lists is checked byte for byte in the unit suite through a recording
sink, which needs no port at all. What that cannot tell anybody is whether the bytes reach a
device and whether the device does what the show meant.

6. `wfg midi` lists the output, by a name that can be typed back into `--midi-out=`.
7. A **program change** reaches a lighting desk and changes what it should. This is the cue that
   exists in every theatre and the one the whole kind is for.
8. A **system-exclusive dump** of a hundred bytes or more arrives intact — and, while it is
   going, the tick lateness at `/godot/engine/lateness` does not move. On Windows
   `sendMessageNow` busy-waits the calling thread for about thirty milliseconds on a dump that
   size, which is a tick and a half; the sender has a thread of its own precisely so that number
   never lands on the one that owns the model. Watching it is how anybody knows the thread is
   where it is supposed to be.
9. A cue naming a port that was **declared and never bound** fails its run with `no-port` while
   the rest of the show runs. A rig that has not been patched yet is a rehearsal.
10. `--midi-out=` naming a device this machine does not have refuses to start, and the message
    lists what the machine does have. That is the one failure that is always a name spelled
    differently.

## A loop boundary, listened to — added at Phase 3's close-out (2026-09-07)

M12 measured the clip's own loop wrap against two alternatives and it won every configuration —
at 96 kHz it leaves **no damaged sample at all** at every block size from 64 to 1024. At 48 kHz
it leaves nothing at 64 and 128 either.

The one cell where it leaves anything is 48 kHz with 512- and 1024-frame blocks: 55 samples at
about 5% of full scale, which is 1.1 ms of very slightly wrong audio at every wrap. Inaudible on
a bed, and the question is what it does to a transient.

11. An ambience bed looping a two-second range, at **48 kHz with 512-frame blocks**, listened to
    for a minute. Then the same file with a transient at the loop point.
12. The same at 128 and 256 frames, which is what a show runs at, and where the measurement says
    there is nothing to hear.
