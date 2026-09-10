# Asparion D700 — measured protocol

**Draft 0.1, updated 2026-09-10.** Everything here was measured on hardware
between 2026-09-07 and 2026-09-10: an **Asparion D700 Rack, 16 faders, with
display modules**, on Windows 11, using MIDI probes, `hidapi` and USBPcap bus
captures.

**What changed on 2026-09-10.** Full RGB colour works over MIDI, so **the
vendor HID interface is no longer needed for anything a Go.dot profile does**.
The display has three rows per strip, not two, through Asparion's own native
commands. Metering and 0–127 encoder rings were confirmed. All of it came from
reading Asparion's published Bitwig control script and confirming it on the
unit.

This document serves **PRD §3.16** (control surfaces) and closes most of **§6.4**
(Asparion — remaining asks). §6.4 proposed requesting a byte-level list of the
Mackie extensions from the vendor, on the grounds that reverse-engineering four
DAW packages was "an afternoon better spent elsewhere". That reverse-engineering
has since happened on hardware instead, which is stronger evidence than a DAW
package would have been: it says what the device *does*, not what one integration
assumes.

**Provenance.** The raw session log, with every finding tagged by how it was
established and four retractions recorded rather than edited away, lives in the
S21-HiJack repository as `Documentation/D700_FIELD_NOTES.md`. Its distillation,
the byte-level **recipe book**, is in this repository as
[`D700_CONTROL_GUIDE.md`](D700_CONTROL_GUIDE.md): setup, addressing, every
message in and out, and the gotchas ranked by cost. This file is the
**Go.dot-facing interpretation** — what the measurements mean for PRD §3.16 and
for a device profile. Where the three disagree, the field notes are the record
of evidence.

**Reading convention:** *(unverified)* marks something inferred but not measured.
Everything unmarked was observed directly.

---

## Verdict summary

| Question | Answer |
|---|---|
| Can Go.dot drive the D700 with no vendor software? | **Yes, over MIDI alone** — faders, touch, encoders, buttons, LEDs, colour, rings, displays and meters |
| Is the vendor HID interface needed? | **No.** It wins one bit of colour resolution and loses on everything else (§2.2) |
| Is the Connector needed? | **No.** Only for its OSC bridge, and it holds the HID interface exclusively while running |
| Does a cross-platform path exist? | **Yes.** USB-MIDI is class-compliant, and it now carries everything |
| Encoder RGB, byte level (§6.4) | **Yes, over MIDI** — note-on on channels 2, 3 and 4 at the element's own button note, 0–127 per component, physical addressing, no provisioning (§4) |
| Display lines, byte level (§6.4) | **Yes** — three rows per strip, 12 + 12 + 8 characters, plus a track-number field, through Asparion's native SysEx (§4). The MCU path's two rows of 56 remain as compatibility |
| Does Mackie-first still hold (§6.10)? | **Yes**, and the D700 profile is now MIDI-only (§5) |

---

## 1. Topology

USB composite device, `VID_04D8` (Microchip) `PID_E44E`, serial `D700RTB12017`.

| Interface | Class | Role |
|---|---|---|
| `MI_00` | HID, vendor-defined (`usage_page 0xff00`) | the Configurator's and Connector's private channel |
| `MI_01` | Audio/MIDI | class-compliant USB-MIDI |

**16 faders arrive as two banks of eight.** Over USB-MIDI they are two cable
numbers, which Windows presents as two port pairs (`D 700` and
`MIDIIN2/MIDIOUT2 (D 700)`); each bank numbers its own faders 1–8, so the
pitch-bend channel alone does not identify a physical fader — the port does.
Each bank identifies itself in the MCU handshake: **bank 1 as device id `0x14`,
bank 2 as `0x15`** (Mackie Control and Extender).

Over HID both banks arrive on **one endpoint**, distinguished by a port byte.

