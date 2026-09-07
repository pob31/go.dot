# Handoff — Phase 4, from the end of Phase 3

**Written 2026-09-07 at `d475a4e`, with Phase 3 complete and CI green on three platforms.**

This is the *builder's* half of the handoff. The author-facing half — the five PRD amendments
Phase 3 proposes, what it deliberately left undone, what it measured, and what is still needed
from the author — is [`../godot-phase3-closeout-0.1.md`](../godot-phase3-closeout-0.1.md), and
[§12.15 of the namespace draft](../godot-namespace-draft-0.1.md) says what §12 drew against what
was built. **Read the close-out first.** Nothing here repeats it.

What is here is the six things Phase 4 will touch that Phase 3 built, one thing it will find
already true that reads like a bug, and four traps that cost time this phase and will cost it
again.

---

## 1. The four seams Phase 4 replaces, and exactly what they are today

Phase 4's allocator "replaces arming with claims" (plan, §*Arming*). Here is what it is
replacing, so that the replacement can be a change of one thing rather than an archaeology.

**`audio.arm <cue> [run]`** — an operator command, in `Runner.cpp`'s command block rather than
in `RunCommands.cpp`, because it is an *action*: it reserves a voice and asks the audio side for
media. `RunCommands.cpp` holds only what the machine says happened, which is what keeps that
file applicable on a machine with no sound card. A claim will want the same split.

**`Runner::armStandby`** — the hook. Notices that the focused list's standby has moved and
submits `audio.arm` once per pointer position, for each cue `armablesFor` names. It does not act
directly, because a hook does not run during a replay and a run created outside the log is a run
the replay would not have.

**`Runner::armablesFor (cue)`** — what a pointer on a *group* means: the first enabled member of
a sequence, or every member of a timeline whose `preWait` is nought, recursively. This is the
lookahead. Phase 4's prepare horizon is the same question asked further ahead, and this function
is where "further" goes.

**`Runner::armMedia` → `ArmRequest` → `Player::requestArm`** — the crossing. `ArmRequest` is a
*value* with no document reference in it, because the tick thread fills it in and the message
thread acts on it. A claim that carried a `ValueTree&` would be the bug this shape exists to
prevent.

**The allocator that exists today is four lines**, in `RunTable::lowestFreeTrack` /
`isTrackBusy`: the lowest track no live run holds. Lowest rather than round-robin *so that a
show replayed puts the same cue on the same track* — two logs of one session compare line for
line. Whatever Phase 4 replaces it with has to keep that property or every replay fixture in the
tree changes meaning.

---

## 2. The header, as Phase 3 left it for Phase 4 to take over

`Header` is an identified element holding ordinary cues; `groupPhase::header` runs them **as a
sequence whatever the group's own mode is**, and the members wait for it. The comment on that
enum already says why: "a header is preparation and preparation has an order". The cursor never
enters one, `standby.set` refuses one, and a cue inside publishes `role = header`.

**Nothing forecloses the prepare horizon**, and two things actively invite it: the phase is
already a distinct state a client can watch, and the header already blocks its members. What
Phase 4 adds is *when* it runs relative to the group being reached, which today is "at entry"
and needs to become "when the horizon reaches it".

`godot-open-questions-0.1.md` §5 is the author's own idea for what else goes in a header —
presets derived from members — with four things to settle first. Worth reading before designing
the horizon, because it changes what a header *is*.

---

## 3. The run tree IS the liveness data, and finished runs are never lost

Phase 4 wants "live-range liveness analysis". The data is already there and the retention rule
is the opposite of what it looks like:

**A finished run retires from the TREE and never from the TABLE.** `retentionTicks = 250`
removes the published address at `/godot/run/<id>` five seconds after the run ended — Gogo is
present tense, and a four-hour show must not publish four hours of runs every tick. The `Run`
stays in `RunTable` for the life of the session.

**And that is deliberate, for a reason that also constrains Phase 4.** Pruning the table would
make the model depend on a hook, and hooks do not run during a replay: a `run.kill` arriving six
seconds after its run finished would be *rejected* live and *applied* on replay, which is a
session that does not reproduce itself.

So a solver walking backwards over run history has the whole history. What it must not do is
assume the published tree is that history.

Each run carries `parent`, `children`, `cue`, `kind`, `track`, `state`, `endedAtTick`, and —
since PR 3.12 — `ownLevel` and the derived `level`.

---

## 4. What is fixed when the graph is built, and why that is a constraint rather than a bug

§3.25 fixes the graph's shape at show load. Phase 3 made that two numbers rather than one:

- **`Audio/@tracks`** — the polyphony ceiling.
- **`EditSpec::slots`** — the widest range count of any media cue in the show, computed by
  `widestRangeCount` in `Console.cpp` and never stored in the document.

