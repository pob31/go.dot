# Go.dot — Product Requirements Document

**Draft 0.8** — **the §6.1 spikes have been run.** All seven, with one report each
in `docs/spikes/`. **All seven pass.** §9.2's rule — the polyphony model
stands unless #2 or #4 fails — is satisfied: **the model stands**, and §3.25's
"fixed graph, dynamic content" is now measured rather than assumed.

Amendments carry an *Amended in 0.8* marker naming the report they come from, so
any changed claim can be traced to the measurement that changed it. Item #2's
premise turned out to be false in Go.dot's favour; the stereo ceiling recorded
during the spikes was removed by a Tracktion update before this draft was
written, and no amendment for it survives.

Draft 0.7 was: audio engine decided: Tracktion Engine (§3.25). Go.dot owns time;
TE is a sample-accurate polyphonic player with a plugin graph. Spikes in §6.1
rewritten as validation of that model; the `OSCClip` fork and MidiClip smuggling
are removed.
Binary/package name: `wfg`. Repo: `github.com/pob31/go.dot` (private until
alpha). Licence: **GPL-3.0** (`LICENSE` in place).
Engine: Tracktion Engine **develop (3.5.0)** with **JUCE 8.0.13**, both pinned as
submodules. Status: Phase 0 complete; Phase 1 next.

**Reading convention:** *(proposed)* marks a design I put forward that has not
been explicitly confirmed. Everything unmarked traces to a stated decision.

---

## 1. What this is

A cross-platform show control application for theatre, dance and live music:
a cue list that carries audio and video, holds live parameter bindings during
a show, and acts as the conductor for a family of specialised processors
(WFS-DIY, XOA/Tight-WFS, S21-HiJack) that it commands but does not contain.

Three ancestors, one synthesis:

- **QLab** — cue list, fades, chaining, operator ergonomics. Taken as the spine.
- **Ableton Live** — hands on parameters while material runs. Taken as the
  binding layer, not as clip launching.
- **Chataigne** — computation on protocol streams, state machines. Taken as the
  control-rate dataflow graph.

### Why not an existing tool

| Tool | Why not |
|---|---|
| QLab | macOS only; cannot compute on an OSC/MIDI stream; network cues are fire-and-forget, so non-linear rehearsal cannot reconstruct state; scripting surface (AppleScript) diverges from its OSC surface; no fader-start |
| Ableton Live | song-shaped, not show-shaped; automation evaluated at audio rate and overloads with many curves; OSC is bolted on |
| ossia score | timeline-first paradigm; excellent, but not a cue list, and the operator model is wrong for a booth |
| Chataigne | a brain, not a host; no cue formalism, no media |

### Non-goals (v1)

- Not a DAW. No composition, no arrangement.
- Not a lighting console.
- Not a spatial renderer. That is WFS-DIY / XOA, addressed over the network.
- No mobile authoring. Tablet is an operating and adjustment surface.

---

## 2. Users and context

**Primary user:** freelance sound engineer/designer running theatre and dance,
often responsible for both sound *and* video, frequently mixing from the house
rather than a closed booth.

**Secondary users:** stage manager (reads cue numbers over comms, never touches
the app), sound designer sitting in the house during tech, researchers running
experiment protocols.

### Booth constraints — design drivers, not preferences

- **Table footprint ≤ 1.5 m.** Two metres of desk in the house costs 8–12 seats
  over 2–3 rows. One screen + one tablet + one control surface is the budget.
- **Minimal emitted light.** Dark UI is mandatory, not a theme option.
- **Operator listens in the house**, not through booth monitors. Legible and
  operable at low brightness, at arm's length, in the dark.
- **No Alt-Tab, no second app.** The direct reason video is built in.

---

## 3. Architecture

### 3.1 State vs content — the load-bearing wall

Cues and media live **in** Go.dot. Audio and video *data* flowing through
external processors stays outside: those processors have state, not content.
S21-HiJack, WFS-DIY and XOA remain standalone applications, possibly on other
machines, addressed over the network.

### 3.2 Headless engine, clients as equals

The engine owns the document, transport and I/O, and exposes everything over
**OSCQuery**. Desktop UI, tablet, surface bridge, MCP and external scripts are
all clients of the same surface.

**Law:** nothing the UI can do that the API cannot. The UI is built as a client.
Every gesture-reachable action also exists as a **named command**, so it can be
bound to a button, called over OSC, or found in a menu. Modifiers and gestures
are an accelerator layer over a complete command set, never the only route.

### 3.3 Parameter tree

One addressable tree. Foreign namespaces are **mounted**, each carrying its own
transport, rate cap and anticipatability annotations:

```
/godot/…            engine, cues, transport, standby
/wfs/…              WFS-DIY instance
/xoa/…
/s21/…              S21-HiJack sidecar
/ext/console/…      arbitrary user-added device via daemon
```

A user's "extra strips for my own use" are ordinary bindings, not an escape
hatch; everything routes through Go.dot and therefore appears in the running
view. Direct surface→device paths are faster and invisible; invisible is what
this application exists to abolish.

Each node declares:

- **kind**: settable state | event (one-shot, has no value at time *T*)
- **rate cap** for outbound dispatch
- **anticipatable**: may be pre-sent before GO (imperceptible *and* revocable in
  the current state); third-party defaults to false
- **panic value**: park | snap-to | declared safe value

### 3.4 Two clocks

- **Audio clock** — the engine's audio callback.
- **Data tick, 50 Hz, derived in sample time.** At 96 kHz / 64 frames the
  callback runs at 1500 Hz; a tick every 30 blocks is exactly 50 Hz,
  phase-locked, tick indices convertible to sample positions.

Curves are evaluated on the tick thread, never in the audio callback. Control
rate decides *where to go*; audio rate decides *how smoothly to get there*
(per-block slewing on internal audio parameters, or you trade Live's overload
for zipper noise).

Launches quantise to the tick: every message belonging to one GO leaves in the
same frame.

*Amended in 0.8 — `docs/spikes/spike04-graph-stability.md`,
`spike05-param-50hz.md`.* Two measured facts bear on this clock.

**Tracktion's own launch instant is not reproducible.** Its sync point is
influenced by transport start rather than purely by processed blocks, so the same
launch lands on a different sample between runs. Go.dot therefore cannot delegate
launch timing to the engine and expect determinism — the tick-quantised GO
described here has to be enforced on Go.dot's side, which this section already
implies and which is now a requirement rather than a preference.

**Plugin parameters can only be written from the message thread.** Tracktion
asserts it unconditionally, so a tick that touches a plugin parameter must hand
over from the tick thread to the message thread. Measured, that handover is
affordable — 512 parameters at 50 Hz cost about an eighth of a 20 ms tick in a
release build, roughly 4 µs per write, with the message thread's responsiveness
indistinguishable from idle. The cost is the ValueTree write itself, not
notification, so there is nothing to be gained by rate-limiting notifications.
What the measurement does *not* remove is the need for the handover.

### 3.5 Cue model and pointers

The **cue graph** is the model. The vertical cue list is its primary
*projection* — sacred, down to decimal numbering, because it is the protocol
between operator and stage manager. Other projections are equal citizens over
the same data: time view, target view, device view, running view, fader-usage
view.

**Identity is two things and they never merge:**

- **Cue number** — human protocol. Mutable, decimal, renumbered during tech.
- **Cue ID** — machine identity. Assigned at creation, immutable, never reused.
  Short opaque base32 (8–10 chars), **visible, copyable and searchable**.

**Group-level editing is a shortcut, not ownership.** In edit mode a group may
display fields common to its members so they can be edited in one place — a
bulk-edit affordance equivalent to multi-selecting every member and editing the
field once. The values belong to the members; the group row is a *view* over
them, following the same derived-upward rule as DCA exposure (§3.6). Standard
mixed-value inspector semantics: show the value when all members agree, show
"mixed" otherwise, and typing sets all. Both routes — group shortcut and explicit
multi-select — exist and achieve the same thing. The group view can also be
**filtered by contained cue type** — all audio, all video, all OSC, all MIDI —
so a mixed group exposes the fields relevant to one kind at a time rather than
the thin intersection of everything. No list-level or group-level
defaults; nothing is inherited downward (§4.12). *(An earlier draft proposed a
live cascade with inherited/escaped provenance; withdrawn.)*

#### Parallel cue lists

Multiple lists run concurrently. **One standby pointer per list**; exactly one
list has operator focus at a time and only that list's standby responds to GO.

This is the clean separation between operator action and machine action:
timecode-, clock-, OSC- and MIDI-triggered cues live in their own lists and can
never move the operator's position. A list with no standby pointer and only
event triggers *is* a background dataflow process — which is how the deferred
state-machine phase arrives without new machinery, and how the experiment
protocol runner works (one list stepping trials, another watching for responses).

#### Three kinds of pointer

| Pointer | Scope | Notes |
|---|---|---|
| **Standby** | one per list, **engine state** | what GO acts on; never moves as a side effect of selection or scrolling |
| **Edit / selection** | **per client** | desktop and tablet each have their own; detached from standby |
| **Run** | *n*, one per active group | live objects, selectable in the running pane, individually addressable |

Explicit actions: "standby = selection", and "show me standby" (scrolls without
moving anything). Three markers compete in one list, so each needs a distinct
treatment; colour cannot carry it alone.

**Only GO advances standby.** A timecode- or OSC-triggered cue never moves it,
or the operator presses GO expecting cue 12 and gets 14.

