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

*Re-measured 2026-09-24, at the Tracktion pin `13b5132`:* the wrap is now exact in every cell,
48 kHz at 512 and 1024 frames included (`docs/spikes/spike03b-loop-joins.md`). Item 11 no
longer has a measured blemish behind it; it stays as a listen through a real device.

## A double Esc and what was still on its way — added by H4 (2026-10-02)

A double Esc now drops what a cue handed over and had not yet left - a value a network device's
rate cap was holding back, a cue's MIDI message still in the sender's queue - and sends a note-off
for each note a cue started on a synth and nothing has ended: one per port, channel and key, and
nothing else (the author's rule, 2026-09-30; namespace draft §23.10). The unit suite checks the
queue and the note record through a sink that sends by hand. What it cannot check is a synth on a
cable, and the sending thread's own timing.

13. A MIDI cue holds a note on a real synth (a `noteOn`, nothing after it). **Double Esc**: the
    note stops, and the network monitor shows exactly one note-off for it, `8n key 00` on the
    cue's channel - not an all-notes-off, and nothing on any other port.
14. The same note **already ended** by a cue's own note-off (or a note-on at velocity nought),
    then a double Esc: nothing more goes out. And **Esc** with the note held: nothing goes out and
    the note rings on, as Esc promises.
15. A D700 (or any surface on a port of its own) lit and showing faders while a synth note is held:
    after the double Esc its LEDs and motor faders are where they were - its traffic is never
    dropped - and the synth gets its note-off.
16. A lighting desk on a mount whose `rateCap` is low (two hertz): a cue writes, then another
    within half a second, then a double Esc - the second value never arrives at the desk, on the
    monitor or on the desk itself.

## The show closing, the lane's fader, and a pass ended by hand — added by K4 (2026-10-02)

Three of the author's rulings (namespace draft §23.15). Go.dot quitting, or its window going as
another show opens, now sends a note-off for each note a cue left down, as a double Esc does; a
double Esc lets a fader taken for lane recording go, where Esc keeps it; and Rec pressed again
stops the pass's cue rather than killing it. The unit suite checks the queue at the close and the
lane table; what it cannot check is a synth on a cable and a motor fader.

17. A MIDI cue holds a note on a real synth. **Quit Go.dot** (or open another show so this window
    goes): the note stops - exactly one `8n key 00` for it. The same with the note already ended by
    a cue's own note-off: nothing more goes out at the quit.
18. On the D700, a fader taken for a lane (Rec in the waveform, then touch the fader), a pass
    running with the hand on it. **Double Esc**: the fader goes back to what it rode before it was
    taken (its DCA, or the sampler clip on it) - no move to the lane's start first - and the strip's
    screen no longer reads `lane`. With the hand still on the fader, moving it moves nothing - the
    DCA's trim stays where it was - and the motor flies to the DCA's level only once the hand lifts.
    **Esc** instead: the fader stays on the lane, and Rec starts the next pass with nothing taken
    again.
19. A media cue with a reverb insert, its lane recorded: **Rec pressed again** ends the pass and the
    reverb's tail rings out - it is not cut, as it was before K4.

## Doh! — taking back a GO pressed too soon — added by D5 (2026-10-03)

Doh! (PRD §3.32, namespace draft §24) is built: a GO pressed before its moment is taken back - the
pointer, the history and what the GO started; what was heard paused and carried on by the next GO,
what nobody heard handed back exactly; a desk value put back on a device that takes back, nothing
sent again to one left to its operator; a cue the GO stopped made again; one report of all of it on
the transport line. The unit suite drives every road against a fake audio side and a fake wire, and
`blackbox.doh` drives the shipped binary over a hosted render and two mock devices. What neither can
do is listen through an interface, look at the window, or put a real desk, a motor fader and a
lighting operator on the other end. Gathered here from every stage's list (§24.9 to §24.14).

On the window, at first look:

20. **The Doh! button and F9.** A GO, then the button reads "Doh! 12" in its own pink, fades over
    the window's last two seconds and is disabled once it is over; F9 held sends one Doh!, and a
    second press after the Doh fade forgets the resume (the button's tooltip says so first).
21. **The inspector's "on Doh!" row** on an OSC and a MIDI cue - "as the device (Meh)" or "as the
    device (Undo(h))", "Undo(h)", "Meh" - and the Network and MIDI tabs' Doh! and Sound cells.
22. **The notice**: a Doh's report on the transport line - its words, its colour, how much of a long
    report the row holds, the whole of it on hover - once, whatever list has the focus ("Doh! on
    Act 2: ..."), what was left to an operator first, and a newer refusal taking the line back.

On the MADIface (or any interface), listening:

23. **A bed GO'd too soon and Doh'd five seconds in.** GO inside the Doh fade: it comes back up where
    it was. GO after the fade: it comes back at the point within a launch latency, the faded second
    heard again, no click. Doh! twice, then GO: from its top, at full level.
24. **A double GO on a bed and on a scene** (the debounce set to nought, two presses a tick apart):
    the second GO taken back, the first carrying on, nothing heard twice.
25. **A scene of three sounds and a fade outside it, Doh'd and re-seated**: the sounds at their
    seconds, the late one on its time, the fade fired again.
26. **A presenter's mic in an act, Doh'd, the reverb ringing, then GO**: the tail cut, the gate open
    over a tenth of a second. A mic scene Doh'd and GO'd inside the Doh fade: the scene seated
    again (L6), its mic's gate opening over the de-click on its channel - nothing doubled, nothing
    clicking. A mic a stop cue was fading, Doh'd: its gate open again.
27. **A bed the GO stopped dead, Doh'd two seconds later**: back at its second, fading in over the
    panic fade.
28. **A scene stopped with a long footer**: put back once its footer has ended, and the report says
    so, then again when it comes back.

On a real desk:

29. **A console that quantises (a motor fader).** A GO too soon moves it; Doh!: the fader back where
    it was before the GO. Moved by a hand after the GO, then Doh!: left where the hand put it, and the
    report says "changed since the GO".
30. **A lighting desk at its default ("Meh").** A scene of OSC cues to it caught part-way by a Doh:
    the desk keeps what it had, nothing is sent at the Doh, the corrected GO runs the rest on its
    clock and sends nothing it already had; the report names the desk on the Doh! line, also while
    the focus sits on another list. And a scene caught before anything sounded: the desk keeps what
    it had, and the corrected GO runs the rest on its clock.
31. **F9 with the interface unplugged**: the line says the pointer is back and what it puts back
    comes when the audio returns; plugged back in, the report replaces it.
32. **The D700's Doh! binding** waits for a bench session of its own - a button of its own, never a
    double click of PLAY, which is the very fault Doh! mends (ruling 9, HA).

On a machine where the tick keeps time:

33. **`blackbox.doh` in a Release build, or on the Mac mini.** On the Windows laptop (Core Ultra 7
    255H) a Debug build's tick thread falls seconds behind its audio once Windows moves it to the
    slow cores, and the driver voids, in words, the readings the tick places: where the corrected
    GO's sound lands in its file, the Doh fade's shape, the de-click's rise, the bed's fade-in
    (namespace draft §24.15, OR). Run where the tick keeps time, none should say `void`: the
    corrected GO lands within a tenth of a second of the press's second, and the de-click rises over
    about a tenth of a second.
