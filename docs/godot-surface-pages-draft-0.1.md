# Go.dot — surface pages and the programming workflow

**Draft 0.1, 2026-09-23.** A design conversation with the author, written down at his request on
the day it happened, the afternoon before the first bench session with the D700. **Nothing in this
document is built** beyond what Phase 6 already built (namespace draft §16), and it amends no PRD
section yet: what the author said is recorded as said, what is only proposed is marked
*(proposed)*, and the questions still his to answer are in §9, each with a recommendation.

**Order of work, as the author set it:** the per-cue **EQ and VST inserts are implemented first,
in another session**; the surface pages come after, on top of them. §8 is what the pages will need
from that work, written so that session can provide it without having to guess.

This document serves PRD §3.16 (control surfaces) and §3.18 (plugin hosting), and builds on the
measured protocol (`docs/godot-asparion-d700-protocol-0.1.md`), the byte-level recipe book
(`docs/D700_CONTROL_GUIDE.md`) and what Phase 6 built (`docs/godot-namespace-draft-0.1.md` §16).

---

## 1. Why

Phase 6 built **one page**: the show's layout. Every strip is a sampler strip or a DCA strip, a
fader rides a sample's level or a DCA's trim, a touch starts a sample, PLAY is GO and STOP is Esc.
That is the right surface for running a show and a poor one for building it.

The author wants the surface to serve the **programming** half of a show's life as well — riding a
cue's sends, placing it, shaping its EQ and its plugins with the rotaries, pressing shortcuts —
and wants the same thinking to reach the **Stream Deck**, the **Stream Deck+** and the **Icon**
controllers rather than stopping at the D700.

## 2. What the author said, 2026-09-23

In his words, or as near as a summary allows. These are the inputs; everything after §3 is built
on them.

1. **The D700 has no modes of its own.** Pan, EQ, Send and FX are buttons whose LEDs the software
   sets, *"like for the LEDs they are set by the software and not the firmware"*. There is no mode
   change on the device, so Go.dot **does not have to use the DAW mode** — the Mackie meaning of
   those buttons.
2. **The modes can be repurposed** — *"they don't have to be set on the faders in the programming
   phase"*. The faders can be assigned to **the send levels of a media file**, for instance, and
   the buttons can be **shortcuts for programming** or other things.
3. **All rotaries have a click**, and **the firmware can detect a double click** — firmware level.
4. **Something similar on a Stream Deck or a Stream Deck+** (with knobs). **The Icon controllers
   have a button grid** like a Stream Deck's, *"if we want to have a broader look at the
   workflow"*.
5. **The D700's buttons:** a `*` button, a metronome button, a loop button, Rec, Play and Stop, an
   up-left arrow and a down-right arrow; on each strip, **Mute and Rec, and below them Solo and
   Select**.
6. **Each media cue will get an EQ and one or more VST inserts**, and it would be great if **the
   rotaries could control the EQ and VST parameters**. The EQ and inserts are implemented first,
   in another session.