**Standby advances immediately on GO** — never on completion. For an automatic
sequence, standby lands on the cue *positionally after* the whole automated chain
(the next sibling), the instant GO is pressed. "After" means list position, not
time: the standby pointer is ready for the operator's next action while the
automated sequence runs on its own. The run pointers catch up underneath it.

### 3.6 Groups

**No auto-follow, no auto-continue. One GO equals one row.** Structure lives in
the hierarchy, where it is visible and foldable, not in a column of arrows.

Two attributes, not three types:

- **Timeline group** — all members scheduled at entry; pre-waits are offsets.
- **Sequence group** — members run one after another.
  - **auto** — a member starts when the previous completes.
  - **manual** — a member starts on GO. The standby pointer *descends into* the
    group; the operator is the parent. Toggleable during tech; a mid-run change
    takes effect at the next member boundary.

The **cue list is, virtually, a sequence group in manual mode** — one model, no
special case at the top — but need not be *displayed* as one.

**Fire-and-forget** is not a flag; it is what an automatic sequence *is* from the
operator's side. The chain completes on its own while the operator continues with
subsequent manual cues. Typical case: a complex background sound design of many
chained cues runs autonomously while the operator fires occasional foreground
effects on manual cues on top of it. For a *nested* sequential parent, the
child's completion is still "last member completes" (§Completion) — if a parent
must not wait for a long child, put them in a timeline group instead.

Groups may nest, have pre-wait and post-wait, and expose member parameters.

#### Selection modes (both auto and manual)

- `sequential`
- `shuffle` — shuffled rounds. **Constraint: the first item of a round is never
  the last item of the previous round.** Implement by reshuffling until the
  constraint holds, not by swapping (which skews the distribution). With two
  members the constraint fully determines the order; that is correct.

A round is **materialised** as a list, not drawn per trigger. This gives seeded
replay, a displayable round, and an honest definition of a loop iteration.

**RNG is seeded per group and the seed is logged**, or deterministic replay
breaks and a technical rehearsal cannot reproduce yesterday. A fixed-seed option
exists for designers who want "random" to be the same every night.

Also: `play N of M`.

#### Loops

- iterations: **N or infinite**, current count visible on the run pointer's
  strip ("3/8")
- runtime command **stop after current iteration**, bindable
- iterations count **rounds**, not playbacks, so pruning members keeps the
  arithmetic predictable
- an emptied round completes the group rather than spinning
- in a manual loop, GO past the final iteration completes the group and advances
  standby to the next sibling on that press (never a GO that does nothing)

#### Round pills (running pane)

Overlay dots showing the materialised round.

