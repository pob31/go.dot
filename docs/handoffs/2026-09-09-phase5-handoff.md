# Handoff — Phase 5, from the end of Phase 4

*Written 2026-09-09, at the end of PRs 4.0–4.11. The builder-facing half of the close-out: what
Phase 5 inherits, what it must not break, where the seams are, and the traps that cost this phase
time. The author-facing half — amendments, measurements, decisions still needed — is
[`../godot-phase4-closeout-0.1.md`](../godot-phase4-closeout-0.1.md).*

Phase 5 is the first build a human runs a rehearsal with: a JUCE desktop client, **as a pure
OSCQuery client**. That constraint is the most important sentence in this document, and Phase 4
spent its whole console budget proving it holds — every gesture the page makes is a datagram, and
every reading is a node. If Phase 5 needs a capability the engine lacks, it goes in the engine
first, with a row in `docs/parameters/godot-parameters.csv` and a command by name.

---

## 1. Everything Phase 4 publishes, and where it comes from

The tree has **two halves with two invalidation sources** (`ParameterTree.cpp`), and knowing which
half a node is in tells you when it changes and what a replay does with it.

| half | rebuilt when | holds |
|---|---|---|
| document | told (`markStale`) | the show: lists, cues, ranges, routes, feeds, inserts, slots' declarations, mounts |
| runtime | every publish | runs, claims' current holders, `prepare`, `aim`, `solve`, `statePosition`, `history`, `document/warnings`, slots' `holder`/`pending`/`usage`/`overlaps` |

**Anything that changes while the show does not is in the runtime half**, published against the
rosters the document half leaves behind (`declaredCues`, `declaredSlots`, `declaredLists`). A
replay never tells the document half anything, so a node that belongs in the runtime half and is
emitted from the document half simply stops existing in `wfg replay` — which is how the rule was
found. `addContainers` must exclude the containers the runtime half owns, or a client sees the same
container twice.

What Phase 5 will draw, all of it already published:

- `/godot/cue/<id>/{role, prepare, preset, duration, headerDerived}`
- `/godot/run/<id>/{phase, claims, pending, warning, asserted, offset}` beside Phase 3's fields
- `/godot/slot/<key>/{kind, name, width, holder, pending, usage, overlaps}` for voices, processor
  inputs and rack channels alike
- `/godot/list/<id>/{aim, solve, statePosition, history, persistentOrder, persistent}`
- `/godot/document/warnings` — one warning per LINE, which no other list-valued readout here is,
  because a warning is a sentence and sentences contain spaces
- `/godot/engine/analysisRebuilds`

`clients/console/index.html` draws every one of them already. It is 1 400 lines of plain HTML with
no build step, and it is the reference for what a client is allowed to assume: poll `GET /godot` at
10 Hz, write binary OSC, and never hold state the engine owns.

---

## 2. The four things Phase 5 must not break

**1. The document holds what somebody decided; the engine holds what it is doing.** §4.10, and it
is the rule that decides where every new field goes. A run's `startOffset` is engine state because
it is where an operator jumped to; `media/startOffset` is document state because it is what the
designer chose. Getting this backwards is what makes a show file that cannot be diffed.

**2. The hook decides, the handler applies, and a handler never submits.** A replay re-runs handlers
AND re-injects records, so a handler that submits duplicates every record it makes. Phase 4 broke
this once (`prepareStandby` submitted `run.spawn`) and the phase 3 black-box replay is what caught
it. Every decision this phase added is a logged engine-origin command: `run.prepare`, `run.revoke`,
`claim.land`, `run.assert`, `mount.readback`.

**3. Only GO moves the standby.** §3.5. `cue.fire`, `trigger.fire`, `run.assert` and the whole
persistent section move nothing, and the Phase 4 driver checks it after every one of them. A UI
that moved the pointer on selection or scroll would break the one promise an operator relies on in
the dark.

**4. Colour is never the sole carrier.** §4.8. Every state Phase 4 publishes has a word: `pending`
is the word "pending", `asserted` is the word "asserted", an overlap is a sentence in
`document/warnings`. §3.30's spectral colour is explicitly an *aid* with brightness monotonic in
frequency, which is how it stays inside the rule — do not let the strip's colour become the only
way to read something.

---

## 3. What Phase 5 takes over, seam by seam

**The header pane** is the one piece of Phase 5 that needs Phase 4's mechanism rather than its
display. `cue/preset` is a mark on the MEMBER naming an ancestor group; `group/headerDerived` is
the derived line, computed at read time and never written into the `Header` element. So editing a
derived line IS editing the member, deleting the member removes the line with no repair, and there
is exactly one object. The console draws derived lines in italics inside the header band with a
marker on the member's own row; Phase 5's double-click-to-open is the same gesture with a real
inspector behind it.