A range added past that during a show has nowhere to be armed and is refused with `no-slot`
until the show is reloaded. **M10 and M11 are why this is affordable**: node identities are
unique at 1..64 tracks × 1..8 slots, and eight slots cost about seven microseconds of a
667 µs block.

**The headroom Phase 4 is spending is therefore about two thirds of a block** at the polyphony
ceiling (M11: ~223 µs of 667 µs at 32 tracks × 8 slots × 64 outputs @ 96 kHz, in Release).
Processor slots come out of that, and the measurement to extend is `measureBlockCost` in
`AudioTests.cpp` — it already takes a slot count and interleaves its comparisons.

If the allocator needs the graph's shape to change while a show is open, that is a **PRD
conversation** and not an implementation detail: §3.25 is explicit, and M4 measured a rebuild on
playing audio as bit-identical only for the arming case.

---

## 5. Claims are references, and there is now one place that knows what a reference is

PR 3.2's `refers` column landed at the end of Phase 3. An identifier-valued attribute declares
in the parameter table what it must point at — `list/@standby` a cue, `fade/@target` a cue,
`route/@bus` a bus, `midi/@port` a port — and `ShowDocument::warnings()` checks every one of
them by identifier **and by kind**, because a fade whose target is a bus would otherwise pass.

**A dangling reference is a warning, never a load refusal**, and `validate()` and `warnings()`
are two functions for exactly that reason. Phase 4's claims — a cue claiming a processor slot, a
channel, a bus — should use the column rather than growing a fifth hand-written check. That was
the whole argument for building it.

---

## 6. Mutation rate is now something to think about, and M9 is why

The tree used to rebuild the mounted namespace on every applied mutation. With WFS-DIY's own
capture that was 3.13 ms of every cue rename — twenty-nine times the rest of the tree, and 16%
of a tick. It is a separate cache now, invalidated by a revision counter `MountTable` bumps
itself rather than by a flag anybody has to remember to set.

**Phase 4 adds mutation rate again**: prepare/commit writes claims, a solver writes waypoints,
and load-to-time writes a great deal at once. Before adding a fourth thing that marks the tree
stale, look at `ParameterTree::rebuildDocumentPart` and ask whether it belongs in the document
half at all. The pattern to copy is `rebuildMountPart` — a cache with its own invalidation
source, asked rather than told.

`M9` in `MountTests.cpp` reports the numbers and asserts the guarantee **by counting rebuilds
rather than by timing**, which is the shape to copy for any measurement that has to survive CI.

---

## 7. Four traps, each of which cost real time in Phase 3

**`Edit::ScopedRenderStatus` is not a batching object.** It is the obvious thing to reach for
when several ValueTree writes should cause one rebuild, and it calls `freePlaybackContext()` on
construction — destroying the running playback context, which `AudioHost` caches and the audio
thread reads every block. The result was a segmentation fault in `wfg serve --hosted`, four
ticks after the banner. Nothing has to be inhibited anyway: every write sets the same
`shouldRestartPlayback` bool, so N writes plus one `dispatchPendingUpdatesSynchronously()` at
the end *is* one rebuild. `TransportControl::ReallocationInhibitor` is the safe one of the two
and is also unnecessary.

**Audio time is not wall time on a loaded runner.** `HostedAudioDriver` is real-time paced — it
waits on an absolute per-block deadline — but a Debug build that cannot render a block inside a
block period delivers every block late and never catches up. A black-box driver that sleeps for
three seconds and then asserts about how much of a file has played is asserting something
different on every machine. Wait on `/godot/engine/tick`, which is the sample counter; see
`wait_ticks` in `phase3_groups.py`. This cost two wrong diagnoses before it was measured.

**A transition is in the log; a state may not be observable.** Anything a driver wants to know
about *what happened* should be read from the session's own log rather than polled for. §3.15
makes transitions events precisely so that they can be seen afterwards, and `ranges_entered` in
`phase3_groups.py` is the pattern.

**CI flakes on shared runners, and three specific tests are the ones that do it**: M4's
bit-identical null test (macOS), `phase1_session`'s WebSocket push checks (Windows), and the
fade step bound (Windows). Each failed once during Phase 3 on a commit that could not have
caused it and passed unchanged on re-run. *Re-run first — and then look properly*, because one
failure that looked exactly like these three was a real race in a new driver.

---

## 8. Where to start

1. Read the close-out §1 and §4. Two of the five amendments (`advance at range end`, §3.9b's
   proposed items) block work Phase 4 is scheduled to do.
2. Read `godot-open-questions-0.1.md` §3 (DCA-style control) and §5 (the header as a preset
   sheet). §3's arithmetic already exists — a run's level is `ownLevel + Σ ancestors'` since
   PR 3.12 — so a DCA is that structure reached from a fader rather than from a cue.
3. Draw §13 of the namespace draft before writing code, as §11 and §12 were drawn. Both phases
   found that worth it, and §12.15 exists because §12 was a text to be reviewed against rather
   than a memory.
