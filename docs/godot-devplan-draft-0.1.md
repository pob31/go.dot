# Go.dot — Development Plan (suggestion for Claude Code)

**Draft 0.1**, derived from PRD draft 0.7 (still current against 0.8 — the 0.8
amendments record spike results and change no phase ordering). A proposed phase order, not a
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
- The author designs the UI layout (PRD §3.17). Phase 5 builds a *minimal
  functional* UI so the engine can be exercised by a human, not the final
  design.

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

**Done when:** load-to-time into the middle of a scene lands the right cues at
the right offsets with the right slots claimed; reordering cues produces the
right overlap warnings; all headless.

**Needs from the author:** the *(proposed)* items in §3.9b (stereo → two mono
slots; processor-declared slots).

---

## Phase 5 — Minimal desktop UI (Didi and Gogo) · L

**Goal:** the first build a human can run a rehearsal with. Functional, not
designed.

- JUCE UI as a pure OSCQuery client. If it needs a capability the engine lacks,
  add it to the engine first.
- **Cue list** with the sacred conventions (PRD §3.5): decimal numbers, vertical
  rows, GO on space. Three pointer kinds visibly distinct, colour not the sole
  carrier. Standby never moves on scroll or select.
- **Running pane**: run pointers, kill/advance/prune, round pills, range name
  and iteration on the strip.
- **Group bulk-edit view** with type filter (PRD §3.5).
- **Header pane**: written lines upright, lines derived from a member's preset
  mark in italics, double-click on a derived line opening the member's inspector
  (open questions §5). Needs Phase 4's mechanism, not just the display.
- **Curve editor** with breakpoint list and numeric entry.
- **Dark UI mandatory** (PRD §2). Layout presets design/tech/show; show mode
  locks the layout and disables editing.
- Undo histories per domain, crash-safe autosave.

**Done when:** the author runs a simple show from the desktop build in a
rehearsal room.

**Needs from the author:** the layout — he designs it; this phase implements
whatever is needed to exercise the engine and no more.

---

## Phase 6 — Control surface and bindings · L

**Goal:** the D700 driving a show; fader-start working.

- **Surface abstraction** (PRD §3.16): strips, transports, device profile vs
  layout, change-origin tagging, touch gating. Endpoint classes absolute /
  relative / rate.
- **Mackie transport first** (vendor recommendation). HUI when and if decided
  (PRD §6.10).
- **D700 profile** from vendor-published files only (PRD §6.4). Display as a
  renderable with the bounded field vocabulary; user-selectable fields per
  line.
- **Bindings** with automation modes, cue-scoped lifetimes, DCA trims, pinning,
  explicit update-cue capture.
- **Fader linking** (PRD §3.9a): start value in prepare, fader-start with
  hysteresis, fader-stop on touch-release at -inf.
- **Banking**: implement the simplest option first; decide in practice (PRD
  §3.9d).
- Standing test: two surfaces on different protocols bound to the same node.

**Done when:** a fader-start cue fires from the D700 with the audio already
armed; a group DCA follows automation on motorised faders; the scribble strips
show provenance.

**Needs from the author:** Asparion extension byte list or the chosen vendor
package; banking policy as it emerges; the OLED field layout he wants.

---

## Phase 7 — Tablet client · M

**Goal:** the running pane in the operator's hands.

- Web client (TypeScript) over OSCQuery + WebSocket. Full surface layout
  rendering, so it is a genuine fallback (PRD §3.17).
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

**Done when:** an audio+video scene plays in sync for a full act with the mesh
aligned from the tablet.

**Needs from the author:** DeckLink vs GPU output (PRD §6.3); fallback codec
list; blend-space choice confirmation.

---

## Phase 9 — Plugins and the rack · L

**Goal:** third-party processing that cannot take the show down.

- Built-in plugin set, in-process, lipogram-compliant.
- **Out-of-process proxy plugin** as a custom TE plugin type wrapping
  shared-memory IPC with a hard deadline (PRD §3.18 / §6.1 #7). Opt-in inline.
- **Out-of-process scanning**, always.
- **Live rack** with a stated latency budget; TE PDC behaviour on live tracks
  understood and controlled (spike #6).
- VST3 / AU / LV2 all exercised.

**Done when:** live input runs through a sandboxed third-party plugin, the plugin
is killed mid-show, and the show continues with the strip marked failed.

**Needs from the author:** the built-in plugin list; the rack's latency budget.

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

**Done when:** a show chases timecode from an external source without drift or
discontinuity, and each stop level does exactly its guarantee and nothing more.

**Needs from the author:** the Go Doh! inventory of in-flight objects (PRD
§4.4, deferred).

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
