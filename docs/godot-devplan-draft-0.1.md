# Go.dot — Development Plan (suggestion for Claude Code)

**Draft 0.1**, derived from PRD draft 0.7 (still current against 0.8 — the 0.8
amendments record spike results and change no phase ordering; *updated 2026-09-07* — PRD
§3.9e, §3.18 and §3.27–3.29 add work to Phases 4, 6, 8, 9, 10 and 11 and reorder nothing; each
of those phases says what below). A proposed phase order, not a
schedule. Each phase ends with something runnable and a replay-log fixture that
becomes a regression test. Phases will be broken into subphases as they go.

---

## How to read this

- **Read PRD §4 first.** `CLAUDE.md` is §4 verbatim; it is the review criterion
  for every PR.
- **Engine before UI, always.** Nothing gets a UI before the engine exposes it
  over OSCQuery and a headless test drives it. The UI is a client (PRD §3.2).
- **Anything marked *(proposed)* in the PRD is not built without confirmation.**
  Surface it as a question, then wait.
- **Decision points listed under "Needs from the author" are his, not yours.**
  Do not resolve them by picking the reasonable-looking option.
- **Spikes are throwaway.** They live in `spikes/`, never migrate into `src/`,
  and each ends with a written pass/fail against the criterion in PRD §6.1.
- **Small PRs per subphase.** One concern per PR; the replay fixture is part of
  the PR.
- The author designs the UI layout (PRD §3.17). Phase 5 does not design it: it
  grows `clients/console` into the surface he designs it in, which is also PRD
  §3.17's web client (decisions T and V, 2026-09-09), and the JUCE desktop
  client waits until the layout stops moving.

Sizes are relative: **S** days, **M** weeks, **L** several weeks. No dates.

---

## Phase 0 — Foundations and spikes · M

**Goal:** a repo that builds on three platforms, and the TE polyphony model
validated or amended before anything depends on it.

- Repo scaffold: CMake, JUCE + Tracktion Engine as submodules, GPL-3 file
  headers, `docs/` holding the PRD and this plan, `CLAUDE.md` = PRD §4.
- CI on GitHub Actions for Linux / macOS / Windows (same pattern as WFS-DIY and
  S21-HiJack).
- Run the **seven TE validation spikes** (PRD §6.1) as separate throwaway
  programs. Priority order: #4 graph stability under launching, #2 launcher
  start at arbitrary offset, then #1, #3, #5, #6, #7.
- Spike report in `docs/spikes/`, one file per spike, pass/fail plus what was
  learned.

**Done when:** CI is green on all three platforms; spike report exists; the
polyphony model (PRD §3.25) is confirmed or its amendment written into the PRD.

**Needs from the author:** copyright holder for the file header (personal vs
Pix et Bel); default fixed track count; target sample rates and buffer sizes.

---

## Phase 1 — Document and tree (the skeleton) · M

**Goal:** a headless engine that loads a show document and exposes it over
OSCQuery. No audio yet.

- **Show document** (PRD §3.20): XML bundle, canonicalisation rules, stable
  short IDs from the first object, RELAX NG schema, locale-independent numerics.
  Round-trip test: load → save → byte-identical. Ephemeral state in a separate
  file from day one.
- **Parameter tree** (PRD §3.3): node model with `kind`, `rate cap`,
  `anticipatable`, `panic value`; OSCQuery server with change notification;
  mounted namespaces as stubs.
- **Named command registry** (PRD §3.2 / §4.11): every action is a command from
  the first commit. No exceptions later.
- **Tick clock** (PRD §3.4): 50 Hz derived from sample time on its own thread,
  driven for now by a dummy audio callback.
- **Tick-indexed event log**: every state mutation flows through one ordered
  path (PRD §3.15). This is the flight recorder that later gives replay,
  regression tests and redundancy. Build it before there is anything to record.
- Cue list with a single standby pointer, manual sequence, no playback.

**Done when:** an external OSCQuery client can load a document, read and write
nodes, move standby, and the event log replays the session bit-for-bit.

**Needs from the author:** the parameter-tree namespace draft (PRD §9.3);
approval of the ID format.

---

## Phase 2 — First sound (vertical slice) · M

**Goal:** GO makes a sound, verified over OSCQuery, with no UI.

- TE integration per PRD §3.25: one Edit *generated* from the document,
  continuous transport, fixed track set, launcher slots.
- **Media cue** (audio file) → slot → bus routing matrix. GO as a command.
- **Fade cue** as a wall-clock interpolator in the control graph writing clip
  gain, with per-block slewing on the audio side.
- **Stop cue** with verbs: hard, fade-and-stop, and the targeting object that
  §3.24 will extend.
- **Closed-loop waits** (PRD §3.11): `none` / `sent` / `verified` against a mock
  OSCQuery target that the test harness controls.
- Audio thread lipogram enforced by a test: no allocation, locks, exceptions,
  syscalls or logging in the callback (instrument it).

**Done when:** *Rien à faire* → load show → GO → sound → fade → GO → next cue,
driven entirely over OSC; the replay log reproduces it.

**Needs from the author:** first real WFS-DIY namespace to mount, so `verified`
can be tested against a real target before Phase 4.

---

## Phase 3 — Groups, triggers, ranges · L

**Goal:** the full cue model running headless.

- **Groups** (PRD §3.6): timeline and sequence, auto/manual toggle, pre/post
  wait, completion semantics per cue kind, headers and **blocking footers**,
  nested scopes tearing down innermost-first.
- **Shuffle** with materialised rounds, boundary constraint, seeded and logged
  RNG; loop counts; round pruning as run-local state.
- **Run pointers**, plural, exposed over OSCQuery as selectable objects.
- **Standby semantics**: advances immediately on GO, positionally past an auto
  chain (PRD §3.5). Fire-and-forget is the auto group's nature, not a flag.
- **Triggers** (PRD §3.7): OSC, MIDI, wall clock. Timecode in Phase 10. GO
  remains the only trigger that moves standby.
- **Parallel lists**, one standby each, focus model.
- **Ranges and in-cue loops** (PRD §3.24) on TE follow actions; `advance`
  verbs; rate as varispeed/stretch toggle; join quality per spike #3.
- **MIDI cues** with every event type (Note, PC, CC, bend, aftertouch, SysEx).

**Done when:** a complex background auto-sequence with a looping ambience runs
while manual foreground cues fire on top; an advance cue exits the loop cleanly;
the replay log reproduces all of it.

**Needs from the author:** restart-vs-second-instance policy (PRD §6.6);
confirmation or rejection of the *(proposed)* items in §3.24. **Both answered
2026-09-06** — decisions L and N in `godot-namespace-draft-0.1.md` §9, along with
M (GO on a manual group) and O (a group fade is a trim). The phase's own shape is
§12 of that draft; ranges turned out **not** to map onto TE follow actions and the
line above saying they do is superseded there.

**COMPLETE, 2026-09-07.** The done-when above is `tests/blackbox/phase3_groups.py`,
twenty-nine checks against the shipped binary over a socket and read back off the
WAV, in CI on three platforms under two locales. `docs/godot-phase3-closeout-0.1.md`
is what it amends in the PRD, what it left undone, and what it measured.

One line of the list above was **not** built: *rate as varispeed/stretch toggle*.
Rate cannot change on a playing launcher clip at this Tracktion pin, so what could
be built is rate at ARM — half of what §3.24 promises — and half of it in the
document would be a row that does not do what the PRD says. It is a proposed
amendment rather than a deferral, and the engine-side change that would settle it
is named in the close-out.

*Answered 2026-09-28:* built as a live speed rather than rate at arm, once a patch
the build applies to Tracktion made a playing clip's speed movable - *Speed:
varispeed and timestretch on a media cue*, below, and namespace draft §22.

---

## Phase 4 — Prepare/commit, solver, allocator · L

**Goal:** rehearsal-grade behaviour: anticipation, load-to-time, resource
safety.

- **Prepare/commit** (PRD §3.12): anticipatable annotations honoured; headers
  pre-arm whole blocks; rollback on standby move is silent by construction.
- **Presets derived into the header** — a member's parameter marked *preset* puts
  a line in its group's header rather than making somebody author one. The
  author's idea, 2026-09-06; the four things to settle first are in
  `docs/godot-open-questions-0.1.md` §5.
- **State solver + waypoints** (PRD §3.13): backward walk over last-writers and
  cue lifetimes, run-pointer reconstruction, diff-and-send against read-back,
  event-kind exclusion, group boundaries as structural waypoints. Load-to-time
  as a command.
- The infinite-loop edge case: implement the "solver says it's confused" path
  first; the loop-configuration UI is a later decision (PRD §3.24).
- **Shared allocator** (PRD §3.9c): processor slots and interface channels,
  live-range liveness analysis, conservative overlap warnings, cross-list
  handling, claim-in-prepare and release-at-footer.
- **Slot destination model** (PRD §3.9b) with the QLab-style bus matrix kept
  as-is.
- **Slots as one concept** (PRD §3.9e, 2026-09-07): the allocator hands out
  voices, strips, rack channels and processor inputs from one table, with a
  release policy and a failure policy per kind. Two things it must not
  foreclose: the **waiting claim** — a claim on a busy slot lands when the
  holder's run ends and shows *pending* meanwhile — and **eviction as a close**,
  which is the sampler group's takeover (PRD §3.27). Phase 3's fail-at-entry
  stays the default for a voice.
- **The rack pool declared at load** (`Show/Audio/Rack`, PRD §3.18): so many
  channels per width class, each shared or exclusive. The pool and its claims
  are this phase's; the plugins inside are Phase 9's.
- **The persistent assertion** (PRD §3.29) is the solver's reconstruct-and-diff
  run over a designated set at every trigger. Build it as a mode of the solver,
  not a second mechanism; the section's UX is the author's and comes later.
- **Measure before the allocator is written:** whether the launcher keeps one
  playing slot per track (PRD §6.11). It decides what a sampler claim is
  (PRD §3.25).

**Done when:** load-to-time into the middle of a scene lands the right cues at
the right offsets with the right slots claimed; reordering cues produces the
right overlap warnings; a claim on a busy slot waits and says so; all headless.

**Needs from the author:** the *(proposed)* items in §3.9b (stereo → two mono
slots; processor-declared slots); the voices claim shape once §6.11's
measurement is in; whether a rack channel's failure policy is *degrade* (§3.9e).

**Four answered 2026-09-07** with the Phase 4 plan — decisions **P**, **Q**, **R**
and **S** in `godot-namespace-draft-0.1.md` §9. P settles §3.9b's
processor-declared slots: the **show** declares a processor's inputs and the
mounted namespace is what a validate pass checks them against, so discovery
becomes a later authoring gesture rather than a mechanism. Q settles the header
preset (open questions §5) as a mark on the member naming an ancestor group, with
the header line derived. R settles waypoints: there is no authored waypoint, only
the list's own step history. S settles the persistent section at list level and
takes §3.29's *(proposed)* running-pane kill as a **yes**. *Stereo → two mono
slots* needs no separate answer — two `Feed` rows are what it means — and
*degrade* is built as §3.9e writes it unless the author says otherwise. **Still
open: the voices claim shape**, which M16 has since answered as a measurement
(PR 4.1, 2026-09-08: a second launcher slot does not stop the first — both play
and sum) and which is now the author's choice, before Phase 6. The phase's own
shape, drawn before the code as §11 and §12 were, is §13 of that draft.

**COMPLETE, 2026-09-09.** The done-when above is `tests/blackbox/phase4_prepare.py`,
fifty-two checks against the shipped binary over UDP and HTTP, in CI on three
platforms under two locales. `docs/godot-phase4-closeout-0.1.md` is what it amends
in the PRD, what it left undone, what it measured, and what it still needs from
the author.

---

## Phase 5 — Undo, crash-safe save, and the operator client (Didi and Gogo) · L

