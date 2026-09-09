# Asparion D700 — measured protocol

**Draft 0.1.** Everything here was measured on hardware between 2026-09-07 and
2026-09-08: an **Asparion D700 Rack, 16 faders, with display modules**, on
Windows 11, using MIDI probes, `hidapi` and USBPcap bus captures.

This document serves **PRD §3.16** (control surfaces) and closes most of **§6.4**
(Asparion — remaining asks). §6.4 proposed requesting a byte-level list of the
Mackie extensions from the vendor, on the grounds that reverse-engineering four
DAW packages was "an afternoon better spent elsewhere". That reverse-engineering
has since happened on hardware instead, which is stronger evidence than a DAW
package would have been: it says what the device *does*, not what one integration
assumes.

**Provenance.** The raw session log, with every finding tagged by how it was
established and four retractions recorded rather than edited away, lives in the
S21-HiJack repository as `Documentation/D700_FIELD_NOTES.md`. This file is the
Go.dot-facing summary. Where the two disagree, the field notes are the record of
evidence and this is the interpretation.

**Reading convention:** *(unverified)* marks something inferred but not measured.
Everything unmarked was observed directly.

---

## Verdict summary

| Question | Answer |
|---|---|
| Can Go.dot drive the D700 with no vendor software? | **Yes** — over MCU/MIDI, or over the vendor HID interface |
| Is the Connector needed? | **No.** Only for its OSC bridge, and it holds the HID interface exclusively while running |
| Does a cross-platform path exist? | **Yes.** MIDI is class-compliant; HID is reachable through `hidapi` on Linux, macOS and Windows |
| Encoder RGB, byte level (§6.4) | **Two routes, both below** |
| Display lines, byte level (§6.4) | **Yes** — two rows of 56 characters |
| Does Mackie-first still hold (§6.10)? | **Yes**, for reasons this document strengthens |

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
| Encoder ring **position** | CC `0x30`–`0x37`; mode in the high nibble, position in the low |
| Button LEDs | host echoes note-on; the surface has **no local feedback** |
| Display text | SysEx `F0 00 00 66 <id> 12 <offset> <text> F7` |
| Colour, coarse | SysEx `F0 00 00 66 <id> 72 <8 bytes> F7` |

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

### 2.2 Vendor HID — richer, D700-only

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

Element class `b6` is the dials. `a2`, `b0` and `b2` also appear in the
Configurator's traffic and address other classes *(unverified: which)*. `b0`
returned a write stall and was not pursued.

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
| **gate** | ~76 buttons: 4 per strip, plus the master section |
| **rate** | none |

**The volume knob is an absolute endpoint, not a relative one.** It reports
14-bit pitch bend on MCU channel 9, and has **no touch sense** — MCU convention
would place it at note `0x70`, which the device never sends — so it gets no touch
gating.

---

## 4. Display as a renderable (§3.16)

**Two rows of 56 characters per bank**, addressed by offset: `0x00` upper, `0x38`
lower. Each row is eight strips of seven characters, at offsets `0x00`, `0x07`,
`0x0E`, `0x15`, `0x1C`, `0x23`, `0x2A`, `0x31`.

Across a 16-fader rack that is **224 characters**, as 16 strips × 7 chars × 2
rows — which fits §3.16's "layout chooses which fields go on which line, per
strip" exactly, with two lines to choose between.

**The buffer is flat, not per-field.** A write replaces only the bytes sent, so a
six-character label at offset 0 leaves character 7 holding whatever was there
before. **Always pad a strip write to seven characters.** Observed directly:
writing `PORT-1` after `S21 HIJACK` rendered as `PORT-1J`.

**Colour has two resolutions**, which matters for §3.16's "colour is never the
sole carrier" — it is a rich channel, so the temptation will be real:

| Route | Resolution |
|---|---|
| MCU SysEx `0x72` | 8 colours; 3-bit RGB (bit 0 red, 1 green, 2 blue) |
| Vendor extension over MIDI | 7-bit per channel — vendor's own method |
| Vendor HID `0a b6` | **8-bit per channel** |

The vendor's MIDI method, in Asparion's words: *"Use the midi code listed in the
configurator for that encoder. Then send r g b values divided by 2 -> 0-127, on
midi channel 1 2 3. It will only refresh after you sent the last one, blue. You
can turn it on/off without changing the colour on the first channel, 0 resp 1."*

**The CC number for that method is not yet known** — see §7. Thirty-two
candidates across `0x10`–`0x17` and `0x30`–`0x37` produced nothing, unsurprising
since both blocks already carry V-pot input and ring position. The Configurator's
**Encoder / LED** tab lists it.

**Encoder rings are monochrome position indicators.** The RGB element is the
strip / knob surround. Ring position and colour are different things.

---

## 5. Device profile implications (§3.16)