**Finding the ports.** Match the device by the name substring `D 700`, and take
the one containing `MIDIIN2`/`MIDIOUT2` as bank 2. Never rely on WinMM port
numbering, which renumbers when other USB devices come and go. Windows MIDI
input is exclusive: a DAW or a Max patch holding the ports means Go.dot sees
nothing, so a failed open is reported as *held by another application* and not
as a silent surface. The Asparion applications do not hold the MIDI ports, but
the Configurator has been seen overriding colour writes, so a show machine runs
without it. See the control guide §1.1 and §1.3.

---

## 2. Transports

Three exist. §3.16's transport list — *(MCU / HUI / raw MIDI / OSC / HID)* —
already anticipates all of them.

### 2.1 MCU over USB-MIDI — the recommended path

Vendor-recommended, documented, portable to other MCU surfaces. The D700 is a
textbook MCU implementation; a 90-second capture of the entire master section
produced **zero unmapped note numbers**.

| Control | Message |
|---|---|
| Faders 1–8 (per bank) | pitch bend `E0`–`E7`, 14-bit |
| Fader touch | note `0x68`–`0x6F`, channel 1 |
| Encoders | CC `0x10`–`0x17` |
| V-Pot press | note `0x20`–`0x27` |
| Rec / Solo / Mute / Select | notes `0x00`/`0x08`/`0x10`/`0x18` + strip |
| Pan / EQ / Send / FX | notes `0x2A` / `0x2C` / `0x29` / `0x2B` |
| Bank arrows | notes `0x2E` / `0x2F` |
| Transport (Rec/Play/Stop) | notes `0x5F` / `0x5E` / `0x5D` |
| Volume knob | pitch bend channel 9 (MCU master fader) |
| Encoder ring **position** | `B<mode> <0x30+n> <0..127>` — the MIDI channel selects the mode: 0 none, 1 fill from centre, 2 fill from the left. The MCU form, mode in the value's high nibble and 11 positions, also works |
| Button LEDs | host echoes note-on; the surface has **no local feedback** |
| Display text | native SysEx `0x1A` rows 1–2, `0x19` row 3, `0x17` track numbers (§4); MCU `0x12` as compatibility |
| **Colour** | note-on on channels 2, 3, 4 at the element's button note, velocity = red, green, blue, 0–127, **blue last** (§4) |
| Metering | channel pressure `D0 <(strip << 4) \| level>`, 12 levels |

**Encoders are sign-magnitude, not two's complement.** Values 1, 2, 3 clockwise
and 65, 66, 67 counter-clockwise — 65 means −1. A two's-complement decoder reads
that as **−63**. This is the most likely implementation bug in any new
integration, because two's complement is the usual MCU assumption.

**Device id is permissive.** `0x10`, `0x11`, `0x14` and `0x15` are all accepted
for display writes, though each bank *reports* its own in the handshake. Use
`0x14`.

**The MCU connection handshake works**, challenge-response included:

```
device -> host   F0 00 00 66 14 01 <7-byte serial> <4-byte challenge> F7
host   -> device F0 00 00 66 14 02 <serial>        <4-byte response>  F7
device -> host   F0 00 00 66 14 03 <serial> F7        accepted
```

Nothing requires it — every control works without a host connection — but it
yields the serial, a stable identifier that survives USB renumbering.

**MIDI needs no pacing.** A full-surface chase — faders, LEDs, rings and
displays across both banks — needed no throttling at all (field notes 11). The
only reason to slow output to this surface is motor end-stop wear (§6), never
throughput. The byte tables for every message are in the control guide §2–§4.

### 2.2 Vendor HID — the raw surface, and no longer needed

*Superseded for Go.dot on 2026-09-10.* Everything in this section still holds,
but colour was HID's only reason to be in a Go.dot profile, and colour now works
over MIDI (§4). Against MIDI, HID wins one bit of colour resolution and both
banks on one handle. It loses physical addressing, needs the surface
provisioned, has no read-back, is undocumented, must be paced, and is held
exclusively by the Connector. It stays documented because it is the raw
protocol the MIDI interface translates, and the only window onto the
configuration block (§7).