**Goal:** the engine gains what every client needs and no client can supply —
undo, a save that cannot be half-written, an autosave, an edit lock — and the
web console grows into the client the author designs the layout in.

**Two halves, and the order is decision T** (2026-09-09, namespace draft §9).
The engine half first, because none of it exists and every client needs all of
it. The client half beside it, one view per pull request, in `clients/console`
rather than in a new compiled program — because PRD §3.17 says the desktop
layout is *deliberately undesigned* and the author's to design, and a page the
engine serves from disk can be edited and refreshed while a show is running,
which is the only loop that suits work decided by looking at it. The JUCE
desktop client starts when the layout stops moving; until then it is an outline
(namespace draft §14.16). The whole phase is drawn in §14, before the code, as
§11, §12 and §13 were.

### Half A — the engine

- **Undo**, per domain (PRD §3.20, §4.3): one transaction per applied command,
  itself a logged command so a replay reproduces it. The `UndoManager` the write
  choke point has been waiting for since Phase 1 arrives — and the seam turns
  out to be less pre-cut than `ShowDocument`'s own comment claims.
- **A save that cannot be half-written**: temp-and-replace in the same
  directory, `document/dirty` assigned for the first time since it was
  published, `document.revert` and `document.saveAs`.
- **Crash-safe autosave** (PRD §4.3) into a recovery folder inside the bundle,
  never over the authored `show.xml`, decided by a hook and applied by a
  handler like every other engine-origin record.
- **The edit lock** show mode needs (decision W): one engine node every client
  honours, refusing document mutation while GO, the standby, every run gesture
  and writes to mounted rig parameters keep working.
- **Spectral colour** (PRD §3.30): the analysis cache built at import beside the
  media, keyed by content hash, stored as a pyramid and looked up through 4.1's
  `MediaInfo` grown into a per-file record; the run's timbre published from the
  tick thread; the pyramid served over HTTP, because it is a file and not a
  parameter. The sine / noise / sweep check on the cache comes before any of it
  is drawn, and the analysis cost is measured (PRD §6.11).
- **Fade breakpoints**, so the curve editor has a curve to edit.

### Half B — the console as the operator client (decision V)

- The **rehearsal blockers** first: rows keyed by identifier, so a long list's
  scroll survives a poll, and a focus policy, so GO still fires after the aim
  slider has been touched.
- A **module split** with no build step, served from the same directory.
- Then one view per pull request, each earning a round of the author's
  feedback: **run pointers on the cue list**; the **running pane's** gestures,
  round pills and range name; **group bulk-edit** with a type filter (PRD
  §3.5); the **header pane**, written lines upright and lines derived from a
  member's preset mark in italics, double-click opening the member's inspector;
  the **curve editor** with breakpoint list and numeric entry; **layout presets**
  design/tech/show with the diagnostics behind *tech*; **show mode**, which
  reads the engine's lock and does not invent its own; the save, revert and undo
  gestures as Half A lands them; and the **coloured waveform** on the running
  pane once the cache is there.
- **Dark UI mandatory** (PRD §2), which the page already is.

**Done when:** the author runs a simple show from the desktop build in a
rehearsal room. Whether a browser on the booth machine satisfies that is
decision U: judged in the room, not now.

**Needs from the author:** the layout — he designs it, and Half B is the surface
he designs it in; the ramp's colours once the sine / noise / sweep bundle is on
screen; PRD §3.30's *(proposed)* idle-colour policy; and the twelve decisions
§14 marks as the implementer's, each taken early so it can be overruled early.

**The show settings window (2026-09-22).** The audio settings window became **Show settings**,
and its first new tab is **Network**: the boxes a show talks to, each with a name, an address, a
port and a pair of Rx/Tx switches, plus one filter deciding whether a message from a sender
nobody declared is obeyed. A device IS a mount, made editable, and one with no namespace file is
*opaque* — sent to blind, which is what a lighting desk gets. Every OSC cue gained a **target**
menu that rewrites the front of its address. Built as M-A; the author's decisions and the
mechanics are in `docs/godot-namespace-draft-0.1.md` §15. **Still owed: M-B**, the MIDI tab and
ports bound from the document, and **M-C**, the network interfaces Go.dot listens on.

**Queued behind Half B (2026-09-18):** the group join setting and the range
crossfade of PRD §3.6 and §3.24 — gap, gapless, crossfade with a user-defined
overlap. Decided, not *(proposed)*; planned and built as its own plan once the
operator client's layout has stopped moving. It is Phase 3's scheduler and the
namespace draft's placed boundary (§12.9) reused at a member boundary, plus two
fade jobs per crossfaded boundary and a prepare horizon that reaches the
incoming member.

---

## Phase 6 — Control surface and bindings · L

**Goal:** the D700 driving a show; fader-start working.

**Four decisions of 2026-09-23 shape it** — **Z**, **AA**, **AB** and **AC** in
`godot-namespace-draft-0.1.md` §9, the author asked directly and each
recommendation taken. One voice per armed member, a member that finds no free
track waiting and saying so (Z). Velocity sets the level a pad starts a clip at
and pressure rides it while the pad is held, per clip and off by default, which
amends the PRD's *"no pressure"* (AA). A **Surfaces** tab in the show settings,
and a virtual surface panel in the desktop client that plays a sampler group
with the mouse (AB). And the order: the engine and that panel first, the generic
Mackie bridge second, the D700 layer third (AC). The phase is drawn before the
code as §16 of that draft, as §11 to §14 were. The D700's protocol is the one
measured on the unit (`docs/godot-asparion-d700-protocol-0.1.md`, byte tables in
`docs/D700_CONTROL_GUIDE.md`), read from vendor-published files only (PRD §6.4).

| PR | What | Depends on |
|---|---|---|
| 6.0 | Docs first: namespace draft §16, decisions Z–AC in §9, the PRD amendments, this section | — |
| 6.1 | The MCU/D700 codec: bytes to typed events and back, pure, tested against the byte tables | nothing — beside 6.2–6.5 |
| 6.2 | Surfaces, strips and DCAs as document objects: rows, schema, fixtures, the three creates | M-B, landed at `5ec980a` |
| 6.3 | DCA arithmetic, the live trims written through `node.set`, fades aimed at a DCA | 6.2 |
| 6.4 | Strips as slots, the sampler mode, press and release, fader-start and fader-stop, velocity and pressure, eviction, refresh | 6.3 |
| 6.5 | The client: the Surfaces tab, the virtual panel, inspector rows, list and run-pane words, the page's lists | 6.4 |
| 6.6 | The bridge: MCU and pad-controller profiles, serve wiring, a MIDI test seam, the standing test | 6.1, 6.5 |
| 6.7 | The D700 layer — native display, colour, rings, two banks, the preset — and measurements M26–M29 | 6.6, the unit |
| 6.8 | Close-out: §16.12, the PRD amendments applied, this section ticked, the Phase 7 handoff | all |

What the list this section carried until 2026-09-23 named and the table does not
build — banking beyond a no-op, HUI, Stream Deck, meters, the rate endpoint
class, bindings with automation modes and update-cue capture, a mapping per DCA
assignment — is in §16.10 of the same draft, with the reason for each.

**Done when:** a fader-start cue fires from the D700 with the audio already
armed; a group DCA follows automation on motorised faders; the strip displays
show provenance; a sampler group arms onto the D700 and a bank change finishes a
playing clip before its strip switches; a DCA assigned to two cues in different
groups trims both; and a virtual surface arms a sampler group and plays it from
the mouse on a machine with no MIDI. *(2026-09-23: in this phase the automation
a group DCA follows is a fade cue aimed at the DCA — lanes are PRD §3.10's and
not built — the fader-start cue is a sampler member, and a bank change is a
second sampler group armed over the first; surface banking is §3.9d's and not
built.)*

**Needs from the author:** banking policy as it emerges (PRD §3.9d — the bank
arrows do nothing until then); the field layout on the D700's three display
rows, built as name, level-or-word and role with the cue number in the number
field *(proposed)*; the touch filter of §3.16 *(proposed)*; the idle-colour
policy of §3.30, built as authored colour at idle and timbre while sounding;
**judging the virtual panel** after PR 6.5, before anything is plugged in
(decision AC); and **the D700 on the bench for M27 and M28**, the colour rate
and the idle animation, which need a person watching the unit. The other
*(proposed)* items the phase builds as defaults are listed in PRD §6.9 under
2026-09-23, each a yes or a no whenever the author has seen it working. The
voices claim shape is answered (decision Z).

**Built on the night of 2026-09-23, on the local branch `phase6`** (not pushed;
what each PR landed, and where the build disagreed with §16, is §16.12 of the
namespace draft). Against the done-when, clause by clause:

- *A virtual surface arms a sampler group and plays it from the mouse on a
  machine with no MIDI* — **built**: Show > Surfaces..., one column per strip,
  a fader and a pad each. Pinned by `RunPaneUiTests` and heard in a real render
  by `blackbox.phase6-sampler`; **not yet judged by the author**, which is this
  clause's real test.
- *A DCA assigned to two cues in different groups trims both* — **built and
  tested** (`DcaTests`).
- *A fader-start cue fires from the D700 with the audio armed* — the engine's
  rule is built and tested (`SamplerTests`), the bridge turns a fader's bytes
  into the write that trips it (`SurfaceBridgeTests`); **not tried on the unit**.
- *A group DCA follows a fade on motorised faders* — a fade aimed at a DCA moves
  its trim, and a motor follows a trim a twentieth of its travel a tick, never
  under the hand on it; **not tried on the unit**.
- *The strip displays show provenance* — MCU scribble strips and the D700's
  three native rows and number field, byte-tested; **not tried on the unit**.
- *A sampler group arms onto the D700 and a bank change finishes a playing clip
  before its strip switches* — a second bank armed over the first closes it and
  hands each strip over as its clip ends (`SamplerTests`); **not tried on the
  unit**.

**Measured:** M29 - a full sixteen-strip D700 refresh costs the tick 0.1 ms in
Release, beside a 1.5 ms publish; in Debug the publish of the same small show
takes some 100 ms, five ticks, which is why a Debug `serve` falls behind its
audio clock. M26, once, in Debug: 68-70 ms from a press being applied to its
sound, plus up to a tick of queueing - structural, and the tick rate is the
lever.