| State | Appearance |
|---|---|
| pending | normal |
| played | dimmed |
| pruned, this round only | dimmed (same as played — same meaning: won't happen again this round) |
| pruned, until group ends | dimmed with red **X** |

Click = prune this round. Modifier/double-click = prune until the group run
ends. Both are **run-local** and evaporate when the group stops; persistent
enable/disable lives on the member in the cue list. Clicking again reinstates,
if not already passed.

#### Completion

`sequential` advance depends on "done", which differs by kind:

| Cue kind | Done when |
|---|---|
| media | file end or out-point |
| fade | duration elapsed |
| group | last member completes (footer included, §Header and footer) |
| OSC/MIDI | per the three-valued wait (§3.11): `none` on send, `sent` on queue drain, `verified` on read-back match |

**Post-wait** = how long after completion this cue reports done to its parent.
Meaningful in `sequential`, inert in `parallel`.

A sequence of OSC cues can therefore be a chain that waits for each processor to
confirm before advancing. This composition is the most valuable thing in the
group model.

#### Header and footer

Independent of each other; the user decides whether to use either.

- **Header** — runs before the group's members. This is where §3.12
  prepare/commit lives, extended from one row to a whole block: pre-position
  sources, preload media, pre-arm bindings for eight cues, then GO commits only
  the perceptible part. Header actions must be **imperceptible and revocable**,
  or moving standby away leaves residue. A member declaring something
  un-anticipatable simply doesn't get it pre-sent; the row shows *partially
  armed*.
- **Footer** — an ordinary cue list that runs at group exit. **Not** an inverse
  of the header. Typical content: kill LFOs and AutoMotion in WFS-DIY, stop
  effects processing, release audio interface channels. Snapshot-and-restore is
  one thing a footer may *contain*, not what a footer is.

**Footers block.** The group is not done until its footer's cues report done, so
a following scene that reallocates the same interface channels waits for the
release rather than racing it.

Nested scopes tear down innermost-first. A footer should only touch what its own
group owns.

#### Group parameter exposure — derived upward, never inherited downward

**Containers describe behaviour; content describes output. Nothing flows down.**
A group has no audio identity of its own. What it exposes is *derived upward*
from whatever its members happen to have. Groups organise time, order, lifetime,
headers/footers and collective adjustment; they do not own outputs, media or
parameters, and members never inherit anything from them.

DCA semantics, stated as such: **trim, not write.** Members' own values
untouched; nested trims compose multiplicatively; park on release. Exposed set is
derived automatically (any parameter at least one member has, applied only to
members that have it) with a manual override — otherwise a heterogeneous
audio+video+OSC group exposes nothing useful.

Because the trim is arithmetic over member values rather than a property pushed
down a hierarchy, a group fader works identically whether its members sit on one
slot, twelve slots, or a mix of slots and buses.

Repetition of an assignment across members is **not** inheritance. It is
multi-select edit and copy-assignment (§3.9), which work across a selection that
spans groups.

#### Authoring ergonomics

Grouping a selection is **one keystroke**; ungrouping is another. If it is a
dialog, people will resent it by the second tech.

### 3.7 Triggers

GO is one trigger among several, not the mechanism. A cue or group carries a
trigger list:

- **GO** (operator, focused list only — the only trigger that advances standby)
- **timecode** (LTC/MTC)
- **wall clock**
- **OSC** message
- **MIDI** message
- **fader movement** (§3.9)

Debounce/false-start guarding is a **user preference** (e.g. GO debounce time),
not a hard-coded constant.

### 3.8 Stop cues

Target by **stable ID** — never by cue number. May also target a **tag**
*(proposed)*, so one cue stops all ambience without enumerating a list that goes
stale during tech. May target a specific **run pointer** rather than a whole
group.

Modes: hard stop · fade-and-stop (duration + curve) · stop after current
iteration · stop after current member. Range-level verbs (`advance`,
`advance at range end`) are the same targeting object with a different verb —
see §3.24.

Targeting a group stops it recursively by default. A stop cue whose target is
not running is a **silent no-op, not an error** — that case is constant during
rehearsal jumps.

**Open:** if a cue is fired while already running, does it restart or run a
second instance? Pick one per cue type, make it visible, and let stop target
either all instances or the most recent.

### 3.9 Exclusive resources: faders, processor slots, interface channels

Three scarce, physically fixed, exclusive resources share **one allocator**
(see "The shared allocator" below). Audio buses are *not* among them: a bus is a
summing point, shared by construction, with nothing to allocate.

#### 3.9a Fader linking, DCA and fader-start

A hardware fader may be linked to a **cue or group's level, a live input stream's
level, or a video cue's opacity**, as a DCA trim. Because it is a trim and not the
clip's own level, -inf is non-destructive and the level can be brought back up.

Requirements:

- fader **follows automation** on the group DCA (motorised, bidirectional)
- a **start value** the fader flies to in advance (during the header/prepare
  phase)
- **fader-start**: a linked fader leaving -inf starts the cue. Threshold
  slightly above bottom, with hysteresis, or a parked fader chatters.
- **fader-stop**: trim reaching -inf stops the cue — **conditional on the fader
  being released** (touch-sense), so riding through the bottom during a fade
  does not kill the cue. This is where touch sensing stops being a nicety.

Fader-start is only possible because of §3.12: by the time the finger moves,
the file is open, the transport armed, the channels allocated and the processor
verified. Nothing remains but the unmute.

#### 3.9b Audio destinations: buses and slots

QLab treats all outputs as identical. That is wrong here, but the distinction is
**not** mono vs stereo — it is **shared vs exclusive**.

| | **Bus** (speaker output) | **Slot** (processor input) |
|---|---|---|
| Nature | summing point | per-source state carrier |
| Sharing | many cues, freely | exclusive, allocated |
| Why | mixing is the point | the slot holds position, trajectory, LFO, AutoMotion state — two cues sharing it don't sum, they fight over where the sound is |
| Width | irrelevant | matters, because the slot has one |
| Model | **QLab-style routing matrix, kept as-is** — the most common way to handle multichannel outputs | allocated resource (§3.9c) |

Width matching is enforced **only** for slots, and only because the slot has a
declared width. For channels that are merely mixed into a bus it does not matter
at all.

Rules:

- **Destinations are a list, not a choice.** A cue may hold a slot *and* a bus
  routing simultaneously — a source into WFS plus a stereo feed to foldback is
  ordinary.
- **Width is explicit, never automatic** *(qualified in 0.8 —
  `docs/spikes/spike01-bus-routing.md`, `spike06-rack-latency-pdc.md`)*. This
  remains Go.dot's rule, but it is **no longer true of the engine underneath**:
  Tracktion now canonically upmixes a mono source into a wider destination, where
  it previously left the extra channels silent. A mono *destination* still stays
  mono. So Go.dot must declare destination widths explicitly rather than relying
  on the engine to leave them alone.
- A stereo cue may occupy one stereo slot
  *or* two mono slots *(proposed)* — two independently positionable objects is a
  legitimate and often better choice in WFS. Offer both; refuse silent downmix or
  upmix.
- **The processor declares its own slots** *(proposed)*. WFS-DIY and XOA speak
  OSCQuery, so Go.dot discovers how many inputs exist and at what width, rather
  than having the user type a channel count that drifts out of date.
- **Slots carry user-authored names** ("Voix solo", "Ambiance G/D"). Those names
  appear in the dropdown and on the OLED — channel numbers do not.
- **No auto-assignment, ever.** The user lays out their channels as the sound
  design intends. The dropdown lists only what is free at that point in the list,
  obvious candidate first.
- **Copy-assignment** ("same output as cue 12") and **multi-select edit** are
  first-class gestures, because that is what a designer actually does when
  writing — not picking from a menu forty times.

Lifecycle:

- **Claim happens in prepare** (§3.12). The header claims the slot and verifies
  the processor accepted it; GO commits only the perceptible part. This is also
  what makes fader-start work on a spatialised cue.
- **Release is footer-timed, not cue-timed** (§3.6). A slot is not free when the
  file ends — a fade or tail may still be running in the processor. It frees when
  the group's footer reports done. This is why footers block, and it is what
  makes fast scene changes safe.

#### 3.9c The shared allocator

Faders, processor slots and interface channels are the same problem and get
**one implementation**, not three parallel mechanisms with slightly different
bugs.

Each binding has a **live range** over show time. Overlapping ranges cannot share
a resource. "Guide me to a free one" is a linear scan; the reorder warning is
liveness re-analysis on edit.

- Some ranges are indefinite (loops, manual groups, operator-paced material), so
  the analysis is **conservative**: it can prove possible overlap, never prove
  impossible. **Warn, don't refuse**; allow marking deliberate sharing.
- Analysis runs **across parallel lists**, since two lists can be live at once.
  Cross-list sharing is **disallowed by default** with an explicit override
  *(proposed)*.
- Show a **usage-over-show-time plot** per resource kind. Designers already draw
  this by hand for radio mic channels and will read it instantly.

One allocator, one plot, one warning system, three resource kinds.

#### 3.9d Banking — decide with the hardware in hand

Logical allocation ≠ physical availability. Forty cue faders on a sixteen-fader
surface means a fader exists on one layer only, and fader-start fails silently
if the operator is on another layer. Options: a reserved always-visible bank for
fader-start cues; auto-switch the layer to follow standby; or refuse to allocate
a fader-start binding outside the visible bank. *(Proposed lean: auto-follow with
a hard indicator, allocation preferring the standby layer.)* Step-by-step
implementation against the D700 will decide; this is novel territory — it is
adding controller commands over the equivalent of QLab audio cues — and is
probably the most UX-critical part of the system. Expect it to be adjusted in
practice.

Fader bindings are **show state** and live in the document, so the document has
a hardware dependency. Loading a 16-fader show on 8 faders, or none, must
**degrade rather than fail** — the show should run from a laptop in an
emergency, badly but completely.

### 3.10 Bindings and automation

A binding is `endpoint ↔ node + scaling`. Three lifetimes: **persistent**
(background bridging), **cue-scoped** (armed on entry, released on exit),
**one-shot** (a macro is a pulse through the same graph).

**Automation modes**: read / touch / latch / write. **Capture into a cue is an
explicit "update cue" action**, never a silent write-back.

**MIDI cues carry every event type**, not only CC curves: Note On/Off, Program
Change, CC, pitch bend, aftertouch, SysEx. Many devices expect a Note On or a
Program Change as their trigger; a MIDI cue that could only emit continuous data
would not talk to them. Sequenced MIDI-note output ("MIDI clips to output") is in
scope from the start.

**Fades and lanes are one representation.** A QLab-style fade is a degenerate
curve (two breakpoints and a shape). Two authoring surfaces:

- **Fade cue** — target, destination or 2D path, duration, curve. Retargetable,
  relative, visible as a row.
- **Lane** — dense breakpoints, recordable from gesture, bound to a clip's local
  transport (scrubs when the clip scrubs).

Curves that must follow a media transport live with that clip; transport-free
fades are wall-clock interpolators in the control graph, hundreds concurrent,
no audio engine involvement.

**Curve editor:** breakpoint lists with numeric entry, not only draggable
pixels.

**2D paths** are two paired lanes plus a dedicated view. A trajectory clip
speaking ADM-OSC, launchable and loopable, is the single most differentiating
feature in the product.

### 3.11 Closed-loop cues

Targets speak OSCQuery, so a cue is an **assertion with read-back**. Per-cue
wait: `none` / `sent` / `verified` (**default for own processors**). Enables
relative moves computed from actual state, and failure visible in the list
rather than discovered by ear.

### 3.12 Prepare / commit

Anticipation is a property of the **parameter**, not the cue. Standby
auto-prepares; group headers extend the horizon from one row to a block. GO
carries only the perceptible commit. Jumping away rolls preparation back —
silently, guaranteed by the revocability rule. Row shows `idle / preparing /
armed-verified`.

Own processors may receive tick N+1 values during tick N as OSC bundles with
timetags, erasing network jitter. Third parties get plain send-on-change.

A generous prepare horizon is also what gives **Go Doh!** (§4.4) something to
recover.

### 3.13 Non-linear rehearsal: the state solver

State at time *T* is **computed**, not replayed:

1. Walk back through the list accumulating the last writer of each parameter;
   evaluate at its end state, or partway if *T* lands inside a fade.
2. Reconstruct **what is running**, not only parameter values: run pointers,
   which member of which nested group would be active, and at what offset. A
   jump into the middle of a five-minute auto sequence lands with the right
   member playing at the right offset. Stop cues cancel prior starts, so the
   walk tracks cue lifetimes alongside last-writers.
3. **Diff against the world's actual state** (read-back) and send only what
   differs. A rehearsal jump is a minimal correction, not a shotgun blast.
4. Event-kind nodes are excluded (do not re-fire the pyro).

**Waypoints and solver work together, not instead of each other.** A waypoint is
a known-good full state; the solver bridges from the nearest waypoint to the
selected time. **Group boundaries are structural waypoints** — the walk can stop
at one rather than going to the top of the show. Manual waypoints remain
available as a way to force a divergent world back into agreement.

Because footers are arbitrary and need not be inverses, jumping backwards past a
group **recomputes forward** from the last waypoint; it does not unwind.

**Two pointers visible during a jump:** standby position and state position.
After a jump they agree; after a manual tweak they do not, and that divergence
is what the running view shows.

Dual-touch load-to-time (§3.17) is these two pointers, one per hand.

### 3.14 Timecode

**Chase and generate**, both required. LTC and MTC.

**Open structural question:** the 50 Hz tick is derived from sample time, which
is clean when master. When chasing, tick indices must remain monotonic and the
derivation re-anchored without discontinuity. Specify before the transport is
written.

### 3.15 Redundancy

Deferred until the cueing architecture settles — designing it first would be a
guess about the wrong failure.

Kept cheap in the meantime:

- **All engine state mutation flows through one ordered, tick-indexed path.** A
  sync link is then a second consumer of that stream, and so is deterministic
  replay for regression tests. Without it, state changes scatter and there is
  nothing coherent to mirror.
- **Sync intent + position, never derived runtime state.** The first is a small
  problem; the second is unbounded.

Known constraints: feedback must be asymmetric (fan-in on control, single
arbitrated source on feedback, or two engines fight over motorised faders); a
USB surface is physically bound to one machine, so the tablet is the redundancy
path.

### 3.16 Control surfaces

Reference hardware: **Asparion D700 Rack** — 16 touch-sensitive 100 mm motor
faders (12-bit, 4000 steps), 17 RGB encoders with LED rings, 76 buttons,
optional D700S OLED (2 × 12 chars + 1 × 6, track number, metering).

**Vendor guidance (Patrick, Asparion):** for own development, use **MIDI**. The
device is Mackie-based with vendor extensions. The DAW scripts published on
their site may be used as code for own projects. They are considering the
suggested **demo mode** and **multiple-target** features.

→ **Mackie-first is now both the Linux-driven constraint and the vendor's own
recommendation.** OSC/MQTT run through the Asparion Connector, which does not
exist on Linux, Android or iOS.

**Abstraction:** *N* strips, each with optional fader+touch, encoder, buttons,
display; plus a transport (MCU / HUI / raw MIDI / OSC / HID). Bindings target
abstract strips; **device profiles** map them to hardware.

- **Device profile** = topology + protocol. Portable, machine-level, shareable,
  community-contributable. Lives in the repo.
- **Layout** = what strips point at in *this* show. Lives in the document.

Implementation order: **to be decided with the D700 in hand.** An earlier draft
said HUI first, on the argument that HUI is grotesque enough to expose any place
where the abstraction is secretly MCU-shaped. That predates the vendor's
Mackie-with-extensions answer and the fact that HUI lacks master volume on this
device. Mackie is the documented, vendor-recommended path; whether HUI is still
worth doing *first* as an abstraction stress test, or later as a compatibility
profile, is an open question for the first week with the hardware.

**Required from day one:** change-origin tagging (echo suppression) and touch
state gating outbound updates — now general engine concerns, not surface
concerns, since tablet and desktop also write the same nodes. Standing test
case: two surfaces on different protocols bound to the same node.

**Strip allocation:** pinning is first-class. Pinned strips never reassign;
unpinned follow the show; the display always says which.

#### Endpoint classes

Three, not one — this drops in a whole family of devices without special-casing:

| Class | Example | Semantics |
|---|---|---|
| **absolute** | motor fader | position is value; soft takeover / touch |
| **relative** | endless encoder | increments |
| **rate** | SpaceMouse, spring-centred joystick, pedal | deflection is rate of change; deadzone; defined behaviour on release mid-deflection |

A 6DOF controller driving a WFS source position is better than two faders —
three axes plus orientation is what the parameter actually is.

#### Display as a renderable

The engine produces a **bounded field vocabulary** — cue number, short name,
owning cue, value, mode, state, group, colour, meter. Each profile renders what
it can:

- **text cells** — MCU scribble SysEx, D700S OLED
- **bitmap tiles** — Stream Deck (std, XL, +, +XL) over HID
- **vector clients** — tablet, desktop

The **layout chooses which fields go on which line, per strip**; the user
decides what makes sense rather than having a fixed scheme imposed. Bounded
vocabulary keeps this from becoming an infinite UX surface and lets it degrade
sanely onto 7-character scribble strips.

**Authored short names (7 chars), never auto-truncation.** Colour is never the
sole carrier — colourblind technicians, blue worklight.

Stream Deck has no touch sense and no motors: it is a **triggering and
state-display surface**, not a binding surface. Same abstraction, different role;
the layout model knows the difference. Natural fit: one running item per tile,
kill on press.

### 3.17 Tablet and multitouch

**Desktop UI layout is deliberately undesigned at this stage.** Tabs, collapsible
sections and pane arrangement will be decided once the workflow is clear, with
one explicit aim: **not a QLab copycat.** The cue list row conventions are kept
(§3.5); the surrounding layout is not.

The tablet is a **second surface, not a second screen**, and does not replace
faders except temporarily (walking the house, testing).

Roles:

- **running pane** — kill, advance, prune round pills, drag playheads. Putting
  all runtime action here lets the cue list scroll freely while the show
  continues, and avoids the wrong-object problem (a cue list row is a *cue*; the
  thing you kill is one of its instances).
- **video mapping at the projection surface** — walk to the wall instead of
  buying binoculars. Requires nothing new: surface geometry, corner pins and
  opacity are ordinary tree nodes, so this is a view.
- **parameter adjustment while walking the house**
- **remote triggering** for a designer sitting away from the console
- **redundancy path** — renders the full surface layout so losing the D700 is a
  downgrade in feel, not capability

Multitouch principles:

- Touch is good for **discrete, deliberate** actions; hardware is for
  **eyes-free** riding. The split is looking-vs-not-looking, not coarse-vs-fine.
- **Precision is a function of gesture geometry, not of a mode.** Radial gain
  (Soundcraft pattern): distance from a rotary's centre multiplies angular
  precision. Linear equivalent: displacement along a fader's axis is the value,
  displacement across it is the divisor.
  - **relative from touch-down, never absolute** — the control does not jump to
    where you touched; soft takeover by construction
  - **continuous ratio**, no discontinuity as radius changes mid-gesture
- **Dual-touch load-to-time**: one finger holds the anchor, the other sets the
  target — the two solver pointers, one per hand, with the solve running live so
  the operator sees the result before committing.
- Accidental contact is real (a forearm on a screen in the dark). In show mode,
  destructive targets want a deliberate gesture — swipe-to-kill beats a 9 mm
  (X).
- **Every gesture has a non-gestural equivalent** (§3.2).
- Wifi loss must leave the pane **visibly stale**, never silently frozen, and
  any in-flight adjustment lands on a defined value.

#### How the tablet client is delivered *(amended in 0.8)*

**A web client is the primary surface, and a native companion is an optional
addition on top of it — not an alternative to it.**

The web client (TypeScript over OSCQuery + WebSocket) is what makes the tablet a
*genuine* fallback: no install, no store relationship, and it reaches the iPad in
the house, an Android tablet, and a browser on the booth machine from one
codebase. It also inherits §3.22: because the engine speaks OSCQuery and OSCQuery
**describes itself**, the client discovers the namespace rather than shipping a
copy of it. WFS-DIY's Android remote needs a hand-maintained parameter table on
both sides; Go.dot's client does not, and cannot drift from the engine.

What a native companion buys is **resume latency**, and it is a real operational
benefit rather than a technical nicety:

- A suspended native app is still resident. Swiping to it restores the surface
  **immediately**, with its state intact; only the socket has to re-establish.
- A backgrounded browser tab may be **discarded** under memory pressure and then
  has to reload the page, re-render and re-subscribe. The operator reaching for
  the tablet mid-cue is the exact moment that cost is least affordable.

So the pane is never *silently* wrong either way — but "swipe and it is there"
and "swipe and it is loading" are different instruments in a show.

Background persistence is a separate property from resume latency, and it splits
by platform rather than by technology:

| surface | resident in the background? |
|---|---|
| **Android** | **Yes** — a foreground service holds the connection open, so the client is never stale at all. WFS-DIY does exactly this. |
| **iOS** | **No**, native or web. Background execution is limited to declared modes (audio, VoIP, location, BLE); a general network listener is not among them. A native app still wins on resume, but nothing keeps it live. |
| **Desktop browser** | **Yes** — desktop browsers do not suspend tabs the way mobile does, so the booth panel is unaffected. |

Two consequences for the design, and neither is a change of direction:

- The **web client remains primary** and must be complete on its own. It is the
  redundancy path (§3.5), and a redundancy path that requires an installed app is
  not one.
- **"Visibly stale" is not a weakness to be engineered away.** On iOS it is the
  only truthful behaviour available, so the design goal is a *fast and honest
  resume* — re-subscribe promptly, show connection state plainly, and land
  in-flight adjustments on a defined value — rather than a pretence of
  continuity. On Android a companion can remove the staleness entirely; that is a
  bonus on one platform, not the baseline the others are held to.

Modifier vocabulary is **deferred until the needs are clear**, then designed
once across desktop, tablet and hardware together — two of the three have no
keyboard, so per-feature decisions produce shift-clicks with no touch
equivalent.

### 3.18 Plugin hosting and the live rack

**VST3 / AU / LV2** — all three. JUCE covers VST3 and AU natively and LV2 since
JUCE 7, which is what makes the Linux target real. Rewriting everyone's relied-on
plugins is not an option.

A **built-in plugin set** covers basic needs and runs in-process, since it obeys
the audio-thread constraint (§4.2).

A **low-latency live plugin rack** for processing live input, so a second app
(LiveProfessor and similar) is not needed. This is the reason such apps are
separate processes: when a plugin dies, the show survives. Therefore:

- **Third-party plugins are hosted out-of-process by default**, with an opt-in
  to inline.
- **Synchronous** cross-process: shared-memory buffers, audio thread signals the
  plugin process and waits with a **hard deadline**. In time → zero added
  latency. Missed → last buffer or silence, **strip marked failed** —
  and marking it failed must *stop calling the proxy*, not merely annotate the
  UI; see the amendment below. Degradation instead of dropout.
- **Plugin scanning is always out-of-process** — that is where most plugin
  crashes actually happen, and a rescan taking down the app during a get-in is
  the classic failure.
- **PDC works for playback and cannot work for live input.** You can align,
  never recover. The rack has a stated latency budget and a plugin that blows it
  must say so loudly rather than smear timing silently.

Also: **crude timeline groups** for simple editing (topping and tailing media).

*Amended in 0.8 — `docs/spikes/spike07-proxy-plugin.md`.* The architecture above
is **feasible, and the healthy-case cost is negligible.** A custom Tracktion
plugin type wrapping a shared-memory round trip to a second process measured
**0.9 µs at p50** — three hundredths of one percent of a 128-frame block at
48 kHz — with zero missed deadlines, and playback survived the child process
being killed mid-run, degrading to passthrough exactly as described.

The wait is a **bounded spin on a lock-free atomic**, which is the only shape
that satisfies §4.2: a condition variable, semaphore or sleep all enter the
kernel. No allocation, no lock, no exception, no syscall — the sole concession is
reading a clock once every sixty-four spins. So §3.18 and §4.2 do not collide,
which was the open risk.

**The deadline is the price of failure, not of success**, and this is what the
"stated latency budget" below has to account for. The healthy round trip costs
0.9 µs *whatever the deadline is*; a **dead** child costs the **full deadline on
every block, indefinitely**, because nothing in the mechanism notices it has
died:

| deadline | cost per block while healthy | cost per block once dead |
|---|---|---|
| 100 µs | 0.9 µs | 3.8 % of the block |
| 250 µs | 0.9 µs | 9.4 % |
| 500 µs | 0.9 µs | 18.8 % |

Five failed strips at 500 µs consume the entire block. Hence the requirement
added above: **"strip marked failed" must disable the call**, or one crashed
plugin quietly eats the audio budget of the whole show. And the budget is
properly stated as *deadline × maximum simultaneously-failed strips*, not as a
single number.

Choosing the deadline follows from the same table — the smallest value that
misses nothing while healthy, since that value is also what a failure costs. On
the development machine 100 µs already missed occasionally and 250 µs did not.

Two things the probe deliberately did not cover: the child applies a gain rather
than hosting a real plugin, so the figures are the *transport*, not the
processing; and on Apple Silicon a sandbox would additionally need the **child
process** inside the audio workgroup, which is the problem Apple designed
workgroups for and is separate work.

### 3.19 Video

Built in, by decision (§2, no Alt-Tab). Millumin over OSC remains the escape
hatch for shows that outgrow this. Video must never sit in the GO path.

**Codec:** **HAP** family as the primary playback format, with a few fallbacks for
convenience (to be chosen). Not a general media player.

#### 3.19a Surfaces and mapping

- **One display per internal surface** (v1). No surface spanning multiple
  outputs.
- **Bezier mesh warp**, not just four-corner keystone: the mesh subdivides
  further so a surface can be fitted to walls, cycloramas and objects that are
  not flat.
- **Capture inputs** — camera and video grabbing. A capture device feeds one
  consumer, so it is an **exclusive resource** and goes straight into the §3.9c
  allocator alongside slots and faders; the live-range analysis applies
  unchanged.
- The **mesh editor inherits the curve editor's rule**: numeric entry on control
  points, not only dragging. A subdivided mesh is hundreds of floats in the
  document — which is exactly where the locale rule (§3.20) bites. One `fr_FR`
  comma in a coordinate list makes the show file unparseable.
- Mesh adjustment is a prime **tablet** case (§3.17): walk to the projection
  surface instead of squinting from the booth.

#### 3.19b Compositing chain

Missing from QLab and wanted: **overlay operations** and **colour grading, per
cue and per display**.

Explicit order, because it is not arbitrary:

```
source / capture
  → per-cue grade           (creative)
  → composite into the surface's flat canvas   (blend mode + opacity)
  → bezier mesh warp
  → per-display grade       (calibration)
  → output
```

Compositing happens in **undistorted space**; the whole canvas is warped once.
Faster and correct.

The two grades sit at different points for different reasons: per-cue is
creative; per-display is calibration — matching a second projector, compensating
a coloured or grey wall. **ASC CDL** (slope / offset / power + saturation) is the
vocabulary for both, since video departments already exchange it, with **3D LUT**
support alongside.

Two decisions that must be explicit rather than emergent *(both proposed; expect
adjustment in practice)*:

- **Layer order is an integer per cue**, never cue list order. List order changes
  constantly during tech and would silently restack the image.
- **Blend space** — linear light vs display space gives visibly different results
  for add and screen. Millumin and Resolume composite in display space, so
  matching them is probably right for user expectations, but it is a *stated
  choice*, not an accident of the shader.

#### 3.19c Latency compensation — user-set, audio adjusted to video

External processing times are not available to Go.dot; the program cannot guess
a rig. **The user sets the delays.**

**Video is the fixed reference; audio is adjusted to match it.** Screens and
projectors rarely move on stage, so the video path's latency is stable for a
production and is treated as the anchor. Audio is delayed (or, if the audio
chain is longer, video is) to line up.

Model:

- a **user-set offset, signed either way** — audio-late or video-late — because
  a long audio chain can exceed a projector's internal delay and vice versa
- **per-destination** *(proposed refinement)*: audio through a WFS-DIY slot
  carries the processor's buffering and possibly a network hop; audio to a bus
  does not. If a single offset per cue proves insufficient for shows mixing
  spatialised and direct cues, the offset moves to the destination and the engine
  sums per cue. Start simple; promote only if needed.
- **WFS-DIY users will want minimal processor latency** to avoid variable delay,
  which keeps the audio side predictable and the correction small.
- Offsets are rig properties, not show properties: store in **machine-level
  config, not the show file** *(proposed)*.

**Guidance, later:** a test clip — black/white inverting every 0.5 s with a beep
on each transition — and a UI to nudge until they coincide. Cheap to ship once
the rest exists; not v1.

#### 3.19d Sync and drift

Drift is the deeper problem, and an offset cannot fix it: an offset corrects a
constant, drift is a rate error, so any offset dialled in is correct exactly once.
QLab tracks drifting noticeably apart is the symptom to design against.

**Cause:** each playing object running its own clock. A cue scheduled against the
system clock while audio advances on the hardware clock diverges; two cues each
doing that diverge from each other.

**Fix — already the architecture (§3.4):** one sample-derived timebase, every
position derived from it.

- Two cues started on the same tick **cannot** drift relative to each other,
  because neither counts time independently — both read the same sample counter.
- **Audio is the master clock.** The hardware audio clock is the most stable thing
  in the machine, and a dropped audio buffer is audible while a dropped video
  frame is not.
- **Video frames are presented against the audio position** plus the destination
  offset — never on vsync alone. Long-file divergence then appears as an
  occasional repeated or dropped frame rather than a slow slide. Never the
  reverse direction.
- The 50 Hz data tick is derived from the same sample time, so a trajectory lane,
  a video frame and an audio sample agree **by construction**, with nobody
  maintaining it.

**What software cannot fix: two clock domains.** Two audio interfaces have two
crystals. At ±50 ppm that is ~360 ms over a two-hour show; even ±10 ppm pro gear
gives ~72 ms. Therefore:

- **Supported configuration: single device, or clock-locked** (word clock, Dante,
  AVB).
- **Multi-device requires adaptive resampling** with a slow control loop against
  the master — implementable, but **v1.x**, and stated honestly rather than
  silently mediocre. (macOS aggregate devices paper over this with resampling of
  variable quality; Windows and Linux mostly do not try.)
- Since drift cannot always be prevented, it is **measured and displayed**: a
  clock-skew readout per device, so a technician *sees* divergence accumulating
  instead of hearing it in act two.

**Open:** DeckLink I/O vs GPU output for latency and sync.

### 3.20 Document format

**Canonical, editable, diffable XML.** JUCE ValueTree serialisation, so document
+ undo + diff share one substrate, and the per-domain undo histories from
WFS-DIY port directly.

Canonicalisation rules:

- stable IDs on every object
- one element per line, sparse attributes
- deterministic attribute ordering
- **locale-independent number formatting** — force C locale on write, fixed
  precision, always a dot. `fr_FR` writing `0,5` into a show file is a
  premiere-night bug.
- derived/ephemeral state (playhead, window geometry, selection, meters, run
  state) in a **separate file in the bundle**
- multi-file bundle: cue lists, surface layout, device profiles diff and merge
  independently

**RELAX NG schema** shipped → validation as a pre-commit hook. **XPath** gives
the projections nearly for free.

**No document-as-program.** Loading a show must never execute it. Scripting
appears as explicit **script nodes** in the dataflow graph: code in a marked
field, sandboxed, limited API, hard time budget per tick.

Most QLab-AppleScript use is *document-time*, not show-time: batch renumber,
retarget, generate sequences, conform to a venue's patch, import from a
spreadsheet. With canonical XML, stable IDs and a published schema those are
external tools in any language and need **no API surface at all**.

### 3.21 Control-rate dataflow graph

Sources (tree nodes, incoming OSC/MIDI/serial/sACN/PSN) → processors (scaling,
logic, conversion, script node) → sinks. The three binding lifetimes give
background bridges, cue bindings and macros from one construct. A background
list (§3.5) is the cue-shaped face of the same thing.

Strictly control rate. Never in the audio callback.

### 3.22 OSC device templates

A device that speaks OSCQuery **describes itself**; templates are only needed for
devices that cannot. So the template format **is** an OSCQuery namespace
description: generate one by querying a compliant device, hand-write one for a
stubborn console, and the engine cannot tell the difference. ADM-OSC ships
built in. Community templates live in the same repo as device profiles.

### 3.23 Choufleur integration (script following and cue prompting)

**Choufleur** — `github.com/pob31/choufleur_prompt`, MIT OR Apache-2.0 — is a
script GPS for theatre technicians, from the same author. A Rust server takes
per-actor feeds from the console (Dante or direct outs, up to 16 channels), runs
offline Whisper recognition against the known script, and serves a web client to
any device on the venue network. Each operator joins the show, picks the cue list
that concerns them (LX, sound, video, flys, SM), and sees the shared script
position, their own cues and their own notes. Phase 0 has tracked two full
performances at the Comédie-Française. A wrist buzzer (XIAO nRF52840 + DRV2605L,
one-byte opcodes over Web Bluetooth) gives peripheral warnings: standby, final,
tracking-lost-with-cue-near.

**Choufleur never triggers anything** — its own stated principle, and reciprocally
Go.dot's rule that no external source moves standby (§3.5). The two agree without
negotiation.

#### Ownership

**Choufleur owns the script and where the show is in it. Go.dot owns the cue and
what firing it does.** A Choufleur cue line is a *pointer* — an ID plus a label —
never a copy of the cue. Copies reinvent the paper conduite, with the same
transcription errors at higher speed.

#### Data flow

| Direction | Nature | Content |
|---|---|---|
| **Go.dot → Choufleur** | read-only, structural | the cue list (number, name, **stable ID**, tags) so the sound operator's column is *populated from* Go.dot rather than transcribed separately; plus standby and running state, so the script shows where the show actually is |
| **Choufleur → Go.dot** | rehearsal convenience only | jump-to-cue from a note or cue line → moves the **edit pointer** only. Setting standby stays a separate deliberate action. |

One authoring pass instead of two: renaming, renumbering and retargeting during
tech propagate automatically, and the discrepancy that would otherwise appear at
the dress rehearsal cannot occur. Go.dot exposes ID→name resolution over
OSCQuery; a note whose ID no longer exists displays as **dangling**, never
silently inert.

**Cue identity is the bridge.** Choufleur has its own notation and line-identity
spec (`docs/choufleur-notation_1.md`); the mapping between its cue references and
Go.dot stable IDs is the one thing that must be agreed. This makes Choufleur the
first external consumer of the visible-and-copyable ID requirement (§3.5) — cue
*numbers* are renumbered every tech and are unusable as a reference.

#### The pane

The split view is a **layout preset** (§3.17), not a mode: script + notes beside
an *independent, non-following* Go.dot cue list. Scrolling stays decoupled —
tracking can lose confidence, and an unreliable source must never scroll the
operator's list away from where they are. If the recogniser's guess is wanted in
the cue list, show it as a **scalar** ("script is 2 cues ahead of standby"), not
as a fourth marker in a dark room.

Pane contract: **takes a URL, emits cue-ID selections.** Fillable either by an
embedded webview or by an external window docked alongside. **Decision open.**
The lean is embedded — it gives layout stability (the split view is a preset,
locked in show mode, no window-size fiddling) and makes jump-to-cue a pure
selection with no GO action — but two issues decide it: layout stability, and the
BLE wearable, which an embedded page almost certainly cannot reach. **Therefore
Go.dot relays the buzz** (see "Alerts as a device"), and the embed question is
settled once the pane exists.

**Webview caveat:** embedding costs Web Bluetooth. WKWebView and WebKitGTK have
none, and WebView2 requires the host to implement the chooser. Choufleur's buzzer
therefore does not survive being embedded.

#### Alerts as a device, not a feature

Resolution: the wearable becomes an **output device in the parameter tree**, not
a Choufleur-only channel. Then any cue, any list and any client can raise it —
Go.dot standby warnings, a fader-start cue armed on a bank the operator is not
looking at (§3.9d), a verification failure, an AD's summons — using the opcode
vocabulary Choufleur has already defined (`buzzer/README.md`), on the same wrist,
with no second wearable.

Implementation: **out-of-process sidecar**, for the same reasons as plugins
(§3.18). BLE stacks share nothing across platforms and JUCE abstracts none of
them; `btleplug` (Rust, MIT/Apache) covers all three, and the Rust-sidecar pattern
already exists in S21-HiJack. Plain GATT needs no bonding, so
scan → connect → write → disconnect and reconnect-by-address is the normal
lifecycle, not a workaround.

Constraints: BLE latency is a connection-interval affair — tens to low hundreds
of ms, non-deterministic — so it is fit for warnings and unfit for anything
time-critical. It shares 2.4 GHz with the tablet's wifi. **Link failure must be
visible, not silent**: heartbeat, status indicator in the running pane, alarm on
loss. Choufleur already does this at both ends; Go.dot must match.

#### Licence direction

Choufleur is MIT OR Apache-2.0; Go.dot is GPL-3.0. **Go.dot may consume Choufleur
code; Choufleur may not consume Go.dot code.** Any component wanted by both — the
BLE sidecar, an OSCQuery client crate, the buzzer opcode table — must live in the
permissively licensed repo and be pulled into Go.dot, never the reverse.

### 3.24 Ranges and in-cue loops (media and curves)

**Not the same mechanism as group loops (§3.6).** Group loops repeat *cues* —
discrete children, relaunched, each with its own start and completion. Range
loops repeat *time inside one running object* — the playhead returns, the cue
never stops, nothing is relaunched. Different owner (the media or curve object,
not the cue scheduler), different implementation. Only the vocabulary is shared:
iteration counts, `stop after current iteration`, a visible `3/8`.

Applies to **media cues (audio, video)** and to **OSC/MIDI cues entered as
curves** (fades and lanes, §3.10).

#### Ranges

A cue's content carries a **range list**: ordered regions of the file or curve,
each with an in-point, an out-point, a **loop count from 1 to infinite**, and a
name. Playback walks the list.

- **Entry points, out-points and playback rate are user-editable and exposed as
  nodes** — accessible over OSC like any other parameter.
- Edits made during playback take effect **at the next iteration**, never
  retroactively *(proposed)*.
- Ranges need not be contiguous nor in file order *(proposed)* — a media cue is
  then a playlist over one file, which covers alternate takes and versioned
  sections without duplicating media.
- **Ranges and loop counts are copyable between cues.** This is the third
  instance of the same gesture (after output assignment and fader binding), so it
  is **structured copy-paste of named field groups** rather than a feature per
  field *(proposed generalisation)*.

#### Advancing

A targeting cue (§3.8 — same object as the stop cue, different verb) can tell a
running media or curve cue to **finish the current pass of the current range and
continue into the next range** — which may itself loop, or be the end of the
file. Verbs: `advance` (at the current range's next join), `advance at range
end`, alongside the stop verbs.

This is the pattern QLab handles worst, done properly: an ambience whose first
range loops indefinitely, a transition range after it, then the outro. She
exits, the operator fires an advance, the loop finishes its pass and moves on.
Because nothing restarts, the **slot, fader binding, grade and any bindings all
persist** across the transition — which a group loop could never give, since each
of its iterations is a fresh launch.

Same for curves: a fade with a looping range is an LFO with a defined shape,
cycling until an advance releases it into the next range and on to its endpoint
— "wave until she exits, then settle."

#### Rate

**Varispeed or time-stretch, as a toggle per cue.** They are different tools:
varispeed pitches and is the creative one; time-stretch preserves pitch and costs
CPU and quality. Neither is a global preference.

Rate is a node, so it is automatable, fader-bindable and can carry a lane. Rate
changes apply to the **whole cue**, driven off the sample clock, so audio and
video move together (§3.19d).

#### Joins

- **Sample-accurate loop points**, with an **optional short crossfade at the
  join** *(proposed)* — a hard cut at a loop boundary is audible on anything with
  tail.
- Frame-accurate equivalents for video.

*Amended in 0.8 — `docs/spikes/spike03-join-quality.md`.* Sample-accurate loop
points are **confirmed**. The crossfade is **not obtainable from Tracktion**, and
not merely unexposed: what exists at a follow-action boundary is a one-sided
decay of the *outgoing* clip that **overwrites** the incoming audio (a fixed
≤40-sample ramp, assigned rather than mixed), and clip fade nodes are skipped
entirely for launcher clips. It is a click suppressor and does that job well.

So a crossfaded join has to be built by Go.dot: two slots playing simultaneously
with a §3.10 curve on each. That is **measured to work** — two overlapping copies
of the same file are sample-aligned, so the sum is exactly twice the source with
no comb filtering, which is the failure mode that would have made the idea
unusable. The *(proposed)* crossfade is therefore a **cost** decision rather than
a capability question: one extra slot and one curve per crossfaded boundary, out
of §3.9c's allocator budget. Still awaiting a yes or no.

#### Running view and solver

- A range-looping cue is **one run pointer** with an internal position: its strip
  shows range name and iteration count. No nesting in the running pane.
- The state solver (§3.13) reconstructs "which range, what phase" — the same
  position work it does for a plain media cue, plus a range index.
- **Infinite-loop ranges inside a load-to-time are an edge case to solve in
  practice.** If *T* falls while a cue would be in a range with an infinite loop
  count, the pass and phase are not reliably computable — the exit is
  operator-timed. Direction: test what works, guard where needed, and if
  necessary offer a **loop-specific UI at load-to-time** that lets the user
  confirm which range and iteration each looping cue should be in before
  committing — effectively the state-position pointer made editable per running
  object. The honest fallback is also acceptable: **tell the user** that the
  target time is past a manually exited loop and load-to-time cannot know where
  that cue would be — a confused solver that says so is better than one that
  guesses. Not to be designed on paper; check how QLab behaves here first.

### 3.25 Audio engine: Tracktion Engine — decided

**Decision: Tracktion Engine (GPL-3 option) is the audio engine.** The decision
holds on one condition, which is the architecture: **Go.dot never asks TE to be
the show.** TE's timeline, tempo map and single transport are the wrong shape for
a cue list; every friction listed in earlier drafts came from imagining cues as
TE structures. Inverted — **Go.dot owns time; TE is a sample-accurate polyphonic
player with a plugin graph that Go.dot commands** — most of those frictions
dissolve.

#### One Edit, one transport, running continuously

The transport starts at the top of the show and is never stopped. Set a fixed
tempo and ignore it. Parallel cue lists do not need parallel transports — they
are different sources of launch commands against one running clock. This is
where drift immunity (§3.19d), sample-accurate scheduling, and a single timebase
for video and the 50 Hz tick all come from, for free.

#### Cues are launcher clips in pre-allocated slots on a fixed track set

**Fixed graph, dynamic content.** Launching a clip into an existing slot does not
change the playback graph; inserting clips or tracks *during* playback does, and
TE rebuilds its graph on structural edits — a rebuild on every GO would be an
audible tax on everything already playing. So the track set is fixed at show
load and only slot contents change.

- The cost is a **polyphony ceiling**: *N* tracks = *N* simultaneous cues. Every
  theatre playback engine has one.
- The launcher slots **are** the exclusive resources the §3.9c allocator hands
  out: prepare loads the clip into a free slot, GO launches it. Same object, no
  translation.

*Amended in 0.8 — `docs/spikes/spike04-graph-stability.md`, `spike01-bus-routing.md`.*
Both halves are now measured rather than assumed.

- **"Launching into an existing slot does not change the playback graph" is
  confirmed**, and it is provable rather than merely observed:
  `EditNodeBuilder::insertOptionalLastStageNode` is called on every graph build,
  so a counter on it measures rebuilds directly. Sustained launching into a fixed
  track set produced **zero rebuilds** at every track count, sample rate and
  buffer size tested, and already-playing material came back bit-for-bit
  identical. `Edit::TreeWatcher` is the exhaustive list of what *does* force a
  rebuild, and `TransportControl::ReallocationInhibitor` suppresses one while a
  batch of edits lands.
- **The ceiling has no width component.** Tracktion capped a track at two
  channels until v3.2.0; on the pinned engine that cap is gone and a track
  carries discrete multichannel content (verified to 8 channels, each carrying
  its own distinct signal). So a cue wider than stereo is still **one** cue in
  **one** slot, and "same object, no translation" holds for mono, stereo and wide
  cues alike.
- **The fixed-track-set discipline extends to routing.** Changing a track's
  output device is a structural edit and does rebuild the graph, so destinations
  are assigned at show load along with the track set, not per cue.

#### Ranges map onto follow actions

*Amended in 0.8 — `docs/spikes/spike03-join-quality.md`.* Measured, with three
results and one of them awkward:

- **The join is sample-accurate.** Two clips that are consecutive halves of one
  file reconstruct that file exactly across a follow-action boundary.
- **There is no `advance` verb in Tracktion.** Its follow-action vocabulary is
  rich but entirely self-triggered; nothing fires one programmatically. So
  `advance` is Go.dot calling launch on the next range's clip, which means a
  range list is *N* clips in *N* slots rather than one clip with *N* regions.
  §3.24's "nothing restarts, so the slot, fader binding, grade and bindings all
  persist" still holds — but that persistence is **Go.dot's**, not the engine's.
- **The boundary corrupts a short window of audio.** The switch happens on a
  block boundary while the *position* stays sample-accurate, so the damaged span
  is up to one block plus a fixed ~40-sample click-suppression fade — 0.67 ms at
  a 32-frame buffer, **3.5 ms at 256**. It scales with buffer size, so the range
  boundary's audible quality is coupled to the latency budget rather than free.

A range (§3.24) is a launcher clip over a region of the file; "loop N then next"
is a follow action; `advance` is a programmatic launch of the next clip. The
uncertain part is join *quality* — sample accuracy and a crossfade at the
boundary without a custom clip type. A spike, not a blocker (§6.1 #3).

#### Plugins and the rack are where TE earns its keep

Racks are a modular plugin graph; PDC, multichannel busses, VST3/AU via JUCE and
LV2 via JUCE 7 are present. Sandboxing (§3.18) is not built in, but TE allows
custom plugin types, so the **out-of-process host is a proxy plugin wrapping the
IPC** — real work that TE permits rather than prevents. **Watch PDC on live
input**: TE may insert delay to align a live track with the rest of the graph,
which is exactly what the rack path must not have (§6.1 #6).

*Amended in 0.8 — `docs/spikes/spike06-rack-latency-pdc.md`,
`spike07-proxy-plugin.md`.* **The PDC warning is confirmed, and it is the most
consequential finding of the seven spikes.**

Delay compensation is **global**. A plugin declaring 250 ms of latency on **one**
track delays **every other track's file playback by exactly 250 ms** — measured
at 50, 100 and 250 ms, identical whether the plugin sits bare on the track or
inside a Rack. The tracks stay perfectly aligned with each other, which is PDC
working precisely as designed; the problem is that "aligned" is achieved by
making everything late. **This collides with §4.1** — a reverb on a live
microphone would put its latency between the GO button and every cue in the
system.

**But it can be switched off, through the public API, with one call.**

```cpp
edit.setLatencyCompensationEnabled (false);   // tracktion_Edit.h:542-543
```

Measured across 50/100/250 ms of plugin latency, at 48 and 96 kHz, at four buffer
sizes, bare and inside a Rack — **every configuration gave a file-path shift of
exactly zero**. The latency track stays late by exactly its own plugin's latency,
which is that plugin doing its job; nothing else is dragged along with it. And
the Edit still *reports* the latency through
`EditPlaybackContext::getLatencySamples()`, so Go.dot can know the number and
apply an offset itself wherever it decides one belongs — §3.19c already
contemplates exactly that mechanism for video.

The call is wired end to end: `Edit::setLatencyCompensationEnabled`
(`tracktion_Edit.cpp:2416`) stores the flag and calls `restartPlayback()`, and
`EditPlaybackContext`'s `setNode` (`tracktion_EditPlaybackContext.cpp:205`)
re-reads it on **every** graph build and forwards it down to `SummingNode`,
`ConnectedNode` and `RackReturnNode`. It is not a private hook, not a fork, and
not something to ask Tracktion for: it is a documented method with the comment
*"Can be used to disable latency compensation when playing (it is enabled by
default)"*.

So this is a **configuration decision, not an architectural one**, and §4.1 is
satisfied without giving anything up. What it costs is that Go.dot then owns
alignment — which is what §3.25 says it should, since it owns time.

One genuine non-escape, recorded because its name suggests otherwise:
`Edit::setLowLatencyMonitoring` left the shift **unchanged**. Its mechanism is to
shrink the device buffer and **bypass** the listed plugins, i.e. remove the
latency by removing the plugin — incompatible with a rack whose purpose is to be
in circuit. It is the wrong lever.

**Scope: nothing before Phase 9 is affected either way.** PDC only engages when
something in the Edit *declares* latency. Recorded-media playback declares none,
Go.dot's built-in plugin set is its own to keep at zero, and the out-of-process
proxy (§3.18) declares zero by design.

The proxy plugin itself is **feasible and cheap**: a custom TE plugin type
wrapping a shared-memory round trip to a second process measured **0.9 µs** at
p50, held a hard deadline inside the audio callback, and survived the child being
killed mid-playback — degrading to passthrough exactly as §3.18 describes. It
declares **zero** latency deliberately, since by the paragraph above any latency
it declared would delay the whole show.

#### Rate, video, timecode

- **Varispeed** = speed ratio with stretch off (resampling). **Time-stretch** via
  Elastique or RubberBand, licence permitting.
- **Video stays Go.dot's**, reading TE's playhead each frame (§3.19d).
- **Timecode chase** = TE's transport following MTC (Waveform does it; verify the
  engine exposes it). LTC is decoded by Go.dot and fed to the same mechanism.

#### Curves never enter TE

Go.dot's control graph evaluates fades and lanes at the tick; when a lane is
bound to a clip, it reads that clip's position from TE to stay locked. **No
custom TE clip type, no closed-factory fork, no `OSCClip`.** This removes what
was the single largest risk in the TE plan, and it was self-inflicted.

#### The Edit is generated, never stored

TE's Edit XML is a *rendering* of the show document (§3.20), produced at load.
The document is authoritative; the Edit is disposable. TE's ValueTree/UndoManager
still serves the in-engine undo model.

#### Residual risks, stated

TE will keep offering tempo-based abstractions to route around; JUCE version
coupling is heavy; and if the launcher cannot start a clip at an arbitrary
in-file offset, load-to-time has a real gap (§6.1 #2). The alternative —
hand-rolling streaming, stretch, plugin hosting, PDC and multichannel routing on
bare JUCE — is the two-to-three years, and the existing RT experience is in
rendering, not DAW-style playback graphs.

*Amended in 0.8 — `docs/spikes/spike02-launch-offset.md`.* **The in-file offset
risk is withdrawn: the capability is present.** The launcher path passes the
clip's offset through (`EditNodeBuilder`, launcher branch), and a measured ladder
of offsets landed on the **nearest sample** to the requested position at both
48 and 96 kHz. Load-to-time can place a cue anywhere in a file and trust the
result.

What replaces it is a **cost**, not a gap, and it shapes where the work happens:
`IDs::offset` is in `Edit::TreeWatcher`'s restart list, so **setting an offset
during playback rebuilds the graph**, once per output device.
`LaunchHandle::nudge` does not. So load-to-time sets offsets **in prepare**
(§3.12) and uses nudge for anything already playing — which gives the prepare
step a second, mechanical reason to exist beyond anticipation.

The JUCE version coupling proved real in the other direction too: Tracktion
v3.2.0 does not build against JUCE 9, and Tracktion's own development branch was
still on JUCE 8 at the time of writing. Linux native multitouch is a JUCE 9
feature (`JUCE_USE_XINPUT`, XInput2 touch events), so **that surface is gated on
Tracktion adopting JUCE 9** and cannot be unlocked from Go.dot's side.

---

## 4. Constraints as law

1. **The GO path is small, boring and merciless.** GO never blocks.
2. **The audio thread is a lipogram**: no allocation, no locks, no exceptions,
   no syscalls, no logging.
3. **Undo, crash-safe autosave, and a GO button that never blocks** are why
   anyone trusts show software.
4. **Three levels of stop, three distinct guarantees:**
   - **Esc — graceful abort.** Stops running cues and **runs footers**. Same code
     path as normal completion, entered early: a group aborted at 04:12 releases
     its channels and kills its LFOs exactly as it would have at 06:00.
   - **Double Esc — immediate.** Drops all actions, **skips footers**, kills all
     internal processing including live effects. Scope is **everything Go.dot
     originates** — it does not mute the PA or shut down the projector. The world
     may be left in a state nobody declared; that is the price of an emergency.
   - **Go Doh! — recovery.** Aimed at the failure that actually happens weekly:
     a trigger fired ahead of time. Recovery-oriented, not destructive.
     *Specification deferred* until the full inventory of in-flight objects
     exists.
5. **Undo of a GO is *revert*, not undo.** Restores standby, releases bindings,
   re-asserts pre-GO state; honest that the audio already escaped. A double-GO
   caught inside the anticipation window is fully recoverable and is the common
   case.
6. **Every parameter has a defined resting state** (§3.3 panic value).
7. **Easter eggs live where idle eyes wander** — splash, About, empty states,
   idle strings. Never in the error path.
8. **Colour is never the sole carrier of information.**
9. **The controller arrives knowing nothing and leaves knowing nothing.**
10. **The document holds what someone decided**, never what the machine happened
    to be doing.
11. **Every gesture-reachable action exists as a named command.**
12. **Containers describe behaviour; content describes output.** Nothing
    inherits downward. Groups organise time, order and lifetime; cues own
    outputs, media and parameters. Repetition of an assignment is multi-select
    edit, not inheritance.

---

## 5. Phasing

Indicative only. Implementation will be broken down into many subphases as it
proceeds; the columns below say what belongs to the first complete product, not
the order in which it gets built.

**v1** — cue lists (parallel) + groups + triggers, audio playback with ranges
and in-cue loops, built-in video, bindings and DCA, fader linking with
fader-start/stop, fades and lanes, closed-loop cues, prepare/commit with
headers/footers, state solver with waypoints, timecode chase+generate, Mackie
surface profile, tablet client, plugin rack, XML document + schema.

**v1.x** — further surface profiles (HUI, OSC, Icon/Behringer/PreSonus, Stream
Deck, SpaceMouse), script nodes, engine redundancy, experiment protocol runner,
latency-alignment test clip.

**Later** — MQTT, deterministic replay tooling on tick-indexed input logging.

---

## 6. Open questions and spikes

### 6.1 Tracktion Engine — **decided** (see §3.25); validation spikes remain

The decision rests on the inversion in §3.25: Go.dot owns time, TE is the
player. The spikes no longer gate the choice; they validate the model and locate
its edges. All seven are answerable in about a fortnight.

1. Launcher clip → arbitrary multichannel bus routing at target channel counts.
2. **Launcher start at an arbitrary in-file offset** — load-to-time depends on
   it; if absent, this is the one genuine gap.
3. Follow-action join quality: sample-accurate? crossfade at the boundary
   achievable without a custom clip?
4. **Graph stability under sustained launching with a fixed track set** — no
   rebuild, no crossfade tax on already-playing material.
5. External parameter control at 50 Hz without disturbing the message thread.
6. Live-input latency through a Rack, with TE's PDC behaviour on live tracks
   understood (it may insert alignment delay exactly where none is wanted).
7. The proxy-plugin sandbox as a custom TE plugin type wrapping the IPC.

*Amended in 0.8.* All seven were run; reports are in `docs/spikes/`, one file per
spike, and the index there carries the verdicts and the machine they were measured
on. In summary: **all seven pass.** Item 2's premise was false — the
offset capability is present, so the "one genuine gap" it names does not exist —
and item 6's parenthesis was correct. §9.2's rule that "the polyphony model stands
unless #2 or #4 fails" is therefore satisfied: **the model stands.**

Neither of the two unnumbered items below was addressed by any spike, and both
remain fully open. MTC needs a real or virtual MIDI port; the multiple-Edits
question was never exercised.

The multiple-Edits item is a fallback held in reserve, with no deadline on it:
the live rack stays in the single Edit.

Also verify: TE transport chasing MTC; multiple active Edits summed by the
DeviceManager (fallback if the single-Edit model proves insufficient).

*Removed in 0.7:* the `OSCClip` fork and the MidiClip-smuggling spike. Curves
never enter the audio graph (§3.25), so neither is needed.

### 6.2 Audio specification — partly specified

Destination model, slot allocation and lifecycle are now in §3.9b–c. Still open:
file playback channel counts, live input handling, device management, and the
interface channel pool that footers recycle.

*Amended in 0.8 — `docs/spikes/spike01-bus-routing.md`.* **File playback channel
counts are closed, and by hardware rather than preference:** the target interface
(RME Digiface Dante) presents **64 channels**, shared between mono direct outs,
stereo sources and mix tracks. A mixed rig of that shape — 32 mono direct outs
interleaved with 16 stereo buses — routes exactly, with every track reaching its
own destination and nothing leaking elsewhere. Mono destinations stay mono; a
track can carry discrete multichannel content where wanted.

One narrower question replaces it: at **96 kHz** the measured ceiling on
*simultaneously launched* streaming clips fell to around 40 on the development
laptop, against roughly 72 at 48 kHz. That tracks total sample throughput rather
than channel count, was not fixed by pacing the render to real time, and has not
been reproduced on other hardware — so it is recorded as a property of that
machine under that load, not as an engine limit. **It wants confirming on the
show machine before a rig is planned around it.** That confirmation does not need
the interface: the spikes drive TE through a `HostedAudioDeviceInterface` with the
device manager disabled, so their sample rate is a software parameter and the
figure is a CPU measurement, independent of what any device can be told to do.

*Amended in 0.8 — device clock ownership.* **Go.dot does not own the sample rate,
and on the target rig it cannot set it.** The Digiface Dante takes its clock from
the Dante domain, configured in Dante Controller; the interface follows the
domain, the driver reports what it is given, and the application is last in that
chain rather than first. The same is true of any interface slaved to external
clock — word clock, MADI, AES — but Dante makes it the normal case rather than
the exception, and a Dante Virtual Soundcard on the same machine sits in the same
domain.

So **the sample rate is an observed property, not a setting**, and three things
follow:

- **A show file's sample rate is a declaration of intent, not a command.** Go.dot
  can record what a show was authored at and compare it with what the device
  reports, but it cannot impose it. **Behaviour on mismatch is an author
  decision** — refuse to load, load with a warning, or adapt and resample — and
  it is not answered here. Note that §3.3's "every parameter has a defined
  resting state" does not apply: this is not a parameter Go.dot owns.
- **The rate can change from outside the application, mid-show.** Someone opening
  Dante Controller and moving the domain to another rate re-clocks the interface
  underneath a running show; the driver signals a reset and playback restarts.
  This is an asynchronous failure mode with no defined behaviour yet, and it is
  the sharp one — §4.4's three levels of stop do not cover a stop nobody asked
  for.
- **Clock lock is a first-class state to surface**, distinct from device presence.
  An unlocked device is present, enumerated and producing nothing. "No clock" and
  "no interface" are different failures with different remedies, and an operator
  needs to be able to tell them apart at a glance.

None of this bears on the seven spike results, which never opened a device.

### 6.3 Video — DeckLink vs GPU

### 6.4 Asparion — remaining asks

- Request the **byte-level list of the Mackie extensions** (OLED lines, encoder
  RGB). Reverse-engineering from four DAW packages is an afternoon better spent
  elsewhere; if they'd rather not write one, ask which package implements the
  extensions most completely.
- Push on **demo mode** — a surface that renders a layout with no engine attached
  is how the profile gets developed on a train and how a user evaluates the app
  without owning hardware.
- **Multi-DAW** may already provide multiple simultaneous MIDI endpoints. Read
  `Multi DAW [en].pdf`.

Source discipline: vendor-published files only (`.mst`, Bitwig script, Mixbus
`.map`, `D700.cpsxml`), now with explicit written permission from Asparion.
CSICode itself has **no licence** — read for facts and concepts, never copy.

### 6.5 Script language

EEL2 (WDL, permissive, no GC, compiles to machine code) for tick-path mapping
math; Lua for orchestration outside it. Verify EEL2 licence terms.

### 6.6 Restart-vs-second-instance (§3.8); banking policy (§3.9d)

Both decided step by step with the hardware.

### 6.7 Modifier vocabulary (§3.17) — after the needs are collected

### 6.8 Choufleur (§3.23)

- Mapping between Choufleur's cue notation / line identity
  (`docs/choufleur-notation_1.md`) and Go.dot stable cue IDs.
- Embedded webview vs docked external window — lean embedded; settled once the
  pane exists. Go.dot relays the buzz either way.
- Read `docs/choufleur-prd_1.md` and `docs/choufleur-notation_1.md` before
  designing the exposed cue-list namespace; the show file format is already
  specified there and should not be duplicated.

### 6.9 Proposals awaiting a yes or a no

Marked *(proposed)* in the text. The ones worth a decision before they get
built into something: stereo cue → two mono slots and processor-declared slots
(§3.9b); tag targeting (§3.8); non-contiguous ranges, edit-at-next-iteration and
crossfaded joins (§3.24); per-destination latency and machine-level storage
(§3.19c). The infinite-loop load-to-time case (§3.24) is settled as
solve-in-practice.

### 6.10 Protocol implementation order (§3.16)

Mackie vs HUI first — first week with the D700.

---

## 7. Vocabulary

Convention at the surface, novelty below the waterline. The stage manager should
notice nothing; the designer should notice everything.

| Term | Meaning |
|---|---|
| **Go.dot** | product name |
| `wfg` | binary / package |
| **Didi** | cue list pane (the one that remembers) |
| **Gogo** | running cues pane (pure present tense, no history) |
| **Go Doh!** | early-trigger recovery |
| *Rien à faire.* | empty cue list / idle state |
| *They do not move.* | show complete, all silent |
| *Répétition* | shipped demo/tutorial show |
| **Ubu** | reserved: a future assistant director's helper application |

---

## 8. Platforms and licence

**Licence:** GPL-3.0, consistent with WFS-DIY and compatible with Tracktion
Engine's GPL-3 option. Decide the per-file header notice string and whether the
copyright holder is the author personally or **Pix et Bel** — much easier at
commit one than with contributors in the history.

**Platforms:** Linux, macOS, Windows for engine and desktop UI. Tablet client
platform-agnostic. The Linux requirement is what makes Mackie-first
non-negotiable (§3.16).

---

## 9. Immediate next actions

1. Read `Multi DAW [en].pdf`, `Mackie [en].pdf`; open the CSI `.mst` and the
   Bitwig script for the D700's extension vocabulary. Request the byte-level
   extension list from Asparion.
2. Run the seven TE validation spikes (§6.1). The polyphony model stands unless
   #2 or #4 fails.
3. Draft the parameter-tree namespace and node metadata schema.
4. Draft the RELAX NG schema for the document bundle.
5. Write `CLAUDE.md` from §4 verbatim.
6. Settle the timecode-chase tick derivation (§3.14).