Framed `<length> 2a <command>` on interrupt endpoints `0x01` (out) / `0x81` (in).
The leading byte is a HID **report ID**, and it partitions the protocol:

| Report | Size | Carries |
|---|---|---|
| `0x04` | 5 bytes | realtime control, in **and** out |
| `0x08` | 9 bytes | short commands, status, colour |
| `0x20` | 33 bytes | configuration pages |

**Report `0x04` is `04 <port> <3 MIDI-style bytes>`**, `<port>` being `00` or `01`
for the two banks. It carries the **raw** surface protocol; the MIDI interface is
a *translation* of it:

| Control | HID (native) | MIDI (MCU-mapped) |
|---|---|---|
| Faders | pitch bend `E0`–`E7` | identical |
| Fader touch | note `0x30`–`0x37` | note `0x68`–`0x6F` |
| Encoders | CC `0x14`–`0x1B` | CC `0x10`–`0x17` |
| Volume knob | CC `0x03` | pitch bend channel 9 |

Writing report `0x04` drives **motor faders** (`04 <port> E<n> <lsb> <msb>`) and
**button LEDs** (`04 <port> 90 <note> <on>`), both confirmed on hardware.

**Colour is report `0x08`** — the byte-level answer §6.4 wanted:

```
08 2a 2b 29 2c 28 00 00 00              open session (device acks 08 2a 2b 00 ...)
08 2a 0a b6 <index> 00 <R> <G> <B>      set colour — full 8 bits per channel
```

Element class `b6` is the dials. `b0` returned a write stall and was not pursued.

**But the index is a configuration slot, not a physical position.** Sweeping
`0x00`–`0x11` under `b6`, with a session properly opened, lit **only the three
elements that had been provisioned** in the Configurator — the other fourteen
dials stayed dark at every index. Sweeping the same range under `a2` and `b2`
lit nothing at all.

So on an unprovisioned unit, most indices address nothing, and **`index N`
cannot be assumed to be dial N**. No read-back command is known, so a host
cannot discover the mapping at runtime; provisioning the surface and recording
the index order becomes a documented prerequisite of the device profile.

**The firmware also reclaims the LEDs**, whichever route painted them — see §4.

Verified by a 40-second hue rotation: 511 steps, 1533 writes, three elements
chasing, smooth throughout, with the Configurator and Connector both closed.

### 2.3 OSC via the Asparion Connector — not a deployment path

Works, and is capable: with an operator-authored template, OSC drives motor
faders and full RGB including the master dial. But the template ships **empty**,
the Connector **holds the HID interface exclusively** while running, and it does
not exist on Linux, Android or iOS — §3.16's stated reason for Mackie-first, and
still correct. Useful for bench work only.

---

## 3. Endpoint classes (§3.16)

| §3.16 class | On the D700 |
|---|---|
| **absolute** | 16 motor faders (touch-sensitive) + the volume knob, which reports as a fader |
| **relative** | 16 encoders — **sign-magnitude** |
| **gate** | ~76 buttons: 4 per strip, plus the master section — but see *double-click* below |
| **rate** | none |

**The volume knob is an absolute endpoint, not a relative one.** It reports
14-bit pitch bend on MCU channel 9, and has **no touch sense** — MCU convention
would place it at note `0x70`, which the device never sends — so it gets no touch
gating.

**Touch is necessary, not sufficient.** Faders next to buttons register real
touches when the operator reaches past them: in one capture, 58 of 81 touch
events landed within 150 ms of a nearby button press (control guide §3.1; the
capture is not in the field notes). That meets two PRD requirements
differently:

- **§3.16's touch state gating outbound updates** would freeze those faders
  every time somebody reaches for the master section — a recall or a fade that
  leaves the faders beside it where they were. *(proposed)* A touch counts as a
  human adjusting only once the position has moved past the hysteresis §3.9a
  already needs, and the engine resends its value on release either way.
- **§3.9a's fader-stop** is unharmed. It keys on the release at −∞, and a
  reach-past touch moves nothing; at worst it defers a stop by the length of
  the reach. Fader-start needs the fader to leave −∞, which a touch alone never
  does.

