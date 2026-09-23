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

**Needs from the author:** DeckLink vs GPU output (PRD §6.3); fallback codec
list; blend-space choice confirmation.

---

## Phase 9 — Plugins and the rack · L

**Goal:** third-party processing that cannot take the show down.

*Split on 2026-09-23 into 9a, pulled forward and being built, and 9b, what is
left. No later phase is renumbered.*

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

*Status, 2026-09-23 late:* 9a.0–9a.8 on `main` and CI green; 9a.9 landed without the FX panel at the
foot (the model, the Plugins tab and the gestures are in; the component is the next session's);
9a.10 waits for a quiet machine; §17.12, the pages draft's §8 and the handoff are written.

After 9a.0, four streams run on disjoint files — the DSP, the document object,
the hosting and the transport — and the FX entries land on top of all four.

**Done when:** a media cue's EQ is heard and drawn from one function; every EQ
and insert parameter is a `node.set`-able node with its name, range, default,
bipolar flag and value text published beside it, and the insert order is
readable (the pages draft's §8, all four items); a third-party VST3 plays on a
cue through the sandbox with its parameters ridden live; a plugin killed
mid-show leaves the cue dry, the strip marked failed in words and the block
cost bounded; M30–M34 recorded; CI green on all six jobs.

**Needs from the author:** what he sees on the desktop once 9a.3 and 9a.9 land
(plan decisions 1, 5, 7, 12 and 14 of §17.11 are the ones a look settles); the
deadline and the failed-strip budget once M31 and M32 are taken; and whether
the pages session should start on the EQ page when 9a.3 lands rather than wait
for the inserts.

### Phase 9b — The live rack, and what 9a leaves · L

- **Live rack** with a stated latency budget; TE PDC behaviour on live tracks
  understood and controlled (spike #6). Live input through the sandbox.
- **Rack channels as slots** (PRD §3.18, 2026-09-07): plain tracks with a plugin
  list, never Tracktion Racks; sends as coefficients fixed at load; width
  classes mono→mono, mono→stereo, stereo→stereo — the chains behind Phase 4's
  `Media/Insert` and `Rack/Channel`, and the shared reverb channel.
- **Inline hosting** (PRD §3.18's opt-in) with §3.4's message-thread handover;
  **LV2**; AU presets; plugin editor windows; curated per-plugin parameter maps
  (the pages draft's §7.2); macOS audio workgroups for the child.

**Done when:** live input runs through a sandboxed third-party plugin, the plugin
is killed mid-show, and the show continues with the strip marked failed; a cue
claims an exclusive rack channel and enables one plugin of its chain without a
graph rebuild.

**Needs from the author:** the rest of the built-in plugin list; the rack's
latency budget.

---

## Phase 10 — Timecode, panic, hardening · M

**Goal:** the stop levels and the sync sources that a touring show requires.

- **LTC/MTC chase and generate**; tick re-anchoring when chasing (PRD §3.14 —
  settle the derivation before writing the transport).
- **Esc / double Esc / Go Doh!** per PRD §4.4: graceful abort runs footers;
  immediate skips them and kills internal processing only; Go Doh! recovers an
  early trigger within the anticipation window. Revert-of-GO (§4.5).
- Panic values on every node honoured.
- Debounce as a user preference.
- **Esc on a persistent media cue as a pause** (PRD §3.29, *(proposed)*): if
  adopted, §4.4 gains a sentence and `CLAUDE.md` is re-copied, never edited. A
  kill from the running pane suspends a persistent assertion; a double Esc does
  not, and the next GO restores the declared world.

**Done when:** a show chases timecode from an external source without drift or
discontinuity, and each stop level does exactly its guarantee and nothing more.

**Needs from the author:** the Go Doh! inventory of in-flight objects (PRD
§4.4, deferred); the Esc-as-pause decision (§3.29).

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
  as a rate endpoint; further surface profiles.
- **OSC and MIDI processing cues as persistent processes** (PRD §3.29): the
  state-machine phase §3.5 deferred, arriving as rows in the persistent section;
  a stateful process restarts at its resting state, and no fixed pool is needed
  because the control graph has no rebuild cost.

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