Earlier the same day, and already built (namespace draft §16.12, *the author's first look*): a
sample has an **initial level** its fader flies to and **a touch starts it**; the strip colour
**keeps the timbre's saturation**, brightness left free for later; **a sampler group is a window
on the side of the cues**; and a DCA fader should have **an initial level "set at some point"** —
*"we'll see what feels most practical there"* (§9, question 10).

## 3. The D700's controls

Everything a page can assign. Notes are MIDI note numbers on channel 1 of the strip's bank port,
per the measured protocol; *to confirm* marks the standard Mackie note for a button the September
capture did not tabulate, to be read at the bench (§10).

**The master section**

| Control | Message | What Go.dot does with it today |
|---|---|---|
| Play | note `0x5E` | GO |
| Stop | note `0x5D` | Esc; a second press within 750 ms is double Esc |
| Rec | note `0x5F` | nothing |
| ↖ / ↘ arrows | notes `0x2E` / `0x2F` (MCU bank left / right) | nothing — banking is undecided |
| Pan / EQ / Send / FX | notes `0x2A` / `0x2C` / `0x29` / `0x2B` | nothing — the page buttons of §5 |
| `*` | note `0x36` under the Mackie preset (`0x5A` under Reaper's); `0x37` on a double click if enabled | nothing |
| Metronome | note `0x59`, MCU *Click* — *to confirm* | nothing |
| Loop | note `0x56`, MCU *Cycle* — *to confirm* | nothing |
| Master dial | turn: MCU jog, CC `0x3C` — *to confirm*; click: note `0x38` | nothing |
| Volume knob | pitch bend on channel 9, 14-bit, absolute, **no touch sense**; a click, if it has one, never captured | nothing |

**Each strip** (16, two banks of 8 by port): a 100 mm touch-sensitive **motor fader** (pitch bend,
touch on note `0x68 + n`), an **encoder** with an RGB surround (CC `0x10 + n`, sign-magnitude;
ring `B<mode> 0x30+n`; colour on channels 2–4 at note `0x20 + n`) whose **click** is note
`0x20 + n`, four buttons — **Mute** `0x10 + n` and **Rec** `0x00 + n` on the upper row, **Solo**
`0x08 + n` and **Select** `0x18 + n` on the lower — and the three-row display (12 + 12 + 8
characters and a track number).

Today only the fader, its touch and the encoder click do anything: the fader rides the strip's
target, a touch starts a sample, and the click is the strip's pad — it starts the sample, or puts
a DCA back to 0 dB.

**Go.dot's decoder already knows every one of these notes** — `McuCodec` carries the full Mackie
button table, the assignment buttons, the function keys, *Click* and *Cycle* included — so giving
a button a job is a table entry and not new code.

**Facts from the first contact** (2026-09-23, namespace draft §16.12): the ports bind as `D 700`,
`MIDIIN2 (D 700)` and `MIDIOUT2 (D 700)`; the unit answers the Mackie handshake with the serial
`D700 MA`; motor moves echo nothing back, so a flying fader is never mistaken for a hand.

## 4. Double click — the firmware's, not Go.dot's

Measured in September (protocol notes §3, control guide §5.3), confirmed by the author today:

- It is a **per-button checkbox in the Asparion Configurator**, off by default. Go.dot cannot
  switch it; a profile has to be **told** which buttons have it on.
- When it is on, a double click arrives as a **different note, alone** (`*`: `0x36` single,
  `0x37` double), and it is **suppressive**: every single click on that button is **held back**
  until the firmware knows no second one is coming. What stays precise is the *release*.
- So it is for **escalation, never alternation**: *reset this, then reset all* is safe; *GO, then
  undo GO* is not, because the single has already fired.
- **Keep it off on STOP**: Go.dot already reads two STOPs within 750 ms as double Esc without
  delaying the first. **Keep it off on encoders used as pads**, or the start of a sample waits.

*(proposed)* A page gives each clickable control **two slots**, a click and a double click; the
second is used only where the D700 profile says the Configurator has it enabled.

## 5. The proposed model — pages

### 5.1 What a page is *(proposed)*

A **page** says, for every control a surface has, what it does right now. Three kinds of control,
three kinds of binding:

- **Things that turn or slide ride a value** — faders, encoders, dials, the volume knob. The value
  is a node in the tree, written with `node.set` and held with `node.touch`, exactly as a fader
  rides a sample's trim today: the one write verb (namespace draft §16.4), so the touch table, the
  echo rules and undo's coalescing work for every page without a line of their own.
- **Things that press run a named command** — buttons, encoder and dial clicks, grid keys. Every
  gesture-reachable action is already a named command (PRD §4.11), so a shortcut is a command and
  its arguments, nothing more.
- **Things that show display fields** from the bounded vocabulary PRD §3.16 already lists — cue
  number, short name, owning cue, value, mode, state, group, colour, meter, timbre — on whatever
  the device has: the D700's rows, rings and colours, a Stream Deck key's image, the Stream Deck+
  strip above each dial.

### 5.2 Profiles and pages *(proposed)*

PRD §3.16 already splits **the device profile** (topology and protocol: what the device has and
how to talk to it — repo, machine-level) from **the layout** (what strips point at in this show —
the document). Pages sit on that line: **a profile says what controls exist; a page says what they
do.** Pages are **data, not code**, so a new device is a new profile and never a second bridge —
the namespace draft's rule (§16.10) that *a new surface is a new word in the profile table*,
carried one level further.

### 5.3 Switching pages *(proposed)*

- The **page in use is live state per surface**, never stored — like a fader's trim.
- On the D700, **Pan, EQ, Send and FX switch to their pages**; pressing the lit one again returns
  to **Show**. The lit button says which page is on, and so does the third display row, in a word
  — colour is never the only carrier (PRD §4.8).
- **Each surface keeps its own page**; the **edit lock switches all of them together** (§5.5).

### 5.4 Which cue a page edits — the focus *(proposed)*

The Send, Pan, EQ and FX pages edit **one cue**. Proposed: **the cue picked in the client**,
published as a live value every client and surface shares (a shape to be settled, for instance
`/godot/focus/cue`, `persist=none`), so the desktop, the page, a tablet and the D700 agree about
what is being edited. On the surface itself:

- **Select** on a strip picks the cue on it — a sampler strip's member;
- the **master dial** walks the cue list, and its **click** picks.

The flow this gives: *Select a strip, press Send, ride its sends, press Send again.*

### 5.5 Programming and show — the edit lock *(proposed)*

The two halves of a show's life map onto the **edit lock** Phase 5 built:

- **Unlocked is programming**: the editing pages (Send, Pan, EQ, FX) and the programming
  shortcuts are available.
- **Locked is the show**: the Show page only — pads, DCAs, GO and stop, running items — and
  nothing that edits the show. The editing page buttons go dark under the lock.

Touch-start (namespace draft §16.5) stays a **Show-page rule**: on an editing page a touch is only
a ride, because the fader is not over a sample.

### 5.6 The pages *(proposed)*

| Page | Button | Faders | Encoders | Clicks |
|---|---|---|---|---|
| **Show** (built) | none lit | sampler strips and DCA strips | ±0.5 dB on the strip's target | the strip's pad; a DCA back to 0 dB |
| **Send** | Send | the focused cue's send levels, one per mix channel in output-list order — the desktop send mixer's model (`model/Sends.h`); raising a silent one creates the `Send` | fine trim of the same | that send back to 0 dB |
| **Pan** | Pan | — | the focused cue's placement: its route gains today, a WFS source position later (question 2) | centre |
| **EQ** | EQ | the band gains *(or none — §7.1)* | the focused cue's EQ (§7.1) | band on/off, or reset |
| **FX** | FX | — | the focused cue's inserts, sixteen parameters at a time (§7.2) | reset to default |

Undo needs nothing new: consecutive `node.set` writes to **one address from one origin less than
half a second apart join one transaction** (`ShowDocument::beginTransaction`), so a fader move
undoes as one step and two faders are two.

### 5.7 The strip buttons and the rest *(proposed)*

- **Select** picks (§5.4) on every page.
- **Mute** mutes what the strip rides on the current page — a sample, a DCA, a send.
- **Solo** and **Rec** are free; **Rec** on the transport, `*`, **Metronome** and **Loop** become
  **programming shortcuts** (question 4 — new cue, record, save, undo, lock are the candidates)
  and stay dark or harmless under the lock.
- The **↖ ↘ arrows** move the standby back and forward on the Show page — the D700 has no
  rewind or forward, so today nothing on it moves the standby but GO — and page through
  parameters on the FX page.
- The **volume knob** is free; a main level, or a DCA *"Everything"*, are the obvious candidates.

## 6. The other surfaces

The same model, other profiles (PRD §3.16 already names the Stream Deck *"a triggering and
state-display surface … one running item per tile, kill on press"*):

- **Stream Deck** — keys run commands and show images Go.dot draws. On the **Show** page a key can
  be a **sampler pad** with a picture — name, colour, state, the timbre while it sounds — or a
  **running item**, killed on press; in programming, a **shortcut**. A key is a gate with a
  bitmap: the `gate` endpoint class Phase 6 built for pads, with a display.
- **Stream Deck+** — its **dials are encoders with a click** and its **touch strip is the display
  above them**: on the Send page, four dials ride four sends; on the FX page, four parameters,
  swiped along.
- **Icon** — a **button grid** that works like Stream Deck keys, and faders that work like the
  D700's strips. The model is unchanged; the profile is what the unit on the desk will say.

**The Stream Deck is another transport.** It speaks USB HID, not MIDI, and Go.dot would render its
key images itself. Talking to it directly works on Linux and needs Elgato's own application closed
— the same trade as the Asparion Configurator (PRD §3.16's bitmap tiles *"over HID"*).

## 7. The EQ and the inserts on the rotaries

### 7.1 The EQ page *(proposed)*

If the EQ is **Go.dot's own** (question 7), its parameters are known and the page can be designed
for the hands. On the D700's sixteen encoders, for instance: **three encoders a band — frequency,
gain, Q — for four bands**, then the high-pass, the low-pass and the output gain. The **ring
fills from the centre for a gain and from the left for a frequency** (mode 1 and mode 2 of the
D700's native ring). A **click switches a band in or out**, or resets it — a double click, where
enabled, resets the whole EQ (escalation, §4). The faders can carry the band gains, motors and
all, or stay where the Show page left them.

### 7.2 The FX page *(proposed)*

A VST has tens or hundreds of parameters and cannot be laid out by hand:

- **By default**, the encoders take the insert's **first sixteen automatable parameters**, and the
  **↖ ↘ arrows page through the rest**; the displays show **the plugin's own names and value text**
  (*"1.2 kHz"*, *"−3.0 dB"*), cut to the field.
- **Later, a per-plugin map** curates the order and the short names, as DAWs' remote maps do — a
  **machine-level library**, like a device profile, since it describes the plugin and not the show.
- With several inserts, the **Select buttons of the first strips choose which insert** the page is
  on, or the page walks them in order.
- **Speed:** a D700 detent can arrive as a step of more than one when the encoder turns fast
  (values 1, 2, 3 … — sign and magnitude), so a slow turn is fine and a fast one coarse.
- A **click resets that parameter** to its default.

### 7.3 What the engine decides — PRD §3.18

Three facts already recorded shape all of this:

- **A track's plugin chain is structural.** Tracktion restarts playback when a chain changes (the
  tree watcher), so a cue cannot insert its plugins at the moment it plays. Per-cue inserts need the
  chain **in place beforehand**: a fixed chain on every track — the EQ and some insert slots,
  loaded at start, a cue writing its settings and switching slots in — or a **rack channel** the
  cue claims, already carrying the plugins (§3.18's current design; question 8). **Bypass restarts
  nothing**; only building the chain does.
- **Plugin parameters can only be written from the message thread** — Tracktion asserts it — so
  every rotary turn hops across from the tick thread. Measured affordable: about **4 µs a write** in
  a Release build, 512 parameters at 50 Hz an eighth of a tick.
- **Editing a cue that is not playing** is the subtle case. Its settings live in the show file, so
  the rotaries can always change them — but a VST's own value text comes from an **instance**, and
  there is one only where the cue is armed. *Play it, tweak, stop* works as it is; editing silently
  wants a spare editing instance, or parameter names and ranges cached from the scan.

## 8. What the pages need from the EQ and inserts *(for the session that builds them)*

Nothing here asks that session to build a page. It asks that the EQ and the inserts be
**reachable** the way everything else a surface rides is, so the pages can be added later without
reopening them:

1. **Every parameter is a node** in the tree, under its cue — a shape such as
   `/godot/cue/<id>/eq/<band>/<parameter>` and `/godot/cue/<id>/insert/<n>/<parameter>`, whatever
   that session settles — carrying its **value**, and published beside it its **name**, a **short
   name** where one exists, its **default**, its **range** or its **steps** (a choice parameter is
   discrete), whether it is **bipolar** (so a ring knows to fill from the centre), and its **value
   text** as the plugin formats it, when an instance exists.
2. **Written with `node.set`**, from any origin, like a send level — so a surface, the desktop, the
   page and a script all reach it, the touch table gates it, and undo's coalescing makes a turn one
   step. The **message-thread hop is the engine's**, never a client's.
3. **The insert order is readable** — which inserts a cue has, in which order, each with its plugin's
   name — so the FX page can say which one it is on.
4. **A parameter's name and range are readable without a playing instance** where the scan can
   provide them, so a page can label a cue that is not sounding.

## 9. Open questions — the author's

Each with the recommendation made in the conversation.

1. **Which cue a page edits** — the client's pick, shared as a live value (*recommended*); the
   standby cue; or only a pick made on the surface.
2. **What Pan moves** — the route gains now; a WFS source position later; something else.
3. **Under the edit lock** — the editing pages unavailable (*recommended*), or allowed.
4. **Shortcuts** — fixed defaults in the D700 profile first and editable in the Surfaces tab later
   (*recommended*), or editable from the start; and **which commands** deserve a button (new cue,
   record, save, undo, lock).
5. **Page switching** — each surface keeps its own page and the lock switches all together
   (*recommended*), or one page for the whole room.
6. **Scope** — build the page model and add devices one at a time, the D700 first and the Stream
   Deck next (*recommended*), or draw every device first.
7. **The EQ** — Go.dot's own built-in (*recommended*: known parameters, in-process, a page designed
   for the hands), or a plugin like the inserts.
8. **Per-cue inserts** — a fixed insert chain on every track, switched per cue, or rack channels the
   cue claims (PRD §3.18 as written). It decides whether the plugin exists before the cue plays.
9. **VST pages** — automatic, first sixteen and page, with curated maps later (*recommended*), or
   curated from the start.
10. **A DCA's initial level** — a level stored on each DCA that its fader flies to when the show
    opens, or a zero-length fade aimed at the DCA where the scene starts (which works today).
11. **Carried from Phase 6** — a stop cue aimed at a sampler group cuts its clips rather than fading
    them; GO on a running non-sampler group starts a second copy (decision N); a surface switched off
    in the show lets its port fire triggers again.

## 10. To read at the bench

- The **Metronome** and **Loop** notes (`0x59` and `0x56` expected), and whether the **volume knob**
  clicks.
- The note an **encoder's double click** sends, with double click enabled on one encoder in the
  Configurator — only `*`'s is known (`0x37`).
- The **master dial's turn** (CC `0x3C` expected).
- **M27** (the colour rate the unit takes) and **M28** (how soon its idle animation returns) —
  instruments in `tests/blackbox/`, which need `python-rtmidi`.

Go.dot has no monitor of incoming MIDI yet; a `wfg midi --watch` printing each message with its
port, its bytes and its decoded name would read all of this in a minute.

## 11. Suggested order

1. **The EQ and the inserts** — the other session, with §8 in hand.
2. **The page model on the D700** — pages as data, the Show page ported onto it unchanged, then Send
   (its model exists), then EQ and FX on what step 1 exposed, the focus and the lock rule with them.
3. **The Stream Deck and Stream Deck+** — a HID transport, key images, the dials.
4. **The Icon** — a profile, once the unit is on the desk.