**Double-click changes a button's class.** Asparion's firmware can emit a *second* note on a
double click — on the `*` (magic) button, F1 (`0x36`) single, F2 (`0x37`) double. It is off by
default and enabled per button by a checkbox in the Configurator. It is **suppressive**: the
single click is withheld until the gesture resolves, and a double emits F2 *alone*, never F1
then F2.

That gives three timing regimes, and a profile needs to know which applies:

| Case | What is precise |
| --- | --- |
| Double-click **off** | the **down stroke** — real switch timing |
| Double-click **on**, deliberate press | the **release** — resolves early, real duration |
| Everything else | cooked by firmware, with latency |

The middle case follows from the firmware being sensible: a double click is two *short* clicks,
so a press held past the short-click threshold cannot be half of one and resolves immediately.
Measured holds of 0.9–2.2 s reported their genuine duration; taps reported 0–70 ms. So
**reported duration = real hold − resolve delay**.

It is **preset-independent** — basic MIDI behaved identically to Mackie — so the processing sits
below the protocol layer and no host-side choice avoids it. The raw switch is not visible over
MIDI at all.

Three consequences for §3.16:

1. **The gate class needs a per-button mode.** With double-click on, the same physical button is
   no longer reliably press-and-release on the down stroke. A device profile should carry it.
2. **Bind on release** where timing matters. The release is a real event under the operator's
   control; the down stroke is not, once double-click is on.
3. **Double-click is for escalation, never alternation.** Panic → Hard Panic works, and maps
   neatly onto §4.4's Esc / double-Esc. **Go → Undo Go does not**: §4.5 is explicit that undo of a
   GO is *revert*, and the audio has already escaped — so a double click would fire the cue and
   then revert it in front of an audience. Any binding editor offering double-click should refuse
   pairs where the second action reverses the first.

The latency is a design input rather than a disqualifier. A graceful panic that fades can absorb
0.4 s; a GO cannot (§4.1). Which buttons carry double-click is a layout decision with a cost
attached.

---

## 4. Display as a renderable (§3.16)

**The MCU `0x12` path is a compatibility shim.** Asparion's own published
Bitwig script uses a native display protocol that is roughly twice as large:

```
rows 0,1:   F0 00 00 66 14 1A <pos> <row+1> <12 chars> F7    pos = strip * 12
row 2:      F0 00 00 66 14 19 <pos> <8 chars>          F7    pos = strip * 8
track no:   F0 00 00 66 14 17 00 <8 bytes>             F7
```

| | MCU `0x12` | native |
| --- | --- | --- |
| Rows | 2 | **3** |
| Characters per strip | 14 | **32** (12 + 12 + 8) |
| Track number | steals from a row | **its own field** |

For §3.16's "layout chooses which fields go on which line, per strip", that is a
materially larger budget: an unabbreviated name, a full value with units, and a
tag row, with the channel number in a dedicated field. The 7-character
authored-short-name constraint applies only to the MCU path.

