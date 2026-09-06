# The hardware checklist — PR 2.7

*Written 2026-09-06, from the Windows box. Everything below needs a machine with an audio
interface, which is why it is a checklist and not a test.*

## What is already known

PR 2.7's device layer is built and tested, and the parts that do not need a particular
interface are in the unit suite (`tests/DeviceTests.cpp`). What ran on the Windows dev box:

- `wfg devices` lists five devices across four types (Windows Audio, Exclusive, Low Latency,
  DirectSound) with their channel counts, rates and buffer sizes.
- `DeviceAudioDriver` opened `Speakers (Cirrus Logic XU)` and **was granted 48 000 Hz / 480
  frames / 6 outputs when it asked for 256 frames** — which is the whole reason PRD §6.2 says
  the rate is observed and never set, demonstrated on the first device anybody tried.
- The device callback drove `AudioHost::processBlock` and advanced Go.dot's sample clock by
  exactly ten blocks in ten callbacks.
- `wfg serve --device="…" --device-type="Windows Audio" --buffer=480` brought a show up on it:
  22 graph nodes, 6 outputs, and the ports printed as usual.

**Nothing has been listened to.** Every statement above is about counters and return values.

## Two bugs it found, both in the seam rather than in either side

Recorded because they will look like Go.dot bugs to the next person who reads the code.

**spatcore's `DeviceHost` assumes an initialised manager.** `openNamedDevice` sets the device
type and then names the device; on a `juce::AudioDeviceManager` that has never been
`initialise`d, the second half answers *"No such device"* for a device the enumeration listed
by that exact name a moment earlier. spatcore's own consumers restore from saved state on
launch and never take this path. Go.dot initialises first.

**It also names the device as an INPUT.** `setDeviceAllChannels` sets `inputDeviceName` and
`outputDeviceName` to the same string, which is right for the RME-class interfaces spatcore was
written for and wrong for most things a show plays through: this machine's speakers have six
outputs and no inputs, so naming them as an input put the lookup in an empty list. Go.dot opens
**output-only** — which is also what a playback engine wants (§3.25: Tracktion is a commanded
player) and stops Go.dot holding an input somebody else needs exclusively. The *policy* is
still spatcore's and is the part worth reusing: explicit masks with both `useDefault…Channels`
flags cleared, because while either is set JUCE throws the caller's mask away.

## To do on a machine with a real interface

### On the Windows box, with the Digiface Dante attached

1. `wfg devices` — confirm the Digiface appears, with its real channel count. Note whether it
   shows under **ASIO**; if it does not, `WFG_ASIO_SDK` has not been pointed at the SDK and the
   build is WASAPI/DirectSound only (author decision I, 2026-09-05).
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