**Still owed:** the unit on the bench for the four hardware clauses, M27 and
M28; M26 retaken on a Release build of a quiet machine; rebinding a port after
start and `midi.rescan` (M-B's debt, not paid here); and the rulings §16.12
lists.

**Designed next, not built (2026-09-23): surface pages.** The author wants the
D700's Pan, EQ, Send and FX buttons repurposed as Go.dot's own pages - the
faders on a cue's sends, the rotaries on its EQ and VST inserts, buttons as
programming shortcuts - and the same model on the Stream Deck, the Stream
Deck+ and the Icon controllers. The conversation is written down in
`docs/godot-surface-pages-draft-0.1.md`, with eleven questions still his. The
per-cue **EQ and VST inserts are built first, in another session**
(`docs/handoffs/2026-09-23-eq-inserts-and-surfaces.md`); the pages follow.
*Later the same day:* that session is **Phase 9a** below — the EQ, the plugin
sandbox and the inserts, decisions AD–AG — and two of the draft's eleven
questions (7, the EQ; 8, where the inserts live) are answered by it.

**Built 2026-09-25: the EQ and Send pages.** SELECT picks a sample strip's cue
(`surface.aim`, also a click on a running cue's name in the window); EQ puts
its EQ on the rotaries in the author's order, Send its send levels, paging and
blinking the page, `*` leaving; band colours on the surrounds, the value on
the ring, names and values on the screens. Each EQ band and send gained a
saved switch. Under the edit lock EQ and sends ride live and unsaved
(`cue/LiveEdits`), kept or discarded from a bar once the show is unlocked; the
window's EQ panel and send mixer, and the page, ride live too. Decisions AJ-AP,
namespace draft §17.14; the pages draft's §9 questions 1, 3 and 5 answered.
**Owed to the bench:** which port the master section's lights answer on, how
fast a detent should be, whether the colours read at a glance, and the author's
walk of `make_d700_bench.py`.

---

## Phase 7 — Tablet client · M

**Goal:** the running pane in the operator's hands.

- The web client over OSCQuery + WebSocket is `clients/console`, grown through
  Phase 5 into PRD §3.17's web client (decision V, 2026-09-09): ES modules served
  by the engine, no build step. *(PRD §3.17 said TypeScript; the amendment that
  drops it is carried by namespace draft §14.15 and applied to the PRD
  separately.)* Full surface layout rendering, so it is a genuine fallback (PRD
  §3.17).
- Multitouch: kill, advance, prune, playhead drag; radial and cross-axis
  precision gain, relative from touch-down; dual-touch load-to-time with the
  solve running live.
- Disconnect handling: visibly stale, in-flight adjustments land on a defined
  value.
- Per-client edit selection; standby stays engine state.

**Done when:** the author manages running cues from the house on the tablet
while the desktop shows the cue list.

**Needs from the author:** modifier vocabulary decisions as they arise (PRD
§6.7) — designed once across desktop, tablet and hardware.

---

## Phase 8 — Video · L

**Goal:** audio+video shows without a second application.

- Surfaces, one display each. **HAP** playback presented against TE's playhead
  (PRD §3.19d); fallbacks as chosen.
- **Bezier mesh** with subdivision; numeric entry on control points; tablet
  editing at the wall.
- **Compositing chain** in the stated order; blend modes; integer layer order;
  per-cue and per-display **ASC CDL** + 3D LUT.
- **Capture inputs** as allocator resources.
- **Latency offsets**: user-set, signed, stored in machine config; video the
  fixed reference (PRD §3.19c). Test clip is v1.x.
- Clock-skew readout per device.
- **Video cues on sampler strips** (PRD §3.27): opacity from zero is fader-start
  for a picture; nothing new beyond the parameter the strip trims.

**Done when:** an audio+video scene plays in sync for a full act with the mesh
aligned from the tablet.

**Needs from the author:** ~~DeckLink vs GPU output (PRD §6.3)~~ answered
2026-10-06, GPU now and DeckLink later; ~~fallback codec list~~ answered 2026-10-07,
preview only, FFmpeg as a child process, converted to HAP in the background
(namespace draft §37.5); ~~blend-space choice confirmation~~ answered 2026-10-06,
display space.

*Started 2026-10-06 and split in two* (namespace draft §35, decisions UU–VM; PRD
§3.19 and §6.3 amended). The author's order: stills first, movies second. The
words are theirs — a **canvas** is what cues are laid onto, an **output** shows
one canvas through its mapping, a cue is a **fill**, a **mask** or a
**picture**. One canvas may feed several outputs. The pictures are drawn by a
child process, `wfg video-render`, in OpenGL, each canvas offscreen and sent on
by an output sink — a window now, a DeckLink card later.

### Phase 8a — Stills: fills, masks, pictures, blending and the mesh · L

| Stage | What the author sees | Depends on |
|---|---|---|
| V.0 | Docs first: namespace draft §35, the PRD amended, this section | — |
| V.1 | A black fullscreen output on the chosen display; a fill comes up on GO, fades on Esc over the panic fade, cuts on double Esc; the renderer killed comes back in a second with the picture, the sound untouched. The Video tab, "+ video", the child and its region | V.0 |
| V.2 | Pictures: fit, fill or stretch; opacity in %; read at standby in the child, GO only reveals; carried by the bundle and Save as (restaged 2026-10-06, namespace draft §36) | V.1 |
| V.3 | Geometry and its fades: scale, offset, rotation, flips; a fade cue moves them and the opacity; the stack's order | V.2 |
| V.4 | The grade on a cue: contrast, saturation, gamma, hue, four curves | V.2 |
| V.5 | Masks and blends: a shape in canvas coordinates, feathered or inverted; add, screen, multiply | V.3 |
| V.6 | The mesh: bezier patches per output, a panel with typed control points; an output's ASC CDL; 3D LUT after | V.1 |
| V.7 | Video members on sampler strips; a DCA multiplies opacity | V.2 |

*Status, 2026-10-06:* V.0 and V.1 built (namespace draft §35.9): the video cue,
canvases and outputs in the document, the Runner's video jobs, the renderer as
`wfg video-render`, the Video tab and Identify. Waiting for the bench: a fill on
a projector.

*Status, 2026-10-06, late:* V.2 (pictures) and V.3 (geometry and its fades)
built (namespace draft §36.5). 8b drawn as §37 - movies, HAP first, a playhead
with speed and loops - every decision there proposed and waiting for the
author; its reader (M.1) built. *2026-10-07, early:* M.1-M.4 built (§37.4) - a
HAP movie plays, loops and keeps time; the bench and the author's yes owed. V.4 (the grade on a cue) built the same morning (§36.6).

*Status, 2026-10-07:* V.5 (blends and masks) and V.6 (each output's mesh and
CDL) built. The author answered the DCA on opacity (it follows the fader's
travel, never past 100 %) and the fallback codecs (a preview, then a background
conversion to HAP by FFmpeg as a child process, of the whole file or the part
used): namespace draft §37.5, WE-WI. Being built in that order.

**Done when:** a scene of fills, masks and pictures runs from the cue list onto
two outputs, one mapped onto a wall that is not flat, with Esc, double Esc and
Doh! doing to the picture what they do to the sound.

### Phase 8b — Movies, capture and DeckLink · L

HAP in QuickTime (demux, snappy, compressed textures) presented against the audio
position with the signed latency offset in machine config; capture inputs as
allocator resources; the clock-skew readout; a DeckLink output sink once its
SDK's licence has been read against GPL-3; the test clip. Drawn when 8a is on a
projector.

*Added 2026-10-08, at the author's direction* (namespace draft §44; PRD §3.19
and §6.3 amended): **the renderer on each system's own graphics, then pictures in
and out.** The renderer is rewritten on sokol_gfx (Direct3D 11, Metal, OpenGL
through EGL) with one device that draws each canvas once; then outputs send over
NDI, Spout or Syphon, video inputs feed `capture` cues, and inserts take a cue's
picture to another program and back.

| Stage | What the author sees | Depends on |
|---|---|---|
| R.0 | Docs first: namespace draft §44, the PRD amended, this paragraph | — |
| R.1 | Nothing on screen: sokol vendored, the shaders ported, the GPU's pixels held to the reference compositor with no window | R.0 |
| R.2 | Fills, pictures, masks, blends, geometry and grade through the new renderer, each display at its own refresh | R.1 |
| R.3 | Movies, zones, warps, calibration, test pattern, Identify - parity with the old renderer; the bench | R.2 |
| R.4 | The OpenGL renderer removed | R.3 |
| N.1 | An output sends over Spout (Windows) or Syphon (macOS) | R.4 |
| N.2 | An output sends over NDI, the runtime found where the user installed it | N.1 |
| N.3 | Video inputs and the `capture` cue | N.2 |
| N.4 | Inserts, on a cue; black and a warning when the return is lost | N.3 |
| N.5 | The Video tab's kinds, inputs and inserts; the inspector's menus | N.4 |

*Added 2026-10-09, at the author's direction* (namespace draft §47; PRD §3.19
amended): **working on a picture**, after the author used the video side - all
built and pushed the same night, S0-S8, each stage its own commit on main:

| Stage | What the author sees |
|---|---|
| S0 | A pick in the window aims the D700's EQ, Send and FX pages (AAA) |
| S1 | The dial's numbers without long tails (AAB) |
| S2 | A movie's strip at the top of its inspector: in and out points, loops, play, seek, a true playhead, its sound drawn (AAC) |
| S3 | A movie and its locked sound edited from either line (AAD) |
| S4 | A playing picture following its edits on the projector (AAE, AAF) |
| S5 | The picture panel: place, colour, curves and mask, dragged (AAG) |
| S6 | The picked cue alone on the video monitor, opened with the panel (AAH) |
| S7 | A movie's pictures along its strip, its cuts marked and snapped to (AAI) |
| S8 | Inserts by kind with their names written once; their send up before any cue (AAJ, AAK) |

Owed to the bench: the strip and its cuts on real movies, the panel's handles by
hand, live edits on a projector, the insert round trip with TouchDesigner, the
D700 pages on a picked cue. A movie that is not HAP has no strip until converted.

*Added 2026-10-09, at the author's direction* (namespace draft §48; PRD §3.12 and
§3.19 amended): **pictures read ahead where sounds are armed, and how ready each
cue is on its row** - the first step towards video in a sampler group, asked the
same day. All built and pushed, RA.0-RA.8, each stage its own commit on main
(RA.5 and RA.6 together):

| Stage | What the author sees |
|---|---|
| RA.0 | Namespace draft §48 |
| RA.1 | Nothing: what a GO starts first, and a movie's starting second, each written once |
| RA.2 | Nothing yet: a scene's first stills and movies named to read ahead, a missing file found |
| RA.3 | Nothing yet: the renderer reads them before GO and says what it holds |
| RA.4 | A cut to a movie on standby shows its first frame |
| RA.5 | The words: `cue/prepare` for pictures, `cue/prepareError` |
| RA.6 | A mark on every row that can be got ready - getting ready, ready, partly, missing - in the window and the browser console |
| RA.7 | A large still shown at GO without its upload in that frame |
| RA.8 | M54, the PRD, this paragraph |

Owed to the bench: M54 on long movies and files not yet in the system's cache;
frames late while a still is uploaded under a playing movie; the light on a
projector; the author's eye on the marks. The questions video in a sampler group
asks - what a fader does to a picture, where it waits, a still that never ends,
which picture lies on top - wait for the author.

*Added 2026-10-09, at the author's direction* (namespace draft §49; PRD §3.27,
§3.28 and §6.9 amended): **pictures in a sampler group** - the author's answers to
those four questions, every kind of video cue, and a movie's sound with it. All
built and pushed the same day, VS.-1-VS.9, each stage its own commit on main
(namespace draft §49.7):

| Stage | What the author sees |
|---|---|
| VS.0 | Namespace draft §49 |
| VS.1 | A video cue's inspector has the member rows, greyed outside a bank |
| VS.2 | A picture may be put in a sampler group; a movie takes its sound with it |
| VS.3 | A picture comes up from a strip, its fader its opacity |
| VS.4 | Let go, it fades out; MUTE takes it away |
| VS.5 | An armed bank's pictures are ready before the hand moves |
| VS.6 | The surface: a picture strip in its picture's colour; SELECT on a movie aims its sound |
| VS.7 | The list takes pictures into a bank by drag, drop and the add menus |
| VS.8 | A movie's sound comes with it, on a voice of its own |
| VS.9 | M55, the docs, the close-out |

Owed to the bench: M55c, a D700 fader to the light on a projector; a bank of
pictures under the hands - the tint on a strip, the fader's travel on a
projector, a movie and its sound from one fader.

Then its own round: the knob above a DCA strip (PRD §3.28, decided the same day).

*Added 2026-10-09, at the author's direction* (namespace draft §50; PRD §3.28 and
§6.9 amended): **the knob above a DCA strip** - the picture's curve and the
sound's offset, kept on each cue's DCA mark, one of each per mark, on the desk
and on the window's panel. All built and pushed the same day, DK.0-DK.8, each
stage its own commit on main (namespace draft §50.7):

| Stage | What the author sees |
|---|---|
| DK.0 | Namespace draft §50 |
| DK.1 | Two rows after the DCA in the inspector, greyed until a DCA is set; nothing moves yet |
| DK.2 | A sound offset is heard |
| DK.3 | A picture curve is seen; two marks multiply |
| DK.4 | Under the lock, live and unsaved, kept or discarded on unlock |
| DK.5 | Nothing yet: which marks a knob reaches, one rule for the desk and the window |
| DK.6 | The knob on the D700 and a Mackie desk |
| DK.7 | The knob on the window's panel |
| DK.8 | The close-out |

