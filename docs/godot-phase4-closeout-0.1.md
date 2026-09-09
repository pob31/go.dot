# Phase 4 close-out

*Written 2026-09-09, at the end of PRs 4.0–4.11. Four things: what the phase amends in the PRD
(proposed, never edited here), what it deliberately left undone, what it measured that the next
phase will be built on, and what is still needed from the author.*

*This is the author-facing half. The builder-facing half — what Phase 5 inherits, which seams it
takes over, and the traps that cost this phase time — is
[`handoffs/2026-09-09-phase5-handoff.md`](handoffs/2026-09-09-phase5-handoff.md).*

---

## 1. PRD amendments, proposed

**Nothing in this section has been applied.** The PRD is the author's document; `CLAUDE.md` is §4
of it reproduced byte-for-byte and gated by `scripts/check-claude-md.py`, so an amendment made here
would either break that gate or silently rewrite the review criterion for every pull request. Each
of these is a sentence to change, with what building the thing taught.

### §3.9b — "the processor declares its own slots"

The section reads as though a processor's inputs are discovered. They are not, in Phase 4 and
probably ever: the show declares them, under the mount, as `Slot` rows carrying a name, an address
prefix, a width and the bus that feeds them (decision 1, 2026-09-07). What a processor that *can*
be asked adds is a **check** — `wfg validate` warns when a declared slot's address is absent from
the mounted namespace — and that is a different verb from *declare*.

**Proposed:** "the show declares the processor's inputs as slots; where the processor can be asked,
the declaration is checked against what it says it has."

The reason is the one the whole mount design rests on. A namespace is a captured or hand-written
file, most devices do not run an OSCQuery server at all, and a pool that only existed when a box
answered would be a pool that vanished when somebody unplugged it during focus.

### §3.13 — what a manual waypoint *is*

§3.13 keeps manual waypoints "as a way to force a divergent world back into agreement" and leaves
their shape open. The author's decision (R, 2026-09-07) settled it: **there is no waypoint object**.
The engine keeps the list's own history of steps — the last sixty-four, `<tick>:<cue>:<origin>` —
and each step is a load-to-time target, so going back is picking a row rather than authoring
anything.

**Proposed:** a sentence naming the history as what a manual waypoint is, and saying that the
structural ones live inside the solver.

### §3.12 — read before write

§3.12 says what anticipation is and does not say what makes it revocable. Building it made the
condition exact: **a value is pre-sent only when the node is `anticipatable` AND its mount can be
asked**, and the value that was there is READ FIRST and kept on the run as the restore. An
anticipatable node on a mount that cannot answer is a `wfg validate` warning and is left for entry.

**Proposed:** one sentence — "a value is pre-sent only where it can be read back first, because a
value nobody can restore is a value nobody can revoke."

### §3.9c — the cross-list override

§3.9c proposes that a cross-list overlap could be refused rather than warned. Phase 4 did not build
it, and the reason is the section's own rule: *warn, don't refuse*. What it built instead is
`Feed/@shared`, a mark on either cue that says the sharing is deliberate and silences the pair.

**Proposed:** withdraw the *(proposed)* cross-list refusal in favour of the shared mark.

### §3.29 — the persistent section's suspensions

Two of §3.29's *(proposed)* items were taken as yes (decision S): a **kill** on a persistent run
suspends it for the session, until a load-to-time re-solves; a **stop cue before standby** aimed at
it suspends it because the document says so. A group-level section and Esc-as-pause remain proposed
and are not built.

**Proposed:** promote those two from *(proposed)* to text, with the load-to-time lift named.

---

## 2. Deliberately not in Phase 4

- **Eviction.** A claim carries a `releasePolicy` and a release is a close, never a kill; nothing
  forecloses the waiting-claim-with-eviction shape Phase 6's sampler group will want.
- **The rack's tracks, sends and plugins** (Phase 9). Phase 4 declares the pool and allocates from
  it; an `Insert` is bookkeeping with no audio effect, claimed, released, degraded and warned about
  like any other claim.