A D700 profile is **topology + protocol**. The topology is unambiguous: 16
strips, each with fader + touch + encoder + 4 buttons + 2 display rows + colour;
plus a master section of ~13 gates and one absolute knob.

The **protocol** choice is a real fork, and it belongs in the profile:

| | MCU/MIDI | Vendor HID |
|---|---|---|
| Documented | yes | no |
| Vendor-recommended | **yes** | no |
| Portable to other surfaces | **yes** | no |
| Both banks on one handle | no — two port pairs | **yes** |
| Preset-independent | no | **yes** |
| Colour resolution | 3-bit, or 7-bit via extension | **8-bit** |
| Survives a firmware update | **yes** | unknown |

**Mackie-first stands** (§6.10). It is documented, vendor-recommended, and the
same profile machinery serves an X-Touch or a FaderPort. HID is a D700-specific
enhancement, best treated as an optional protocol *within* the D700 profile
rather than as the primary path.

**Preset drift is a real hazard for the MCU path.** Between the Mackie and Reaper
presets, exactly **one** control moves — the `*` button, note `0x36` under Mackie
and `0x5A` under Reaper. Everything else is byte-identical. That is precisely the
shape of a silent failure: a binding learned under one preset keeps working for
every control except one, with no error. **A profile should name the preset it
expects**, and the app should say so rather than auto-detect: a surface 97%
identical across presets cannot be fingerprinted from traffic.

---

## 6. Hazards

**Do not sweep undocumented SysEx command bytes.** A sweep of `0x10`–`0x7F`,
already excluding the documented-destructive `0x0A`–`0x0F` and `0x61`–`0x63`, put
the display modules into a logo-only state and **required a full restart of the
controller**. The exclusions were not sufficient. Only four commands are
established as safe: `0x12` (text), `0x72` (colour), `0x00` (device query),
`0x02` (handshake reply).

**Do not command full fader travel.** Driving a fader to either end from the far
end hits the physical stop at full speed with no deceleration. The bottom of the
range is not an edge case — a parked channel lives there — so any sync sweep or
cue recall moving a fader from unity to −∞ commands exactly that. Interpolate
large jumps; roughly 20 steps was smooth. Do **not** clamp short of the
endpoints: a fader that cannot reach −∞ misrepresents the desk, which is worse
than the wear.

**Pace HID writes.** Three issued back to back returned `device not functioning`;
spacing them ~25 ms apart ran 1533 writes without a stall.

For context on the first: the surface is otherwise fast. A full-surface chase —
faders, LEDs, rings and displays across both banks — needed no throttling at all,
and the operator reports the D700's faders as **faster and more consistently
responsive than an S21's own**.

---

## 7. What remains unknown

1. **The CC number for the vendor's MIDI RGB method.** The Configurator's
   Encoder / LED tab lists it. The one item that would complete the MCU-only path
   to full colour, and a thirty-second read rather than a research task. It
   matters more since PRD §3.30 (2026-09-09): a strip whose colour follows a
   running clip's timbre needs full-depth colour, and on a machine where
   something else holds the HID interface this CC is the only route.
2. **HID element classes `a2`, `b0`, `b2`.** `b6` is the dials. `b0` stalled the
   device and was left alone.
3. **Whether the D700S OLED module** (§3.16: 2 × 12 chars + 1 × 6, track number,
   metering) uses the same scribble path. The unit measured has display modules
   showing **more than two physical lines**, but only two are reachable through
   SysEx `0x12` — its buffer caps at 128 characters and two rows of 56 consume
   112. The extra lines exist and are not addressable by that command.
4. **Linux behaviour.** Everything here was measured on Windows. The device is a
   standard composite device and `hidapi` is cross-platform, so both transports
   *should* carry over *(unverified)*. Linux needs a udev rule for `hidraw`
   access; there is no Connector to contend with, so the exclusivity problem
   disappears.
5. **Whether the config block is safely writable.** The Configurator changes any
   setting by reading all 86 pages (24 bytes each, 2064 total) and writing all 86
   back. Whether a single targeted page write is accepted was not tested; the
   failure mode is a corrupted device configuration.

---

## 8. What this closes in §6.4

| §6.4 ask | Status |
|---|---|
| Byte-level Mackie extensions — encoder RGB | **Answered** — two routes (§2.2, §4); the MIDI CC number is the only gap |
| Byte-level — OLED lines | **Answered for the scribble path**: two rows × 56. The D700S module's own lines remain open (§7.3) |
| Demo mode | Still worth pushing for. Note a **profile can now be developed without hardware** against the maps here |
| Multi-DAW / multiple simultaneous MIDI endpoints | Not investigated. The device already presents **two** MIDI port pairs for its two banks |

The vendor was asked directly about encoder colour and replied with the MIDI
method quoted in §4. That exchange is the source for that paragraph; everything
else here is our own measurement.