Owed to the bench: the detents and the screen's words with the D700 in hand; a
movie and its sound on one D700 DCA, both turned.

---

## Phase 9 — Plugins and the rack · L

**Goal:** third-party processing that cannot take the show down.

*Split on 2026-09-23 into 9a, pulled forward and being built, and 9b, what is
left. No later phase is renumbered. On 2026-09-26 9b was redrawn as the live rack and 9c
added, the live sampling channels.*

### Phase 9a — EQ on media cues, the plugin sandbox, and VST inserts · L

Started 2026-09-23 on `main` at `c3d75fe`, the day Phase 6 landed. **Four
decisions of 2026-09-23 shape it** — **AD**, **AE**, **AF** and **AG** in
`godot-namespace-draft-0.1.md` §9, the author asked directly; two
recommendations were declined and the reasons are in §17.1. The EQ is Go.dot's
own, a fixed stage on every voice written from the tick thread like the level
(AD). Inserts are a chain on every voice: the show declares a plugin set, every
voice carries it bypassed, a cue switches plugins in and carries its own values
— the PRD's bypassed stack answered yes on the voices (AE). The out-of-process
proxy is built before any VST insert (AF). The scope stops at the inspectors;
the surface pages follow in their own session (AG). The phase is drawn before
the code as §17 of that draft, as §11 to §16 were, and the plan's PR table is:

| PR | What | Depends on |
|---|---|---|
| 9a.0 | Docs first: namespace draft §17, decisions AD–AG in §9, the PRD amendments, this section | — |
| 9a.1 | The EQ DSP, pure: `CueEq`, `EqMath.h`, `EqSettings.h`, tested against magnitude responses | 9a.0 |
| 9a.2 | The EQ on every voice: `EqPlugin`, nineteen `media` rows, the arm and the live push, `eq.reset`, render test, replay fixture, driver | 9a.1 |
| 9a.3 | The desktop EQ panel — the curve drawn from the DSP's own function — and the page's rows | 9a.2 |
| 9a.4 | The plugin set as a document object: `<Audio><Plugins><Plugin>`, rows, `plugin.create`, tree, fixture, replay | 9a.0 |
| 9a.5 | Hosting compiled in, `wfg plugins --scan|--list|--catalogue`, the known list, the catalogue and its cache, the tree subtree | 9a.0 |
| 9a.6 | The proxy transport: the region, `ProxyPlugin`, `ProxyHost`, the `godot:test-gain` child, misses and the failed state, rtsan clean | 9a.4 |
| 9a.7 | The child hosting a real VST3/AU: instances, preset, parameters and enables, catalogue reporting, baseline | 9a.5, 9a.6 |
| 9a.8 | The cue's `<Fx>` entries: rows, `fx.create`, the `p<n>` door, the arm and the live push, full-loop tests, driver | 9a.2, 9a.4, 9a.6 |
| 9a.9 | The desktop FX panel, the Plugins tab, preset import, the page's rows | 9a.8 |
| 9a.10 | Measurements M30–M34 | 9a.2, 9a.7, 9a.8 |
| 9a.11 | Close-out: §17.12, the PRD ticked, this section ticked, the handoff and the pages draft's §8 answered | all |

*Status, 2026-09-23 late:* 9a.0–9a.8 on `main`; 9a.9 landed without the FX panel at the foot (the
model, the Plugins tab and the gestures are in; the component is the next session's); 9a.10 taken
on the author's box once it was quiet and written into §17.9 and PRD §6.11; §17.12, the pages
draft's §8 and the handoff are written.

*Status, 2026-09-25:* the FX panel at the foot is built, **redesigned by the author** (namespace
draft §17.13, decisions AH and AI, the PRD overridden): the signal chain with a switch a plugin;
*Edit…* opens the plugin's own window in a separate editing helper that follows the pick; and a
plugin's whole state is kept per cue, saved as the hand stops and loaded onto the voice before
the cue launches. Plugin editor windows therefore leave Phase 9b's list. M35 (a state's load
time on the author's own plugin) is the measurement it owes.

After 9a.0, four streams run on disjoint files — the DSP, the document object,
the hosting and the transport — and the FX entries land on top of all four.

**Done when:** a media cue's EQ is heard and drawn from one function; every EQ
and insert parameter is a `node.set`-able node with its name, range, default,
bipolar flag and value text published beside it, and the insert order is
readable (the pages draft's §8, all four items); a third-party VST3 plays on a
cue through the sandbox with its parameters ridden live; a plugin killed
mid-show leaves the cue dry (silent since 2026-09-26, decision CU), the strip marked failed in words and the block
cost bounded; M30–M34 recorded; CI green on all six jobs.

**Needs from the author:** what he sees on the desktop once 9a.3 and 9a.9 land
(plan decisions 1, 5, 7, 12 and 14 of §17.11 are the ones a look settles); the
deadline and the failed-strip budget once M31 and M32 are taken; and whether
the pages session should start on the EQ page when 9a.3 lands rather than wait
for the inserts.

### Phase 9b — The live rack: mic cues on named rack channels · L

*Redrawn on 2026-09-26 at the author's request* — *"could we add the effects rack for live
inputs?"* — with **five decisions** of the author's (**BW**, **BX**, **BY**, **CE**, **CG**,
`godot-namespace-draft-0.1.md` §18.1), all as recommended, and nine of the implementer's (CH–CP)
written down to be overruled early. A live input is a **mic cue**: it names an input and a rack
channel and runs until something stops it. The show names its inputs and declares its rack
channels, each with its own chain of plugins hosted out of process. The latency budget is five
milliseconds of plugins, always said in words. The phase is drawn before the code as §18 of that
draft. One commit and push a stage:

| Stage | What | Depends on |
|---|---|---|
| 9b.0 | Docs first: namespace §18 and §19, the PRD amendments (§3.18, §3.9e, §3.29, §6.2, new §3.31, §6.9, §6.11, §7), this section and 9c's | — |
| 9b.1 | Two persistent-section faults (a double Esc suspending, a jump cutting) and `isPlaying` on slot 0; `persistent.wfglog` | 9b.0 |
| 9b.2 | Named inputs, the input tap and its meters, the interface's delays, `--input-wav`, the Inputs list | 9b.0 |
| 9b.3 | Rack channels become tracks: their plugins, `channel.plugin`, `LiveInputPlugin`, the rack's children, Load now, the Rack tab | 9b.2 |
| 9b.4 | The owner split (media → sound), then the `Mic` element, its rows, validation, the inspector and the page | 9b.3 |
| 9b.5 | Mic cues sound: the claim and the wait, arm, launch, stop and its tail, kill, the live pushes, the latency words; `mic.wfglog`, the driver | 9b.4 |
| 9b.6 | Mic cues in the show's structure: persistent, standby, the horizon, load-to-time, DCAs, the Load now refusal | 9b.5, 9b.1 |
| 9b.7 | The hands and the window — the EQ, Send and FX pages, the chain at the foot, the running pane — M38–M40, close-out | 9b.6 |

*As built, 2026-09-26:* every stage on `main` - 9b.0 `f757cab`, 9b.1 `319bbae`, 9b.2 `f32d43c`,
9b.3 `b11af15`, 9b.4 `6dcf8a5` and `503cdd9`, 9b.5 `04f8fb1`, 9b.6 `e93a87d`, 9b.7 closing it - with
CI green on all six jobs at `503cdd9`. Namespace §18.12 says where the build departs from the
drawing. Of the list below, what the code can show is shown (`blackbox/phase9b_inputs.py`,
`phase9b_mic.py`, `logs/mic.wfglog`); what waits for the bench is hearing a mic cue through a real
plugin on the MADIface and M39's loopback against the words.

*Amended after close-out, 2026-09-26 (decisions CU and CV, the implementer's call CW; namespace
§18.13):* a plugin that cannot play a cue leaves it silent, never dry - failed, late, missing or still
loading - and a relaunched child is given back the state its voice held, the voice fading back in
where the cue has got to. The same for the set's voices and the rack's channels.

**Done when:** a named input's meter moves; a mic cue through a real plugin on a rack channel is
heard on the MADIface; the plugin's child killed mid-cue leaves it silent and saying so (dry as first
written; §18.13), the show going
on; Esc lets its tail ring out and frees the channel for a waiting cue; a double Esc is silence at
once and GO restores a persistent mic; the words match M39's loopback; CI green on all six jobs.