- **Time-tagged bundles** (§3.12's second paragraph). spatcore's OSC parser drops time tags, so the
  author's own processors would ignore them; a spatcore change comes first.
- **`LISTEN` on mounts.** The observation sweep asks; nothing subscribes.
- **Strips, bindings, fader-start, sampler groups, DCAs** (Phase 6). The slot table reserves the
  kind and the claim carries a policy; nothing else.
- **Authored waypoints**, per decision R.
- **The in-range offset.** A jump into a *ranged* cue enters the right range at that range's start
  rather than partway into it. `validate()` already refuses a `startOffset` beside a `Range`, so the
  whole-file offset M17 measured is the only place an offset can apply today; landing partway into a
  range needs `armRangeInto` to shift and shorten one slot's clip, which is an audio-side change
  with a measurement of its own.
- **Three replay fixtures** — `slots`, `claims` and `persistent`. Each of the three is a hook's
  decision re-injected from the log rather than re-decided, so what a fixture would pin is the
  record shape rather than the behaviour; the black-box session replays record for record and covers
  the same ground end to end. They are named here rather than quietly dropped.
- **The equivalence test** (§13.8's own words: the solver's plan at tick T against what a session's
  log says was live at T). The solver is checked against the scheduler *indirectly* — the Phase 4
  driver jumps into a running show and the render agrees with the plan to the sample — but the
  systematic version over the four deterministic fixtures is not written. It is the single largest
  piece of confidence left on the table, and it is where Phase 5 should start if it touches the
  solver at all.

---

## 3. What Phase 4 measured

Six measurements, all on the Windows box, all in a Debug build unless said otherwise. The full
numbers are in the commit messages and in namespace draft §13.14.

| | what | answer |
|---|---|---|
| **M16** | whether launching a clip in slot *b* of a track stops the clip playing in slot *a* | **it does not.** A track keeps both slots playing and they sum: slot 0 at mean 0.25, both at 1.0, no frame belonging to either alone. The guess §3.25 was built on was wrong |
| **M17** | whether a clip armed with an offset lands on the sample | **exactly.** 0.5 s, 1.25 s and 2.0 s each landed **out by 0 samples**. `LaunchHandle::nudge` is not reachable from Go.dot's own code, so load-to-time relaunches rather than nudges |
| **M18** | the liveness analysis over a 500-cue, 20-slot show under a burst of moves | **one mutation, one rebuild**, counted rather than timed. 208 ms per rebuild, of which `findById` is 131 ms — which is a debt, not a measurement |
| **M19** | a prepared header against the mock target | the block settles **1 tick** after the pointer lands, with one read-before-write round trip per anticipatable node |
| **M20** | `solve` over a 500-cue show | **17.7 ms** per call in Debug with iterator debugging — roughly an order of magnitude above the shipped build. The node is capped at 5 Hz and a drag re-solves at the drag's own rate, so what it has to fit inside is a gesture rather than a tick. 40 distinct addresses out of 167 network cues |
| **M21** | the observation sweep, in the two shapes it could take | **per address, by two orders of magnitude.** One subtree GET of WFS-DIY's capture is 1.09 MB and 2 480 nodes and costs **135 ms** to digest — nearly seven ticks, every second. Forty single-value replies cost **0.6 ms**, three per cent of one tick. Break-even is past eight thousand written addresses, more than the capture holds |

**What the measurements changed, rather than confirmed.** M16 falsified the guess §3.25's sampler
claim was built on and is now written into the PRD. M17 made load-to-time's landing exact and
removed the nudge branch before it was written. M20 set the aim node's rate cap. M21 decided the
probe's shape outright — a subtree read would have cost seven ticks a second for data a show does
not use.

---

## 4. Still needed from the author

- **The five amendments in §1**, each of which is a sentence.
- **The voices claim shape.** M16 is answered and in the PRD; what is left is the choice between
  *a group declares its voices and claims that many tracks* and *a claim per slot, a bank of eight
  being eight voices*. §3.25 and namespace draft §13.15 name both. **Before Phase 6**, and Phase 4's
  allocator assumes neither.
- **The idle-colour policy of §3.30** — whether a strip shows its idle colour or its timbre while a
  cue sounds — which arrived with the spectral-colour section and is marked *(proposed)*.
- **Whether a rack channel's failure policy is *degrade*.** Built as §3.9e's own table says, and
  the devplan asks for confirmation: a busy exclusive channel means the run plays dry and says
  `no-channel`. It has never been overruled, and this is the last chance before Phase 6 builds on it.
- **The Phase 3 amendments still waiting** (close-out 3 §1), `advance at range end` in particular:
  the solver reconstructs "which range, what pass", and the verb needs defining before that
  reconstruction can be trusted to mean what the author meant.
- **The hardware pass**, which needs the room: a `Mount/Slot` naming a real WFS-DIY input, a Feed
  heard on it, a prepared header positioning it before GO, and `verified` against the real processor
  — Phase 2's last open item, still open, and now with something specific to try.