**Spectral colour** (§3.30) is looked up through 4.1's `MediaInfo`, and there is a keying mismatch
to resolve first: `audio::mediaDurations` returns `std::map<path, double>` — keyed by PATH — while
§3.30's cache is keyed by CONTENT HASH. Grow `MediaInfo` into a per-file record holding duration,
content hash and a pyramid handle, rather than adding a second parallel map. `/godot/run/<id>/
timbre` is then a row in the CSV and a table lookup in the runtime half, beside `position`.

**The curve editor** needs nothing new: a fade's curve is document state and `FadeJob` reads it
every tick.

**The running pane** is drawn in the console today and is the closest thing to a specification.
What Phase 4 added to it: `phase`, `claims`, `pending`, `warning` and `asserted`.

---

## 4. Six traps, each of which cost this phase real time

**1. `-Wshadow` is invisible on the Windows box, and it cost three red CI builds.** MSVC does not
warn; the strict Linux build does, with `-Werror`. Before pushing, check every new member and every
new local against the enclosing scope by hand. The same goes for standard-library includes: MSVC
provides `<map>`, `<algorithm>` and `<cmath>` transitively where GCC does not.

**2. A script that rewrites a source file reads AND writes in binary, or in text mode, never one of
each.** Mixing them doubles every carriage return, after which multi-line search strings stop
matching **silently** — three edits reported success and changed nothing. It cost a whole-file
repair commit.

**3. A black-box driver waits for the THING, never for the TIME.** Three flakes in this phase, each
a wait whose length was a guess about the runner: a subscription's echo, an error count, a save's
log record, and the render's own tail. `common.wait_until` is the tool; the rule is that a check's
wait must see what the check reads. The render is the subtle one — a WAV's header carries its
length, the writer rewrites it once per second of audio, and `serve` is stopped with
TerminateProcess, so the file ends at the last header rewrite. `first_sound.wait_for_render_tail`
does the arithmetic.

**4. The render drops blocks when the disk is busy, and a dropped block looks like a launch.** The
hosted writer's FIFO drops rather than blocking (deliberately: a stuttering recording beats a
stalled graph). A driver reading "where did this cue start" off a ramp must therefore accept any of
the silence-then-loud edges rather than the last one.

**5. A finished run is published for five seconds and then is not.** `run_for_cue` on a cue that has
sounded before will answer with the old run. When a test means "the run this gesture made", it has
to name the runs that already existed and exclude them.

**6. Two things that are separately correct meet at a seam nobody tested.** All three engine faults
PR 4.11 found were of this shape: a derived-only header against a `partial` count that assumed a
written one; a plain media cue at the pointer against a revocation written for prepared blocks; a
preset cue already run against a member phase waiting for something armed. The unit suite could not
see any of them because each half was right. **A driver that drives a whole show is what finds
these**, and it is worth writing before the phase feels finished rather than after.

---

## 5. The debts, named

- **The equivalence test** (§13.8): the solver's plan at tick T against what a session's own log
  says was live at T, over the four deterministic fixtures. Not written. It is the stated reason to
  trust the solver, and the largest piece of confidence left on the table.
- **`ShowDocument::findById` is a depth-first walk per call**, and M18 measured it at 131 ms of a
  208 ms analysis on a 500-cue show. Its own comment asks for a cache invalidated in one place, and
  `revision()` is now that place.
- **Three replay fixtures** — `slots`, `claims`, `persistent` — named in the close-out §2.
- **The in-range offset**: a jump into a ranged cue enters the right range at its start.
- **The console owes 4.2's slot inspector lines and 4.3's claims-and-pending on the running pane**;
  4.4's warnings, 4.8's aim bar, 4.9's steps and 4.10's persistent band are in.

---

## 6. Where to start

1. Read the close-out §1 and §4. The voices claim shape and `advance at range end` both block work
   Phase 6 will do, and the author answers faster when the question arrives with a measurement.
2. Run `wfg serve tests/fixtures/bundles/phase4 --hosted --ui=clients/console` with
   `tests/blackbox/mock_target.py` beside it. Everything Phase 4 built is visible in one page in
   about a minute: park on the scene and watch `prepare` reach `verified` with the desk's value
   arriving before any GO, move away and watch it come back, fire the cue after the scene and read
   *pending* in words, drag the aim and read the solve, press *load to time*, move a cue and watch
   the overlap appear.
3. Draw the UI's own section of the namespace draft before writing code, as §11, §12 and §13 were
   drawn. Three phases have now found it worth the day it costs.