**Still Phase 9b's and not in these stages:** the **shared rack channel** (a reverb return cues send
into — a return track and Tracktion's aux sends) and a media cue's `Insert` made to sound; **inline
hosting** (PRD §3.18's opt-in) with §3.4's message-thread handover; AU presets; AUv3; curated
per-plugin parameter maps (the pages draft's §7.2) for the FX page, which takes a mic cue's inserts
as it takes a media cue's (namespace §17.16); macOS audio workgroups for the child. *(LV2 left this
list on 2026-09-26: built on every platform, with AU on macOS, the scan in the app and the
mono→stereo widening on the voice inserts — namespace §17.15. The width classes are the rack
channels' own, built in 9b.3.)*

**Needs from the author:** the rest of the built-in plugin list; a loopback cable on the MADIface
for M39; the D700 on the desk for 9b.7.

### Phase 9c — Live sampling channels · M

*Added on 2026-09-26* — the second half of the same request, *"live sampling channels that can take
in an input, loop with continuously variable in and out points with pre-recording and
post-looping/playback effects"*, not in the PRD until then (now §3.31). **Six decisions** of the
author's (**BZ**, **CA**, **CB**, **CC**, **CD**, **CF**, `godot-namespace-draft-0.1.md` §19.1), one
against the recommendation — layers with overdub rather than a single take — and four of the
implementer's (CQ–CT). Built on 9b: a sampling channel is a rack channel with a recorder between its
plugins.

| Stage | What | Depends on |
|---|---|---|
| 9c.1 | The looper, pure: layers, crossfaded wraps and jumps, moving points, peaks; `LooperTests` under rtsan | 9b.0 |
| 9c.2 | The recorder in the graph: `takeSeconds`, `layers`, a plugin's `side`, `LooperPlugin`, the take store outside the Edit, the memory in words | 9b.7, 9c.1 |
| 9c.3 | The verbs: `take.*`, the transport verbs, `onGo`, `through`, the loop points' door, the D700's Rec; `take.wfglog`, the driver | 9c.2 |
| 9c.4 | The take's picture at the foot: edges, playhead, layers, the buttons; the master dial on the points | 9c.3 |
| 9c.5 | The Loop page on the D700 | 9c.3 |
| 9c.6 | Keep, and Keep as cue | 9c.3 |
| 9c.7 | M41–M44 and close-out | 9c.4, 9c.5, 9c.6 |

**Done when:** a take recorded on a mic cue through a plugin before the recorder loops through a
plugin after it; a layer laid and undone; the in and out points ridden on the D700's Loop page with
no click; a transport cue records and loops it; Keep leaves a file a media cue loops; CI green.

*As built, 2026-09-27:* every stage on `main` - 9c.1 `19dcf27` (and `0d184ba`), 9c.2 `bc55df6`,
9c.3 `338d3a0`, 9c.4 `2d73400`, 9c.5 `d129ee2` (and `7c22911` for the strict build), 9c.6 `4d5ab77`,
9c.7 `2238346` and the close-out - with CI green on all six jobs at `66dac3e`, which also mends a
fault of the rack's that the close-out's CI runs found: a block Tracktion muted made the input stage
start its cue's fade-in again. Namespace §19.9 records M41-M44, and §19.11 says where the build
departs from the drawing. Of the list above, what the code can show is
shown: a take through a plugin before the recorder looping through one after it (`ProxyTests`, 9c.2);
a layer laid and undone, the transport cues' Rec and Loop, Keep's file read back and Keep as cue's
media cue (`blackbox/phase9c_take.py`, `logs/take.wfglog`); the points ridden with no step
(`LooperTests`) and turned on the Loop page (`SurfaceBridgeTests`). What waits for the bench is the
D700 itself - its Rec and light, a Loop key if it has one, the page's law - and a take recorded
through a real plugin on the MADIface.

**Needs from the author:** the D700's Rec (and a Loop key, if it has one) pressed at the bench, and
the page's law for a loop point.

### Level lanes — a volume curve on a media cue · S

*Added on 2026-09-27*, at the author's request: *"a volume automation curve that's sync'd with the
media file. This for the media cues and the samples."* PRD §3.10's lane, the first one built.
**Three decisions** of the author's (**CX**, **CY**, **CZ**, `godot-namespace-draft-0.1.md` §20.1)
and four of the implementer's (DA–DD). Not a phase: it sits here because it follows 9c in time, and
no later phase is renumbered.

| Stage | What | Depends on |
|---|---|---|
| L.0 | Docs: namespace §20, PRD §3.10, §6.9 and §6.11, this section | — |
| L.1 | The row `media/levelLane`, `doc::readLevelLane`, the write door and `validate` | L.0 |
| L.2 | The Runner: the lane's term in the level sum, read one slew ahead on the file's clock, the arm's snap | L.1 |
| L.3 | The window's model: `model/Lane`, the foot's reading, the two inspectors | L.1 |
| L.4 | The lane over the waveform: its points, the gestures, the typed numbers | L.3 |
| L.5 | `lane.wfglog`, `blackbox/lane_level.py`, M45 and close-out | L.2, L.4 |

**Done when:** a lane drawn over a media cue's waveform is heard following the file - from its start
offset, after a jump, and the same on every pass of a looping slice; a sampler clip's lane rides
under its strip's fader; one gesture is one undo step; CI green.

*As built, 2026-09-27:* every stage on `main` - L.0 `d39d589`, L.1 `d807e1f`, L.2 `e8fddd0`, L.3
`5d32619`, L.4 `f5b3d78` and L.5 with the close-out. What the code can show is shown: a lane heard
following the file through a real Tracktion graph - a ramp, a hold, a step, a looping slice hearing
the same stretch on each pass and a lane rewritten while it loops (`blackbox/lane_level.py`, both
locales); M45 within 0.014-0.022 dB away from the corners and a step inside a tick, with the checks a
starved CI runner's stalled tick moved voided by the render's own witness; one gesture one write
(`RunPaneUiTests`) and one undo step (`UndoTests`); a hand's trim and a fade beside a lane, each a
term of the same sum (`GoTests`) - the trim being what a sampler strip's fader writes, though no case
yet plays a lane through a sampler group itself, which the bench will. Namespace §20.8 says where
the build departs from the drawing.

**Needs from the author:** judging the vertical law and the gestures by eye (DD), and listening on
the MADIface.

**Recording a lane from a fader** - *added on 2026-09-28*: *"Could we use a chosen fader to record
the level curve instead of mouse clicks only?"* §3.10's automation, latch first. Four decisions of
the author's (**DF**-**DI**, `godot-namespace-draft-0.1.md` §20.9), three against the
recommendation - a fader taken by touch, the curve's start value, latch - and five of the
implementer's (DJ-DN).

| Stage | What | Depends on |
|---|---|---|
| R.0 | Docs: namespace §20.9, PRD §3.10 and §6.9, this | L.5 |
| R.1 | The pick: the `surfaces` lane rows, `lane.arm`/`take`/`free`, the taken strip's target, the ride's live door | R.0 |
| R.2 | The pass: `lane.record`/`stop`, the Runner's recorder, the splice and the thinning, Esc and double Esc | R.1 |
| R.3 | The hands: the bridge (a touch that takes, the Rec key, the lights) and the virtual panel | R.2 |
| R.4 | The window: the Rec button's four states, the fader's name, the trail | R.2 |
| R.5 | `blackbox/lane_record.py`, `logs/lane-record.wfglog`, close-out §20.10 | R.3, R.4 |

**Done when:** a fader touched while a lane waits is taken and flies to the curve's start; a pass
plays the cue with the fader following the lane; a touch is heard at once and written, held after
let-go, until the pass stops; the lane holds the ride and one undo takes the pass away; the D700's
Rec starts and stops it; CI green.

*As built, 2026-09-28:* R.0 `39600af`, R.1-R.2 `447d14d`, R.3-R.4 `7767a52` and R.5 with the
close-out (namespace §20.10). What the code can show is shown: the pick, the refusals and a pass
latched and written once (`LaneRecordTests`, `logs/lane-record.wfglog`); a touch that takes and the
Rec key's pass (`SurfaceBridgeTests`); the panel's take and the window's four states
(`RunPaneUiTests`); and the whole of it over a real graph - heard, written, undone, replayed
(`blackbox/lane_record.py`, timing judged off CI). The D700 itself is the bench's.

### Speed: varispeed and timestretch on a media cue · M

*Added on 2026-09-28*, at the author's request: *"We have unfinished work on Varispeed and
Timestretch. This is a toggle in each media file to change the behaviour of faster or slower
playback speed. The playback speed can be adjusted from 0.f to 20.f default to 1.f"* - and, the same
day, fades on the rate *"in either mode"*. Phase 3's one dropped line (PR 3.10), built live rather
than at arm.

**Three decisions** are the author's (`godot-namespace-draft-0.1.md` §22.1), one against the
recommendation:
- **DQ**: Tracktion is changed by a patch the build applies, not a fork.
- **DR**: timestretch freezes at nought.
- **DS**: the fade cue gains a speed.

Fifteen more are the implementer's (DT-EH). Not a phase: it sits here because it follows the lanes
in time, and no later phase is renumbered.

| Stage | What | Depends on |
|---|---|---|
| S.0 | Docs: namespace §22, PRD §3.24, §3.25, §6.9 and §6.11, this | - |
| S.1 | The patch mechanism (`patches/tracktion_engine/`, `cmake/WfgTracktionPatches.cmake`, `te-patches.py`, `check-pins` (g)), patch 0001, Signalsmith switched on - no change in behaviour | S.0 |
| S.2 | Patch 0002 (a launched clip's speed), `RateClock`, `RateVoice`, the slot adaptors, heard through a real graph | S.1 |
| S.3 | The rows, the Runner's clock (playhead, lane, ranges), the wiring, the inspector's two rows | S.2 |
| S.4 | Speed fades: the fade's switches, per-parameter takeover, Esc, the solver's `levelOn` | S.3 |
| S.5 | The window: the running pane's `×`, the head row, the dial's semitone law, the page | S.3 |
| S.6 | The list, the walk, the timeline and load-to-time at the cue's own speed | S.3 |
| S.7 | `blackbox/rate_speed.py`, `logs/rate.wfglog`, M14, M46, M47 and the close-out | S.4-S.6 |

**Done when:** a media cue plays at any speed from nought to twenty in either mode, typed, dialled or
faded while it sounds. Varispeed's pitch moves and timestretch's holds; nought is silence in one mode
and a freeze in the other. At one, every render is bit-identical to the day before. The playhead, a
level lane and a looping range follow the file at its speed, and one fade moves the level, the speed
or both. CI is green on fresh checkouts with the patch applied by the build.

**Needs from the author:** listening at S.2 (tape stop, freeze, 2× both ways), before any row exists;
the D700 dial's semitone law; a Mac mini run.

**Built 2026-09-29, S.0-S.7** (namespace draft §22.10). The measurements found two faults in
Tracktion's stretcher, mended in patch 0002, and left three things named and proposed rather than
built: a stretched launch's prime costs 1 to 5 ms on the audio thread (a dropout at blocks of 64,
so timestretch wants 256 or more until it primes on the message thread); a stretched cue with ranges
had a short gap and a click at every pass - since mended, the loop moved below the stretcher
(namespace draft §22.12); and above one Lagrange aliased what the speed lifts past Nyquist at full
level - since filtered, the author's way (namespace draft §22.11).

### Fades on what a cue owns, and a fade's mixer · S

*Added on 2026-10-03*, at the author's direction: *"Fades can act on DCA, these fades will make the
DCA fader move. Otherwise fades will act on several possible parameters: global cue level, send
levels, speed, eventually EQ and effect parameters. What is already represented as a slider should be
a slider in the foot panel when editing a fade. EQ and inserted effects should open the EQ interface
or inserted effect UI."* The design is in namespace draft §26. Its decisions OY-PJ are the
implementer's, and the author may overrule any of them.

- **Engine.** Three lists on the fade (`sends`, `eq`, `fx`). There is a move job per entry, keyed
  per run and per entry. The run holds the moved values, which `resolveRouting`, `eqOf` and `fxOf`
  read over the document. Each entry has a door, `/godot/cue/<fade>/moves/...`. Esc leaves moved
  values where they are, and Doh! puts them back. A hand on a DCA's fader takes over the DCA's fade.
- **Desktop.** Picking a fade opens a mixer in the foot: the level or DCA strip, the speed and the
  sends, each with a tick box. Its doors open the EQ panel on the fade, with a tick box per number,
  and the plugin's own window on the fade. The curve is a door from there.
- **Console.** The three lists appear as text rows.

**Needs from the author:** a look at the mixer, and a listen to an EQ sweep, whose coefficients
change at the tick rate.

**Built 2026-10-03** (namespace draft §26). Load-to-time does not yet place moved values (PJ).

### Send lanes · S

*Added on 2026-10-05*, at the author's direction: send lanes are built first, as the first part
of importing the author's Ableton Live sets, whose sends move all the time (namespace draft §28).
Decisions PX-QB are the implementer's, and the author may overrule any of them. Not a phase: it
sits here because it follows the fades in time, and no later phase is renumbered.

| Stage | What | Depends on |
|---|---|---|
| SL.0 | Docs: namespace §28, PRD §3.10, this | - |
| SL.1 | The row `send,levelLane` in the table, with the regenerated schema and grammar; the write door and `validate` judge it with `readLevelLane`, and refuse it on a mic cue's send (QA); one write is one undo step | SL.0 |
| SL.2 | The engine: `applyLanes` reads the send lanes one coefficient slew ahead (PZ), `Run::sendLaneDb`, `sendLaneRevision` gating `applyRouting`, and the offset in `resolveRouting`'s `Send` branch and at the arm | SL.1 |
| SL.3 | The window: the lane picker in the waveform editor (QB), with the level lane's gestures; the page lists the row on each send | SL.2 |
| SL.4 | `SendLaneTests` and the `GoTests` cases, in both locales | SL.2 |
| SL.5 | `blackbox/lane_send.py`, `logs/send-lane.wfglog`, M48 and the close-out | SL.3, SL.4 |

**Done when:** a media cue's send follows a drawn curve over the file, round a looping range and
through a jump, beside a fade's moved value and the lock's ride. Every render is bit-identical to
the day before for a show with no send lane. CI is green.

**Needs from the author:** a listen to a sound travelling between two mixes, and a look at the
picker.

**Built 2026-10-05, SL.0-SL.5** (namespace draft §28.6). M48: a send follows its lane within 0.06 dB
away from its corners, and a step drawn on a send is a 50 ms ramp whose middle lands 26 to 31 ms
early - the coefficient's glide, where a level's is one tick.

### Importing an Ableton Live set · L

*Added on 2026-10-05*, at the author's direction: import the Lazzi tour's Live sets, one scene per
GO, the silent "track in" clips as the fades they are (namespace draft §29). The author's decisions
are QC-QF; QG-QV are the implementer's, the author's to overrule. Not a phase: it follows the send
lanes, which it needs, and no later phase is renumbered.

| Stage | What | Depends on |
|---|---|---|
| AL.0 | Docs: namespace §29, PRD §3.20, this, a note in the QLab draft | - |
| AL.1 | The probe set (the author, in Live 12): one scene per law the walk relies on, exported (QU) | - |
| AL.2 | `import/AlsReader`: the set read into plain facts, Live 10 to 12; `AlsReaderTests` on the probes | AL.1 |
| AL.3 | `import/AlsWalk`: the scenes replayed and each sound flattened (QH-QJ); `AlsWalkTests` | AL.2 |
| AL.4 | `import/AlsTranslate` and `ImportReport`: cues, buses, EQ, DCAs, media, ids (QK-QR, QV); `wfg import-als`; the imported probe validated by both grammars | AL.3, SL.1 |
| AL.5 | Venues: a show with a performance per set, notes from the template (QF, QT) | AL.4 |
| AL.6 | The window: the menu, the scene list (QS), the import off the message thread, the show and its report opened | AL.4 |
| AL.7 | `blackbox/import_als.py`: the probe imported, rendered and compared with Live's export (M49); the close-out | AL.5, AL.6, SL.5 |

**Done when:** the Lazzi sets import as one show with a performance per venue whose cues are the
conduite's Q1-Q14 in its words; each cue sounds as Live played it, the parked effects aside; and
the report names every approximation, the hands table among them.

**Needs from the author:** the probe set in Live (AL.1); a look at the scene list and the report; a
listen, cue by cue, against Live playing the same set.

**Built 2026-10-05, AL.0 and AL.2-AL.6** (namespace draft §29.5): the reader, the walk, the show
written, the tour, `wfg import-als` and File > Import Ableton Live set.... The Lazzi tour imports as one
show with eleven performances of fourteen GOs. A fresh document's `<Audio>` order was found wrong and
mended on the way. A hand and a drawing on one fader: the drawing is imported (the author, QW).
Waiting: the probe set (AL.1) and AL.7's render against Live's export.

### Level and sends recorded from flipped faders · S

*Added on 2026-10-06*, at the author's direction: the faders flip to one media cue's level and
sends, each strip's REC arms its lane, and one pass records them all (namespace draft §34). The
author's decisions are UI-UM; UN-UT are the implementer's, the author's to overrule. Not a phase:
it follows the send lanes, and no later phase is renumbered.

| Stage | What | Depends on |
|---|---|---|
| F.0 | Docs: namespace §34, PRD §3.10, this; the rows `surfaces,laneRec`, `bus,laneRide` and the strip's `rec`, `laneFader` removed, the schema regenerated | - |
| F.1 | The pick: `LaneTable` of one flipped cue and its armed lanes, `lane.arm` as the flip, `lane.rec`, `lane.take` retired; the tree's strips by UN; the live door for every ride | F.0 |
| F.2 | The pass: the Runner's hook for every lane - the level's term, a send's offset, a send of the run alone - and the end written in one step | F.1 |
| F.3 | The bridge: a strip's REC arms its lane, its light, the strip's screen, the transport Rec | F.1 |
| F.4 | The window: the automation button, the trail of the picked lane, the virtual panel's REC | F.2, F.3 |
| F.5 | `blackbox/lane_record.py` and its log recorded again, and the close-out | F.4 |

**Done when:** a cue's level and one of its sends are ridden in one pass and both written, one
undo takes both back, a mix with no send becomes one, and CI is green.

**Needs from the author:** the flip on the D700, its REC lights and screens, and the window's words.

**Built 2026-10-06, F.0-F.5** (namespace draft §34.7). The lanes a pass rode are one `lane.write`,
the send a mix lacked made in the same step; the driver hears a level and a send ridden in one pass,
the send under the level's ride.

### OSC cues with several messages, bundles and recorded curves · L

*Added on 2026-10-08*, at the author's direction: an OSC cue sends several messages of several values,
a device may take them in bundles, and every number may follow a curve on the cue's own time, recorded
from what the device reports (heard, or asked with OSCQuery's LISTEN) or from a SpaceMouse (namespace
draft §45). The author's decisions are YP-YV; YW-ZM are the implementer's, the author's to overrule.
Not a phase: it follows the flipped faders, and no later phase is renumbered. It brings forward two
things listed later - the SpaceMouse as a rate endpoint (Phase 11) and a device's Rx processed
(namespace draft §15.1, D4) - for the one use the author named.

| Stage | What | Depends on |
|---|---|---|
| O.0 | Docs: namespace §45, PRD §3.10, §3.11, §3.12, §3.16, §3.24, §6.9, §6.11, this | - |
| O.1 | Several values in a message: the value as a list, the device's door, the sender, the read-back, the solver | O.0 |
| O.2 | Several messages in a cue: `<Message>`, one device per cue, waits across messages, never prepared ahead | O.1 |
| O.3 | Bundles per device: `mount/bundles`, the sender's datagrams of 1200 bytes, the Network tab's column | O.1 |
| O.4 | Curves played: `<Curve>`, the cue's clock, duration and loop, sent when changed; the mounted values apart from the shape, M9 taken again | O.2 |
| O.5 | The window: a cue's messages and their values, the target menu moving them all | O.2, O.3 |
| O.6 | The level lane's drawing taken out of the waveform editor, nothing changed on screen | - |
| O.7 | The window: the curve editor in the foot panel, one curve at a time | O.4-O.6 |
| O.8 | Heard values: `mount.heard`, a device's reports never echoed | O.1 |
| O.9 | A pass recorded from heard values: `curve.*`, latch, one step of undo | O.4, O.8 |
| O.10 | LISTEN: a WebSocket client per device, while a curve on it is armed | O.9 |
| O.11 | The SpaceMouse: hidapi pinned, the engine's reader, `curve.ride`, the 3DxWare question | O.9, O.7 |
| O.12 | The drivers, the logs, M51-M53, the close-out | all |

**Done when:** a cue moving a source's x, y and z leaves in one bundle a tick, a curve is recorded from
a source dragged on a device that reports it and another from the SpaceMouse, one undo takes a pass
back, an old show saves unchanged, and CI is green.

**Needs from the author:** WFS-DIY live - bundles, LISTEN, no echo, one machine and two; the
SpaceMouse Compact on each system, with and without 3DxWare; the window's words.

**Built 2026-10-08, O.0-O.12** (namespace draft §45.9): O.6 folded into O.7. The driver
(`blackbox/osc_curves.py`) plays a cue of three curves into a device as bundles and records a pass from
what the device pushes over LISTEN, and the session replays; M51 and M52 are measured, M53's time and
everything above under "Needs from the author" are the bench's. The SpaceMouse is built against a puck
nobody here has pushed.

### Importing a QLab workspace · L

*Added on 2026-10-08*, at the author's direction: import a QLab 4 or QLab 5 workspace from its file, the
way the Ableton Live set is imported (namespace draft §46), now that §45 gives an OSC cue every message a
QLab show sends. The author's decisions are ZN-ZR; ZS-ZZ are the implementer's, the author's to overrule.
Not a phase: it follows the OSC cue's messages and curves, which it needs, and no later phase is
renumbered.

| Stage | What | Depends on |
|---|---|---|
| QL.0 | Docs: namespace §46, PRD §3.20, this, notes in the QLab draft and the extraction | O.4 |
| QL.1 | The probe set (the author, in QLab 5 and QLab 4): small workspaces holding every kind and mode the walk relies on (ZQ) | - |
| QL.2 | `import/ImportCommon`: identifiers, the media search and copy, the folder written and the report's shell, taken out of `AlsImport`; the Live import's tests and `import_als.py` unchanged, byte for byte | QL.0 |
| QL.3 | `import/Bplist`: binary property lists and keyed archives, nested ones included, every offset checked; `BplistTests` on lists made byte by byte | QL.2 |
| QL.4 | `import/QlabReader`: a workspace read into plain facts, QLab 4 and 5 (§46.5), any other version refused in words; `QlabReaderTests` on the probes | QL.1, QL.3 |
| QL.5 | `import/QlabWalk`: groups and chains (ZS), audio and routing (ZT, ZU), fades and trims (ZV), devices, messages and curves (ZW), the other kinds (ZX); `QlabWalkTests` | QL.4 |
| QL.6 | `import/QlabImport`: the show written through the checked writes, the report (ZY, ZZ); `wfg import-qlab`; `blackbox/import_qlab.py` in both locales, the probes validated by both grammars and imported twice to the same bytes | QL.5 |
| QL.7 | The window: File > Import QLab workspace..., the lists to tick, the import off the message thread, the show and its report opened | QL.6 |
| QL.8 | The author's two shows imported from the corpus (`WFG_QLAB_CORPUS`), checked against what QLab reports over OSC; the close-out | QL.7 |

**Done when:** the author's QLab 5 show imports with its 126 groups, its 348 network cues as OSC cues -
those of several values as lists, the three fades as curves - and memos only for the kinds §46.4 names;
the QLab 4 show's auto-follow chains come back as groups that fire on one GO; both shows pass both
grammars; and the report names every approximation.

**Needs from the author:** the probe workspaces (QL.1), a QLab 4 one included if QLab 4 still installs;
a reading of ZS-ZZ; a look at the list ticks and the report; a run through an imported show's first
scenes against the rig.

**Built 2026-10-08, QL.0 and QL.2-QL.8** (namespace draft §46.7): the shared parts, the decoder, the
reader, the walk, `wfg import-qlab` and File > Import QLab workspace.... Every save of the author's two
shows on the drive - 11 in QLab 5, 83 in QLab 4 - builds into a show that validates. A cart is a list, a
mixed chain an automatic sequence, and MIDI, video and the triggers wait for the probe workspaces; the
command line reads its arguments as UTF-8 on macOS and Linux, found on an accented show folder.
Waiting: the probe set (QL.1), the shapes measured, and a run against the rig.

### Process cues: a Pure Data patch in a cue · L

*Added on 2026-10-09*, at the author's direction: a cue whose patch - Pure Data's, run by libpd inside
Go.dot - takes in what devices, MIDI, a serial port and the show say, works on it, and sends it on or
makes Go.dot fire, enable, disable or jump; edited on Go.dot's own canvas at the foot of the window,
with Pd's own window as the fallback (namespace draft §51; PRD §3.20, §3.21). The author's decisions
are ACC-ACF; ACG-ACU are the implementer's, the author's to overrule. Not a phase: it brings Phase 11's
"OSC and MIDI processing cues" forward, and serial with it, and no later phase is renumbered.

| Stage | What | Depends on |
|---|---|---|
| PC.0 | Docs: namespace §51, PRD §3.6, §3.8, §3.20, §3.21, §3.29, §6.5, §6.9, §6.11, this | - |
| PC.1 | libpd and Pure Data pinned and built on the three systems; the Windows threads stand-in; the binary loader refused; a patch run headless; Pd's text read and written back | PC.0 |
| PC.2 | The kind: rows, schema, the engine's host and threads, heard in, devices and commands out, Esc, Doh!, the persistent section, the budget, late and stuck, the quiet point | PC.1 |
| PC.3 | MIDI in and out, `[print]`, the ports' values, the SpaceMouse, `process.send` | PC.2 |
| PC.4 | The window: the + menu, the icon, the patch's text and MIDI ports in the inspector, late and stuck on the row, the Playback tab | PC.2 |
| PC.5 | The canvas, round one: drawn, panned, zoomed, moved, deleted | PC.4 |
| PC.6 | The canvas, round two: typed, placed, joined, copied | PC.5 |
| PC.7 | Pd's window, a save in it on the canvas, `pd.install` | PC.2, PC.5 |
| PC.8 | The canvas, round three: the values live, toggles and sliders | PC.3, PC.6 |
| PC.9 | The ready-made patches, their help, an example show | PC.2 |
| PC.10 | Serial ports: lines in and out | PC.2 |
| PC.11 | OSC over SLIP: a device on a serial port | PC.10 |
| PC.12 | The driver, the log, M56-M59, the close-out | all |

**Done when:** a patch averaging what a device reports fires a cue when it crosses a line, made on the
canvas from nothing; an Arduino's lines reach a patch; a stuck patch leaves the show running and says
so; an old show saves unchanged; CI is green.

**Needs from the author:** the canvas's words and look; a patch of their own made on it and in Pd's
window; an Arduino on the bench, lines and SLIP.

**Built** 2026-10-09, PC.0 to PC.12, each on main (namespace draft §51.9 says what each did; M56-M59
in §51.6). Two of the implementer's calls changed while building: Pd's window became plugdata or Pd as a
program of its own (ACN, for three ways Pd's window on the running patch could end the show), and the
serial names a patch uses became `/godot/serial/<id>/in` and `/out` (ACR). Found on the way and fixed:
a show's rx devices were heard only after its first edit (PC.11); `[expr]` printed to stdout (PC.9);
an identifier Pd would read as a number (PC.9). Owed to the bench: everything under "Needs from the
author" above, on screen and with an Arduino.

---

### Editing a sound: sections, crossfades, trim, and a freeze · M

*Added on 2026-10-10*, at the author's direction: a sound cue cut into sections at the playhead, the
sections reordered or removed, a crossfade at each join and a trim on each, frozen to a bounce the cue
plays and unfrozen for more changes, the lanes and ranges carried with the sound (namespace draft §55;
PRD §1 non-goals, §3.24). The author's decisions are ADI and ADJ; ADK-ADR are the implementer's, the
author's to overrule. Sounds only; movies are a later round.

| Stage | What | Depends on |
|---|---|---|
| E.0 | Docs: namespace §55, the PRD's non-goal and §3.24, both guides, this | - |
| E.1 | The Section element: rows, schema, publication, grammar, validate | E.0 |
| E.2 | The math: the timeline, the map between two section lists, the carry of lanes, ranges and offset, the clamp | E.1 |
| E.3 | The commands and the carry at the doors; freeze and unfreeze as document edits | E.2 |
| E.4 | One resolver of the played file; the Runner follows it on standby; lengths from the sections | E.3 |
| E.5 | The renderer: the render, its thread and cache, `media.freeze`, the readout | E.4 |
| E.6 | The window's model: the sections, the foot reading, the gestures | E.3 |
| E.7 | The window: the sections row, the bar on the edited timeline, Freeze and Unfreeze | E.5, E.6 |
| E.8 | The driver, the replay, the close-out | all |

**Done when:** a music cue split into verse and chorus with the chorus moved first plays without a click
at the join, the lane's dip stays over the verse, a looping Range over the chorus still loops the chorus,
Freeze then Unfreeze sound the same, an old show saves unchanged; CI is green.

**Needs from the author:** the row's look and words; the joins by ear at 10 ms and at 200 ms; a trim
ramp heard; Save as with a frozen cue.

**Built** 2026-10-10, E.0 to E.8, each on main (namespace draft §55.4 says what each did). The
implementer's calls ADK-ADR stand as proposed; what the author has not yet seen: the two rows on
screen, a join by ear, a bounce in a show folder.

---

### Editing a movie: sections, dissolves, the sound in step · M

*Added on 2026-10-10*, at the author's direction, the sound's row an hour old: a HAP movie cue cut into
sections at the playhead on its frame grid, the sections reordered or removed, a linear dissolve at
each join, its locked sound cut in step, rendered to a HAP movie while open and frozen with its sound
as one pair; a movie that is not HAP converted first (namespace draft §55.5-55.8; PRD §3.19, §3.24).
The author's decisions are ADS and the three picks under ADT, ADU and ADW; the rest are the
implementer's, the author's to overrule.

| Stage | What | Depends on |
|---|---|---|
| V.0 | Docs: namespace §55.5-55.8, the PRD's two sentences, both guides, this | - |
| V.1 | The document: a Video holds Sections, its sound follows, the pair freezes | V.0 |
| V.2 | The file's facts published (frame rate, codec), the grid at the verbs, the HAP refusal | V.1 |
| V.3 | The resolver and the Runner: what a movie plays, its read-ahead, the monitor's second | V.2 |
| V.4 | The render: `renderMovieEdit`, frames copied, dissolves blended | V.2 |
| V.5 | The renderer service by kind, the pair's freeze, serve | V.3, V.4 |
| V.6 | The window's model: the movie's facts, the mapping of the strip and the cuts | V.2 |
| V.7 | The window: the row over the strip | V.5, V.6 |
| V.8 | The driver, the replay, the close-out | all |

**Done when:** a three-shot HAP movie with its sound is reordered with a dissolve and plays so, the
sound in step with its lane dip where the shot went, Freeze then Unfreeze show the same, a movie that is
not HAP says to convert it first, an old show saves unchanged; CI is green.

**Needs from the author:** a dissolve by eye at four frames and at twenty-five, on a projector; a Hap Q
movie's join; a frozen pair in a show folder; the row's words for a preview movie.

**Built** 2026-10-10, V.0 to V.8, each on main (namespace draft §55.8 says what each did). The
implementer's calls ADT-ADX stand as proposed, the author's three picks inside them; what the author
has not yet seen: the row over a strip on screen, a dissolve on a projector, a frozen pair in a show
folder.

---

### Handles on the waveform: fades, gaps, selection, keys · M

*Added on 2026-10-10*, at the author's direction, the movie's row an hour old: Samplitude's gestures on
the sections - a volume handle in the middle, an edge handle and a fade-length handle at each end, Shift
to unlock a join's fades, the wheel to bend a fade's curve, the top half for a time selection and the
lower half for a section, x to split, Backspace to delete leaving silence and Shift+Backspace with ripple
(namespace draft §55.9-55.12; PRD §3.24). The author's decision is ADY; ADZ-AEH are the implementer's,
the author's to overrule - ADZ reads a slip in their message.

| Stage | What | Depends on |
|---|---|---|
| G.0 | Docs: namespace §55.9-55.12, the PRD's sentence, both guides, this | - |
| G.1 | The model: two fades, their curves and a gap on every section, both renders, the legacy read | G.0 |
| G.2 | The verbs: edge, fade, curve, remove leaving silence, split and delete a span | G.1 |
| G.3 | The window: the handles on the bar, the two halves, the keys, the wheel | G.2 |
| G.4 | The drivers, the close-out | all |

**Done when:** a sound's join is lengthened with one side alone and its curve bent, a section deleted
leaving silence and another with ripple, a selection split and deleted, each by the handles and the keys
and each heard so in the render; a movie's gap is black; CI is green.

**Needs from the author:** the handles' feel by hand; a bent crossfade by ear; whether ADZ read the slip
right.

**Built** 2026-10-10, G.0 to G.4, on main (namespace draft §55.12 says what each did). The implementer's
calls ADZ-AEH stand as proposed; what the author has not yet seen: the handles on screen, a bent
crossfade by ear, the keys.

---

## Phase 10 — Timecode, panic, hardening · M

**Goal:** the stop levels and the sync sources that a touring show requires.

- **LTC/MTC chase and generate**; tick re-anchoring when chasing (PRD §3.14 —
  settle the derivation before writing the transport).
- **Esc / double Esc / Doh!** per PRD §4.4: graceful abort runs footers;
  immediate skips them and kills internal processing only; Doh! recovers an
  early trigger within the anticipation window. Revert-of-GO (§4.5).
- Panic values on every node honoured.
- Debounce as a user preference.
- **Esc on a persistent media cue as a pause** (PRD §3.29): *decided by the
  author 2026-10-02 and built as K8 (namespace draft §23.17)*; §4.4 gained its
  sentence and `CLAUDE.md` was re-copied, never edited. A kill from the running
  pane suspends a persistent assertion; a double Esc does not, and the next GO
  restores the declared world - from the top, a double Esc forgetting a pause.

**Done when:** a show chases timecode from an external source without drift or
discontinuity, and each stop level does exactly its guarantee and nothing more.

**Needs from the author:** the Doh! inventory of in-flight objects (PRD
§4.4, deferred) - *met: the author's answers of 2026-09-30 and 2026-10-01 are in
PRD §3.32, the inventory in namespace draft §24.2*; the Esc-as-pause decision
(§3.29) - *met 2026-10-02: a pause, built as K8*.

**Built** (the hardening plan, stage by stage in namespace draft §23):

- **H0, 2026-09-30 — the real-time safety job can go red** (§23.1). It could
  not: its reports went to a log nobody was shown. RTSan in Clang 20 ignores
  `log_path`, so the gate reads ctest's `LastTest.log`, the harness reads a
  driver's `wfg` stderr, and `rtsan.control` proves the gate hears a planted
  report. First real measurement: no report anywhere in the suite.
- **H1 and H2, 2026-09-30 — the two stop levels held to §4.4** (§23.2-23.3).
  Esc brings a scene down the way it would have ended: members stopped, not
  killed, so nested footers run innermost-first and a mic's tail rings; a
  double Esc cuts a footer already running and is read from above, and follows
  a stop that has already landed. A scene that was only made ready is given
  back, never footered, and Esc and double Esc leave the standby's preparation
  standing.
- **H6 and H6b, 2026-09-30 — crash-safe saving, killed halfway through a
  write.** `blackbox.crash-write.{C,fr_FR}` kills the engine at twenty seeded
  points while saves and autosaves are in flight (§23.4). It found a save
  leaving the older autosave on offer, on every platform (about one kill in
  five), and on Windows `ReplaceFile` leaving a file under no name (about one
  in twenty-five, show.xml included). Both are closed (§23.5): a save marks
  what it will retire `superseded` before it writes - and every save after it
  carries a folder it could not delete - and a file takes its name in one
  rename, on Windows by handle with POSIX semantics, so that a program reading
  the file stops no save. The driver is registered with the fixes it failed
  first on.
- **H3, 2026-10-01 — a double Esc silences Go.dot's own effects** (§23.6). A
  voice's kill stops it, silences its output, clears its EQ and resets its
  inserts while still heard; the press sweeps every idle voice to silence and
  every idle EQ, leaving what the press keeps ready; no level reaches a cut run;
  a kill goes through a stop cue's fade hold at once. Inserts on voices are not
  reset in a burst (one plugin process resets lanes in series; GE); AU and LV2
  inserts are silenced by the level.
- **J3, 2026-10-01 — a MIDI port switched off sends nothing** (§23.7). Its row
  had always said so and the engine never read it: a cue on a bound port with
  `Tx` off went out on the cable. It now runs, sends nothing and ends
  `not-sent`, as a network device's `Tx` has made it since 2026-09-22.
- **J1, 2026-10-01 — a jump lands in the round it is in** (§23.8; the author's
  "fix it, own commit"). A jump seated its scene before its first round, so the
  scene played its round again once it ended, and the GO on a manual group's
  last member sent the pointer back to its first. A seated scene is in round
  one now, a scene a seek re-seats keeps its own round, and a jumped shuffle
  draws a seed of its own. And a seek on a scene the engine cannot time - one
  that loops, a timeline with a header, a manual group - leaves it as it is,
  where keeping its round had a scrub end it with its footer (the review's
  finding; before J1 the scrub started its round over). A log with a jump or a
  seek into a manual group and a GO on its last member replays differently, as
  does one with a seek on a scene the engine cannot time; no fixture holds
  either.
- **J2, 2026-10-01 — a GO on a scene's row inside a running act plays the
  scene made ready there** (§23.9; the author's "fix it, own commit"). The GO
  started a second copy beside it - sounds armed again with the hand down, the
  header sent to the desk twice - while the prepared one held its voices and its
  pre-sends for as long as the act ran, and after. The GO now adopts the block,
  stamped with its serial, and the act launches it as it launches every member;
  the pointer walking away from a scene made ready under a running act gives it
  back, its desk values put back first; an adopted block is never marked
  prepared again by its own job; and a sampler bank its act launches from the
  hold takes over as it arms, as on every other road. A log holding such a GO
  replays differently; no fixture holds one. The review of the first build
  added three: the GO adopts its own scene's block also when the same GO
  enters the scene's parent, which the first give-back had revoked before the
  parent reached it (IC); a sound passed over in a running act gives its voice
  back (ID); and a block's preparation no longer launches the scene nested in
  it, which had played with no GO behind a header the horizon could take ahead
  (IE).
- **Doh! D0 and D1, 2026-10-01 — taking back the last GO** (PRD §3.32,
  namespace draft §24). D0 is the specification: PRD §3.32, §4.4's pointer to
  it, and §24's rule, inventory, decisions and limitations. D1 builds the
  command (`go.doh`), the show's window (Show settings > Playback, ten seconds),
  the Doh! button left of PANIC and F9, its refusals in words, and the Doh!
  setting - every network device and MIDI port left to its operator unless set
  to take back, a cue overriding either way - with the MIDI port's "plays
  sound". A Doh puts the pointer, the list's end, the GO debounce and the
  history back, brings what the GO started down (heard: over the panic fade,
  never killed; not heard: at once), gives back what the horizon made after
  it, brings back to life an act the GO ended, and never has a device left to
  its operator sent the same cue twice. Until D2 the corrected GO starts the
  cue from the top; D2 to D5 pause and carry on, put back, report, and drive it
  end to end *(2026-10-02: D2 built, below)*. *(2026-10-02, K7, at the author's direction: named **Doh!** -
  the command stays `go.doh`, and PRD §4.4 and `CLAUDE.md` say it so; a row's
  width of air between the button and PANIC; the button in its own colour while
  a GO can be taken back, fading as the window runs out, disabled once it is
  over; the setting's words "Meh" and "Undo(h)"; three of the author's open
  questions answered - namespace draft §24.8, §24.11.)*
- **Doh! D2, 2026-10-02 — what was heard carries on, what nobody heard is handed
  back exactly** (namespace draft §24.12). A GO a Doh takes back that had been
  heard is paused: the next GO on that cue carries it on from where it was at
  the press - its own playhead, as K8's paused bed (MB) - arriving over a tenth
  of a second: in place inside the Doh fade, through the arm the standby makes
  at the point once the old voice has gone, seated there otherwise; a member of
  a running act in that act; a scene re-seated where it was, firing again what
  it had fired except what reached a device left to its operator, its own fades
  carried on. What nobody heard and the GO had adopted - an arm, a prepared
  block, a scene made ready inside a running act - is handed back as it was, its
  pre-sends still on the desk, and the corrected GO is the rehearsed one. A
  second press forgets the resume; the standby says "resumes at 0:08", and
  `/godot/list/<id>/resume` publishes it. New: `go.dohPlayhead`, `list,resume`.
  A log with a Doh made on D1 is not promised to replay on D2 (HD).
- **Doh! D3, 2026-10-03 — what the GO changed elsewhere, put back** (namespace
  draft §24.13). A desk value the GO wrote goes back to what the desk held
  before it - on a device that takes back, unless another writer has touched it
  since, decided by a hook on the next tick against the desk's echo; on a device
  left to its operator nothing goes back and the report names it. A level, a
  speed or a DCA trim a GO fade moved comes back over the panic fade, or the
  fade moving it before the GO comes back with its stop on time; a stop that has
  not landed is called off, a mic's gate opened again; a cue the GO stopped is
  made again where it would be now, fading in - the rest of a pre-wait, a timed
  scene at its second, at once or once its footer has ended (`go.dohRelaunch`),
  never sending a device left to its operator anything late; an advance, a bank
  it closed, a take press put back. One report, `/godot/list/dohReport`, says
  what was put back and what was left (`list.dohReport`). A jump's values now
  leave from the same hook, so a replay writes each once; `serve`'s device write
  honours `tx`. New: `go.dohRelaunch`, `list.dohReport`, `lists,dohReport`. A log
  with a Doh made on D2 is not promised to replay on D3 (HD). D4 (the client's
  notice, the MIDI a takeBack port was sent) and D5 (end to end) remain
  *(2026-10-03: D4 built, below)*.
  *(2026-10-03, D3's review, §24.13: a jump's values land before a member its
  seat fires at once again - D3 had let the member be overwritten, in every
  show; a scene's fade-out is called off; a relaunch never plays beside a copy
  running again, nor under a sequence that moved on; a bank fired again skips
  its header; what was left survives a GO cut short by Esc. NY-OH.)*
- **Doh! D4, 2026-10-03 — what could not be taken back is named, and the
  operator is told** (namespace draft §24.14). The report now names the MIDI a
  GO sent to a port that takes back - "<cue>: MIDI to <port> could not be taken
  back - the next GO sends it again" - decided from the Doh's own walk of what
  left (NU). The desktop puts the report on the transport line the moment it
  arrives, once, whatever list has the focus ("Doh! on Act 2: ..."), what was
  left to an operator first, the whole sentence on hover; a refusal newer than
  it takes the line back; and a press in an audio outage says "Doh!: the pointer
  is back; what it puts back comes when the audio returns" (L18). Nothing new is
  logged: a log with a Doh made on D3 replays on D4. D5 (end to end) remains
  *(2026-10-03: D5 built, below - Doh! is complete)*.
  *(2026-10-03, D4's review, OJ-OQ: the outage sentence is the engine's, on
  the readout, for a press it accepted; a relaunch's report is appended
  ("...; then: ...", the orchestrator's ruling); the next GO retires the
  report; what was left comes first in the engine's sentence; the MIDI item
  says when and where it goes again; and the notice gives way to any newer
  sentence of its line and follows a focus change.)*
- **Doh! D5, 2026-10-03 — end to end; Doh! is complete** (namespace draft
  §24.15). Nothing new in the engine: `blackbox.doh` drives the shipped binary
  over a hosted render and two mock devices - a console that takes back, a
  lighting desk at the default - through a ramp paused and carried on from the
  press's second over the de-click, a scene paused part-way and re-seated (the
  console put back and sent again, the lighting desk sent each cue once in all
  and nothing at the Doh, the report naming it), a bed the GO stopped made
  again fading in, a second press forgetting and a press past the window
  refused, and replays the session; `wfg.replay.doh` keeps one such session as
  a fixture. The tests D1-D4 named owed are written (test 47's bed and MIDI,
  test 50's deleted device, the leave-out replayed, test 27's forget, L43's
  mic member and take, the inspector's menu); test 49's decision-N case cannot
  be built (§24.15). Found: on the Windows laptop a Debug build's tick thread
  falls seconds behind its audio, so the driver judges what the tick places
  where the tick keeps time (OR). **Owed to the bench**: items 20-33 of
  `docs/handoffs/2026-09-06-audio-hardware-checklist.md` - the button, F9, the
  inspector and the notice on screen; the pause, resume and de-click heard on a
  bed, a scene and a mic; a quantising fader; a lighting desk at its default; a
  long footer; F9 with the interface unplugged; the D700 binding; and the drive
  run where the tick keeps time. **Still the author's**: whether a second
  Doh! keeps what was left (L37), and his first look at the words.
- **H4, 2026-10-02 — a double Esc drops what is still waiting to leave**
  (§23.10). A value a rate cap held back went out on its turn after the press,
  the MIDI queue sent everything it held, and no note-off existed anywhere. The
  press's own handler now empties the network sender - keeping the standby's
  pre-sends, which the press leaves ready - drops the cues' MIDI messages (a
  surface's never) and sends one note-off per port, channel and key for each
  note a cue started and nothing ended (the author's rule); nothing a kill has
  reached launches, fires or writes after it, nor a start cue's fire or a
  persistent pass decided before it - the next GO restores the section; and the
  osc runs it kills in its own drain are stamped `sendDropped`, so Doh! does
  not count them as sent.
  Found and named, not closed: a pre-send its scene walked past is out of the
  press's reach.
- **H5, 2026-10-02 — after a double Esc nothing keeps writing; the panic
  column checked** (§23.11). One test runs at once the writers a unit rig can
  run cheaply - a voice's level (a lane and a fade) and speed, a DCA's trim, a
  desk's values and a value its rate cap holds back, launches and arms (a short
  media cue in a looping stream) - requires each to have moved just before the
  press, and finds nothing written after it: every run over within four
  ticks, at most one level a voice, and nothing in the published tree changed
  but the clock for fifty ticks. Routing, EQ, inserts, effects sweeps and
  stops are watched, not exercised; MIDI is H4's tests'. It passed first, H4
  having closed what it was written to catch. The one move kept: a fader taken
  for lane recording stays taken and shows the lane's start once (the author's
  to rule on). The generator now refuses a `panic` that is not `park`, `snap`
  or one value of the row's own type and width - policies only for blob, list
  and event rows - and holds every `default` to the same rule, with its own
  cases run by every `--check`; the table passes untouched. A device's `PANIC`
  array is read as a state node's safe value and published back as one; one
  the node could never hold is ignored with a warning and the device loads
  *(2026-10-02, K1: H5 refused its namespace; the author overruled it, "stay
  flexible" - namespace draft §23.12)*, and on a container or an event
  anything but a policy is ignored. PRD §3.3's "snap-to" against the schema's
  `snap` was left for the author *(2026-10-02, K1: ruled - the PRD says `snap`
  now)*. Nothing applies a panic value yet.

---

## Phase 11 — Integrations · M

- **Choufleur** (PRD §3.23): exposed cue-list namespace with number/name/ID/
  tags; pane contract; BLE sidecar in Rust with `btleplug`, speaking Choufleur's
  opcode table; Go.dot relays the buzz. Embedded vs docked decided here.
- **OSC device templates** as OSCQuery namespace descriptions; ADM-OSC built in.
- **Authoring from a processor** (PRD §3.26, added 2026-09-06): the capture verb
  that lets WFS-DIY write a cue rather than only be commanded by one — QLab's
  authoring API in Go.dot's own protocol. It needs no new transport (§4.11 already
  makes every gesture a command); what it needs is the verb, an explicit landing
  place, and a cue-list view for the processor. Here because a capture is a solved
  state written down, so it wants §3.13 finished.
- **Stream Deck** profile (bitmap renderable, triggering role); **SpaceMouse**
  as a rate endpoint; further surface profiles. *(2026-10-08: the SpaceMouse
  arrives first as what records an OSC cue's curves - the item before Phase 10,
  namespace draft §45.)*
- **OSC and MIDI processing cues as persistent processes** (PRD §3.29): the
  state-machine phase §3.5 deferred, arriving as rows in the persistent section;
  a stateful process restarts at its resting state, and no fixed pool is needed
  because the control graph has no rebuild cost. *(2026-10-09: brought forward as
  process cues - Pure Data patches, serial with them - the item before Phase 10,
  namespace draft §51.)*

**Done when:** the sound operator's Choufleur column is populated from Go.dot
and a Go.dot warning taps the wrist.

**Needs from the author:** the cue-notation ↔ ID mapping (PRD §6.8).

---

## Phase 12 — Redundancy and replay · L

- **Engine sync** from the tick-indexed log (Phase 1 pays off): intent +
  position, never derived runtime state; asymmetric feedback; tablet as
  failover surface.
- **Deterministic replay** promoted from test harness to a tool: record a
  rehearsal, replay it, diff the outputs.

**Done when:** killing the primary mid-show leaves the backup running the same
cue with the tablet controlling it.

**Needs from the author:** the redundancy design he said he'd imagine once the
architecture settled (PRD §3.15).

---

## Cross-cutting, every phase

- A **replay-log fixture** per phase, run in CI.
- **RT-safety instrumentation** stays on in tests.
- **Locale test**: every serialisation test runs under `fr_FR` as well as `C`.
- **Two-surface / two-client write test** once bindings exist.
- No *(proposed)* item implemented without a recorded yes.
- Easter eggs only in the lobby (PRD §4.7) — *Rien à faire* and *They do not
  move* may land in Phase 5; nothing in an error path, ever.