PRD §3.16 describes the D700S OLED as *2 × 12 chars + 1 × 6*. The native
command's third-row field is **eight** characters (`SINGLE_DISPLAY_WIDTH_THIRD
= 8` in Asparion's script), confirmed on the unit, so §3.16's six should read
eight when it is next amended. Pad every write to the full field width.

**Metering and rings exist too.** VU is standard MCU channel pressure,
`D0 <(strip<<4)|level>`, 12 levels, `0x0F` resets peak hold. Asparion's own
script sends it at 5 fps and only while the transport plays; 18 fps was smooth
here, and 5 is a sensible default. Encoder rings are `B<mode> <0x30+n> <0..127>`
— the MIDI channel selects the display mode (0 none, 1 pan-from-centre, 2
fill-from-left) and the value is **0–127**, not MCU's 11 positions.

The MCU-compatible view of the display remains **two rows of 56 characters per
bank**, addressed by offset: `0x00` upper, `0x38` lower. Each row is eight
strips of seven characters, at offsets `0x00`, `0x07`, `0x0E`, `0x15`, `0x1C`,
`0x23`, `0x2A`, `0x31`.

Across a 16-fader rack that is **224 characters** through `0x12`, against
**512** through the native commands — 16 strips × 32. The MCU view is what a
generic MCU profile gets, and what the D700 profile should not use.

**The buffer is flat, not per-field.** A write replaces only the bytes sent, so a
six-character label at offset 0 leaves character 7 holding whatever was there
before. **Always pad a strip write to seven characters.** Observed directly:
writing `PORT-1` after `S21 HIJACK` rendered as `PORT-1J`.

**Colour has three routes, and one of them is right.** It matters for §3.16's
"colour is never the sole carrier" — it is a rich channel, so the temptation
will be real:

| Route | Resolution | Reaches |
|---|---|---|
| MCU SysEx `0x72` | 8 colours; 3-bit RGB (bit 0 red, 1 green, 2 blue) | strips only — never the real colour interface |
| **Vendor note-on over MIDI** | **7-bit per channel** | **all 17 RGB elements, by physical position** |
| Vendor HID `0a b6` | 8-bit per channel | only the elements provisioned in the Configurator |

**The MIDI method is the one to use**, and it is documented in Asparion's own
published Bitwig control script (`Dxxx_encoders.js`):

```
91 <0x20+n> <r>      channel 2, velocity = red   (0..127)
92 <0x20+n> <g>      channel 3, velocity = green
93 <0x20+n> <b>      channel 4, velocity = blue, triggers the refresh
```

`0x20` is `VPOT_CLICK0`, the MCU V-Pot press note; `n` is 0–7 within a bank; and
the **bank is selected by which MIDI port the message is sent to**. Confirmed on
hardware across all sixteen encoders. Components are 0–127, so 8-bit colour is
halved. **Blue must be sent last**: the element refreshes only when blue
arrives.

It is **note-on, not CC** — the colour component travels as the velocity byte.
That distinction cost this project two days: Asparion's prose description ("the
midi code listed in the configurator", "on midi channel 1 2 3") reads equally
well as a CC scheme, and thirty-two CC-based probes found nothing.

Note-on on **channel 1** at the same note is the ordinary V-Pot LED state, so
colour and lit-state are independent.

**The rule generalises: an element's colour note is its own button note.** The
encoders use `0x20`+n because that is their V-Pot press note; the master dial
uses `0x38`, its knob-press note. No special cases.

**But only 17 elements have RGB hardware.** Painting the whole note space in
coloured blocks coloured the rotaries and the master dial only — every other
button is a **single-colour LED**, driven the ordinary MCU way on channel 1:

| Elements | Count | Colour |
| --- | --- | --- |
| Encoders | 16 | full RGB, 128 levels per channel |
| Master dial | 1 | full RGB |
| All other buttons | ~59 | on/off, single colour |

**Colour is preset-independent** — identical under Mackie and Universal — which
matters because the button *map* is not (the `*` button moves between `0x36`
and `0x5A`). That makes colour the most robust capability on the surface, and
the one a layout can rely on without pinning a preset.

For §3.16's "colour is never the sole carrier": the constraint is easy to
honour here, because there are only 17 colour-bearing elements against 16
strips. Colour can carry channel *type* on the encoder while text carries
identity, and the ~59 monochrome buttons cannot carry colour meaning at all.

**A strip's colour is its encoder's.** PRD §3.30 has a strip's colour follow
the timbre of the clip it carries. On this surface the only RGB element on a
strip is the encoder surround above the fader, so that is where §3.16's colour
cell lands. The fader and its four buttons cannot carry it.

**Prefer this over the HID route.** MIDI addresses **physical positions**, needs
no provisioning and no read-back, and is vendor-documented, so it should survive
firmware updates. HID's only advantage is one extra bit per channel, which
against 128 levels is invisible.

**Encoder rings are monochrome position indicators.** The RGB element is the
strip / knob surround. Ring position and colour are different things.

**The firmware reclaims the LEDs.** The device runs its own idle animation —
all dial RGB cycling smoothly together — which resumes when nothing is driving
the surface, whichever route painted the colour (field notes 35b; control
guide §4.4). Colour is therefore a matter of **ownership**, not only
addressing: a host that paints a colour and stops will have it taken back. No
command disabling the animation is known; it is probably a setting in the
configuration block (§7). Until one is found, a profile **re-asserts** colour.
PRD §3.30's timbre binding does so anyway while a clip sounds, and at idle the
profile repaints the authored colour on a timer.

**What colour costs on the wire.** Three three-byte messages per element, and
seventeen elements, so repainting all of them ten times a second is 510
messages a second. Nothing measured says that is too many, since a full-surface
chase needed no throttling. Nothing measured says it is fine either: the MIDI
hue rotation that confirmed the route did not record its rate. So PRD §6.11's
colour write-rate measurement survives, now over MIDI rather than HID.

---

## 5. Device profile implications (§3.16)

A D700 profile is **topology + protocol**. The topology is unambiguous: 16
strips, each with a fader and touch, an encoder with an RGB surround and a
0–127 ring, 4 single-colour buttons, three display rows of 12 + 12 + 8
characters with a track-number field, and a 12-level meter; plus a master
section of ~13 gates, one absolute knob, and the RGB master dial.

The **protocol** choice was a real fork until 2026-09-10. It no longer is:

| | MCU/MIDI with Asparion's extensions | Vendor HID |
|---|---|---|
| Documented | **yes** — Asparion's published scripts | no |
| Vendor-recommended | **yes** | no |
| Portable to other surfaces | **yes**, for everything but the extensions | no |
| Addressing | **physical position** | colour by configuration slot |
| Colour | **7-bit, all 17 RGB elements, no provisioning** | 8-bit, provisioned elements only |
| Display | **three rows and a track number** | not measured |
| Both banks on one handle | no — two port pairs | yes |
| Button map preset-independent | no — the `*` button moves | yes |
| Pacing needed | **none** | ~25 ms between writes |
| Survives a firmware update | **yes** | unknown |

**Mackie-first stands** (§6.10), **and the D700 profile is MIDI-only.** It is
documented, vendor-recommended, and the same profile machinery serves an
X-Touch or a FaderPort. The D700's extensions — colour, the native display, the
fine rings — are a layer on top of MCU in the same transport rather than a
second protocol. HID leaves the profile.

**Preset drift is a real hazard for the MCU path.** Between the Mackie and Reaper
presets, exactly **one** control moves — the `*` button, note `0x36` under Mackie
and `0x5A` under Reaper. Everything else is byte-identical. That is precisely the
shape of a silent failure: a binding learned under one preset keeps working for
every control except one, with no error. **A profile should name the preset it
expects**, and the app should say so rather than auto-detect: a surface 97%
identical across presets cannot be fingerprinted from traffic. Everything else
the profile uses — colour, the native display, rings and meters — behaves
identically under Mackie and Universal, so the button map is the only thing the
preset pins. The control guide confirms all of it under **Mackie** (preset 2),
which is the one to name.

---

## 6. Hazards

**Do not sweep undocumented SysEx command bytes.** A sweep of `0x10`–`0x7F`,
already excluding the documented-destructive `0x0A`–`0x0F` and `0x61`–`0x63`, put
the display modules into a logo-only state and **required a full restart of the
controller**. The exclusions were not sufficient. Seven commands are
established as safe: `0x12` (MCU text), `0x1A`, `0x19` and `0x17` (the native
display rows and track numbers, from Asparion's own script), `0x72` (coarse
colour), `0x00` (device query) and `0x02` (handshake reply).

**Do not command full fader travel.** Driving a fader to either end from the far
end hits the physical stop at full speed with no deceleration. The bottom of the
range is not an edge case — a parked channel lives there — so any sync sweep or
cue recall moving a fader from unity to −∞ commands exactly that. Interpolate
large jumps; roughly 20 steps was smooth. Do **not** clamp short of the
endpoints: a fader that cannot reach −∞ misrepresents the desk, which is worse
than the wear.

**Pace HID writes** — a concern for bench tools only, now that no Go.dot
profile uses HID. Three issued back to back returned `device not functioning`;
spacing them ~25 ms apart ran 1533 writes without a stall. MIDI needs no such
pacing.

For context on the first: the surface is otherwise fast. A full-surface chase —
faders, LEDs, rings and displays across both banks — needed no throttling at all,
and the operator reports the D700's faders as **faster and more consistently
responsive than an S21's own**.

---

## 7. What remains unknown

1. ~~The CC number for the vendor's MIDI RGB method.~~ **Resolved** — it is not a
   CC at all but note-on on channels 2/3/4 at the element's button note, per
   Asparion's published Bitwig script. See §4. The MIDI path to full colour is
   complete, which settles PRD §3.30's timbre on the strip: it goes over MIDI at
   7 bits per channel, and nothing in a Go.dot profile needs HID.
2. **HID element classes `a2`, `b0`, `b2`.** `b6` is the dials. `b0` stalled the
   device and was left alone. Moot for Go.dot since colour moved to MIDI, and
   kept for completeness.
3. ~~Whether the D700S OLED module uses the same scribble path.~~ **Resolved** —
   it does not need to. The third line was never unreachable: `0x12` addresses
   two rows of 56 and nothing more, and Asparion's native commands `0x1A`,
   `0x19` and `0x17` reach all three rows and the track number (§4). The
   measured third row is eight characters where PRD §3.16 says six.
4. **Linux and macOS behaviour.** Everything here was measured on Windows. The
   MIDI interface is class-compliant, so it *should* carry over *(unverified)*,
   and with HID out of the profile no udev rule for `hidraw` is needed. What is
   likely to differ is the **port names** the profile matches on (§1), which
   ALSA and CoreMIDI present in their own ways.
5. **Whether the config block is safely writable.** The Configurator changes any
   setting by reading all 86 pages and writing all 86 back — 24 bytes each on
   the wire, the last carrying 8, so the block is **2048 bytes**. Whether a
   single targeted page write is accepted was not tested; the failure mode is a
   corrupted device configuration. The block can be **studied without the
   hardware**: Asparion's published `.aPres` preset files are exactly that
   2048-byte block in hex (field notes 36a), so a setting can be located by
   diffing presets offline before anything is written to a unit.
6. **What disables the idle animation, and how soon it resumes.** The first
   decides whether a profile must re-assert colour at all; the second sets the
   interval it re-asserts at (§4). The switch is probably in the configuration
   block, which makes item 5 the way to find it.
7. **How to tell a reach-past touch from a grab** (§3). The control guide gives
   the problem and one capture's statistic; the filter proposed in §3 is
   untested on the unit.

---

## 8. What this closes in §6.4

| §6.4 ask | Status |
|---|---|
| Byte-level Mackie extensions — encoder RGB | **Answered** — MIDI note-on on channels 2/3/4, 7-bit, physical addressing (§4). HID's 8-bit route is documented and not used (§2.2) |
| Byte-level — OLED lines | **Answered** — three rows of 12 + 12 + 8 per strip and a track-number field through the native commands (§4); `0x12`'s two rows × 56 as compatibility |
| Demo mode | Still worth pushing for. Note a **profile can now be developed without hardware** against the maps here and in the control guide |
| Multi-DAW / multiple simultaneous MIDI endpoints | Not investigated. The device already presents **two** MIDI port pairs for its two banks |

The vendor was asked directly about encoder colour and replied in prose — "the
midi code listed in the configurator", "on midi channel 1 2 3" — that reads
equally well as a CC scheme, which is why thirty-two CC probes found nothing.
The bytes in §4, like the native display commands and the ring modes, come from
Asparion's published Bitwig control script, each confirmed on the unit.
Everything else here is our own measurement.
