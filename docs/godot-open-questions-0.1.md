# Open questions — fades, waits, and what a fader is for

*Started 2026-09-06, during Phase 2. This is a working list rather than a specification: things
to go and measure, decisions that have surfaced early and have nowhere else to live yet, and
one thing the author knows they have forgotten.*

Two of these already have answers and are recorded here so the answer does not get lost between
the commit that implements it and the phase that reconciles the documents. *Updated 2026-09-06
with the Phase 3 plan: the group-fade question and the second-GO question now have answers
(decisions O and N, namespace draft §9); the relative-fade half of §2 does not.*

---

## 1. What to check in QLab

QLab is not the target — Go.dot exists because QLab cannot do several of these things at all —
but on the questions where it *does* have an answer, that answer is thirty years of theatre
practice compressed into a default, and inventing a different one without knowing what it says
is how a program ends up surprising people who already know how to work.

### 1a. The fade questions

**The case at hand.** A fade fired at a cue that a fade-and-stop is already taking down.

- Does the stop still land at its original time? *(Go.dot's answer, author 2026-09-06: **yes**.
  Implemented — the arrival is kept as an absolute tick and inherited by whatever takes over.)*
- Does the level follow the new fade, the old ramp, or both summed?
- What level is the cue actually **at** when it stops — and does QLab click there? A fade-and-stop
  exists so that the clip stops in silence; a fade that took the level back up defeats that, and
  it is worth knowing whether QLab accepts the click, prevents it, or never let you get there.

**Concurrent absolute fades.** Two absolute fades live on the same target, neither of them a
stop. Does the later one win outright, or is there a rule about which? *(The author has said
relative fades compound; absolute-on-absolute is the case still to check.)*

**Relative over absolute, and absolute over relative.** The author's reading is that in QLab
several relative fades compound and eventually compound with an absolute one. Worth confirming
the direction: does an absolute fade **reset** the accumulated relative offset, or is it a new
base that the offsets continue to sit on top of?

**Group fade over member fades.** The author's "most probable" real case. When a group-level
fade and a member fade are both live on the same cue, which owns the level? Is the group's
applied as a **multiplier on top** — a master fader over a channel fader — or does it replace?

> This is the question that decides more of Go.dot's Phase 3 than any other on this page,
> because "a group fade is a master fader" and "a group fade is another fade" produce different
> data structures, not just different behaviour.

*(**Settled 2026-09-06, with the Phase 3 plan — decision O**: a group fade is a **trim**, a master
fader over a channel fader. A run's level becomes `base + Σ trims`, a group run's `level` is a
trim over its members, and PR 3.12 builds it. This is what PRD §3.6 already said — "trim, not
write; nested trims compose" — and it is the structure §2 and §3 below need. The QLab check is
still worth doing for the *relative-fade* half, which is not settled.)*

### 1b. Cheap to check while you are there

- **A fade whose target ends on its own, mid-fade.** Does the fade cue report done, error, or
  sit there? *(Go.dot ends the fade's run — a fade with nothing left to fade is over.)*
- **A stop or fade aimed at a cue that already finished.** Silent no-op, or an error in the
  list? *(Go.dot: applied, not refused — PRD §3.8. Firing at a cue that ended earlier than
  expected is what an operator does, not a mistake they made.)*
- **When a fade cue reports done to its group** — at the end of its duration, or when the level
  arrives? The two differ whenever the audio side lags the control side.
- **A second GO on a cue that is already running.** The author chose *ignore* for media
  (decision B, 2026-09-05) and, with the Phase 3 plan (decision N, 2026-09-06), **per kind**:
  ignore for media and groups, *restart* for a fade or stop (it takes over from the current
  level), a *second instance* for osc, midi and memo. Still worth confirming against the field
  convention, since the answer is now three answers.
- **A fade-and-stop with a duration of zero.** Instant, or refused?

### 1c. The network cue question

**Does QLab's OSC cue offer any wait at all beyond fire-and-forget?**

The PRD's own competitor table says no — *"network cues are fire-and-forget, so non-linear
rehearsal cannot reconstruct state"* — and Go.dot's three-valued wait (§3.11) is one of the
differentiators. Worth confirming rather than remembering, because if QLab has grown one, its
shape matters.

**The author's note, 2026-09-06, and it reframes the question:**

> Fire-and-forget was mostly for sequences that would go on on their own. If we can reconstruct
> the OSC state or MIDI state this would be great, to invert an unintended GO or to go back. But
> this can also be done by waypoints and reconstructing from the intermediate cues. There is
> something to design in the UX about this.

So the real question is not "can a cue wait" but **"can the show say what the world was like
before this cue ran"**, which is §3.13's state solver seen from the network side. Two candidate
mechanisms, and they are not exclusive:

| | how it works | what it costs |
|---|---|---|
| **Read before write** | a cue that verifies also *asks first*, and the previous value is what an undo restores | one extra round trip per cue, and it only works against a target that answers |
| **Waypoints** | the show declares known-good states at intervals; anything in between is recomputed by replaying the cues from the last one | nothing at run time; the designer has to place them, and the reconstruction is only as good as the cue list's honesty |

The second is what §3.13 already promises for Go.dot's own state, so extending it outward is the
cheaper design. The first is what makes an **unintended GO** recoverable, which is §4.5's
*revert, not undo* — and it is the case that actually happens weekly.

**The UX question underneath both**, which is the one the author flagged and which nothing in
the PRD answers yet: when an operator goes back, what do they see? A list that has scrolled
backwards is a lie if the world has not moved with it. Deferred to the phase that has a UI, but
worth writing down now because it constrains what the engine must be able to answer.

---

## 2. Relative fades

**The author, 2026-09-06:** *"We should also have relative fades that only give the delta,
positive or negative. This can't apply to all parameters, but it would help."*

Go.dot has only absolute fades today: `fade/@level` is a destination in dB, never an offset.
That is the simpler half and it was the right half to build first, but the design has already
run into the gap twice — the fade-over-stop case above, and the group-fade case below.

**What has to be decided.**

- **Which parameters can take one.** A delta is meaningful on anything with a linear or
  logarithmic scale and a defined zero — level in dB, a position on an axis, a delay time. It is
  meaningless on an enum, a boolean, a file name, and arguably on anything whose range is a hard
  clamp rather than a scale. The parameter table already carries `type` and `range` per row, so
  this can be *derived* rather than declared, which is one fewer column and one fewer thing to
  get wrong. Worth checking that the derivation is actually total before committing to it.
- **How several compose.** The author's reading of QLab is that relatives compound. Summing the
  deltas is the obvious rule and it is the one a mixing desk implements; it also has the
  property that the order they arrive in does not matter, which matters more than it sounds
  because cues arrive in whatever order the operator pressed GO.
- **What an absolute does to accumulated relatives.** Resets, or becomes a new base underneath
  them. See the QLab question above.
- **What happens at the ends.** A relative fade that would take a level past +12 or below −120
  has to clamp, and a clamp that is then *un*-clamped by a later negative delta must remember
  what it clamped away or the show becomes path-dependent. This is the detail most likely to be
  got wrong, and it is the reason a desk keeps the accumulated offset separately from the
  applied one.

**Go.dot's shape for it, now decided rather than provisional:** a run's level becomes
`base + Σ trims` rather than a single number — Phase 3's PR 3.12 builds it for group fades
(decision O) — where an absolute fade moves `base` and a relative fade would move an entry in
`trims`. That is the structure `run/<id>/level` publishes, and it is also exactly what a DCA is
(below) — which is why building the group fade first costs nothing later. What a *relative fade
cue* is — which parameters admit one, how the ends clamp — is still the author's, and a PRD
amendment.

---

## 3. DCA-style control, and fader-start

**The author, 2026-09-06:** *"What I'd like is to have DCA style control over cues and fades so
they can be assigned to faders. So the user can see the fades and override them manually. Or
have the fader-start and fader-stop actions."*

This is the feature the PRD's competitor table already names — *"no fader-start"* is listed
against QLab — and it is bigger than it looks, because it is three things:

1. **A cue's level is assignable to a physical fader.** The fader shows where the level is,
   including while a fade is moving it, and touching it takes over. That is a DCA in the
   console sense, and it needs the level to be a *composition* rather than a number (see
   relative fades above) so that "what the fade is doing" and "what the operator is doing" can
   both be visible and can be separated again.
2. **Touching a fader is a named action** (§4.11), so it is logged, replayed, and undoable like
   anything else. Which means the engine needs a vocabulary for *override*: entered, held,
   released, and what happens to the fade underneath while the operator has it.
3. **Fader-start and fader-stop** — moving a fader off zero *fires* a cue, and returning it to
   zero stops it. Ancient, universally understood, and it is a trigger source, which puts it in
   the same family as §3.5's rule that **only GO moves standby**. A fader-start must therefore
   fire a cue *without* moving standby, exactly as `cue.fire` does.

**Why it matters to the current design.** The touch machinery already exists — `node.touch` /
`node.release` per origin, with pushes withheld from the touching origin, settled in PR 1.9 —
and it was built for exactly this shape of problem. What does not exist is the level
composition. Getting that right is what makes the difference between a fader that can override a
fade and one that fights it.

**Open:** whether a DCA is a *cue*, a *group*, or an object of its own. A group already organises
time, order and lifetime (§4.12); a DCA organises *level* across members that may be in different
groups. Those are different axes, and PRD §4.12's "containers describe behaviour, content
describes output; nothing inherits downward" is the sentence that has to be reconciled with it.

---

## 4. The one still to identify

The author has flagged that there is another item, **from Phase 0 or Phase 1**, that has slipped
their mind. It is not either of the two Phase 2 questions still on the board:

- **§4.2** — should the PRD record that Tracktion's own device callback takes one uncontended
  `std::shared_lock` per block, and that its node-player pool uses semaphores? The lipogram is
  *enforced* on Go.dot's code and only *measured* on Tracktion's. This changes no code either
  way; it changes what §4.2 claims. *(Namespace draft question J, still open.)*
- **Question K** — how a mount declares what it can do. **Settled 2026-09-06 in PR 2.6**, the
  way the fallback recommended: a `readback` enum (`none | oscquery`, default `none`) plus a
  `queryPort`, and a `verified` cue aimed at a target that declares neither is refused when the
  show is read rather than discovered during it.

Both are recorded in `godot-namespace-draft-0.1.md` §9. If the forgotten item turns out to be a
third, it belongs there or here depending on whether it changes the namespace.

---

## 5. The header as a preset sheet

*Raised by the author on 2026-09-06, looking at the first web client: "since this is something
that preloads and prepares OSC parameters ahead of time, the parameters of the groups could have
a preload/preset tickbox to add them in the header, and we could add cues manually too. The
preloaded or preset lines would appear in italics showing they are set from a cue from the group.
Double clicking on them would lead to the edit panel."*

**What it proposes.** A group's header is where preparation lives — Phase 3 runs it as an
ordinary cue list at entry, and Phase 4 turns it into the prepare horizon proper (PRD §3.12). The
idea is to let a member cue put a line there without anybody authoring one: a parameter carries a
*preset* mark, and ticking it means "send this ahead of the GO rather than on it". The header
then reads as two kinds of line — the ones somebody wrote, upright, and the ones a member asked
for, in italics — and a header nobody wrote is still a header worth looking at, because it shows
what the scene will have done to the desk before the operator's hand comes down.

**Why it is the right shape.** §4.12 forbids inheritance *downward*: a container may not push
values into its content. This pushes nothing. It derives a header line *upward* from a decision
taken on the member, which is the direction §3.6 already works in and the direction the run tree
already reports in. And it answers a real question about §3.3's `anticipatable` — that flag says a
parameter *may* be sent early; this says *which* ones the designer wants sent early, on the cue
where they were typed rather than in a second place.

**What has to be decided before it is built.**

- **Where the preset lives.** A mark on the member's parameter, with the header line derived at
  read time, or a real header cue written into the show? §4.10 says the document holds what
  somebody decided, so the mark is a decision and the derived line is not: writing both would put
  the same fact in two places and let them disagree. The italics are then the visible form of
  "this line is not in the file".
- **What editing a derived line does.** Refusing the edit and sending the operator to the member —
  the double-click the author describes — is consistent and is probably right. The alternative is
  that editing detaches it into a real header cue, which is a second concept the file has to carry.
- **Their order among the written ones.** A header is a sequence (Phase 3 makes it one whatever
  the group's mode says). Derived lines before the written ones is the guess; §3.12's allocator
  may have an opinion once claims exist.
- **What happens when the member goes.** Disabled, deleted, or moved out of the group — the
  derived line goes with it, which is exactly what a repair rule has to be written for, and is
  the same class of problem as the standby pointer inside a deleted subtree.

**Where it lands.** The mechanism is Phase 4's, beside prepare/commit and the allocator; the
italics and the double-click are Phase 5's. Nothing in Phase 3 forecloses it: `Header` is already
an identified element holding ordinary cues, `role` is already a derived node, and the cursor and
the scheduler already refuse to treat a header cue as a member.

---

## Where these go when they are answered

Anything that changes the parameter table or the tree goes into
`docs/godot-namespace-draft-0.1.md` §9, where the settled/open convention already lives.
Anything that changes what the product *is* — relative fades, DCAs, fader-start — is a PRD
amendment, which is the author's to make and never this repository's to write.

This file is for the space in between: the questions that are live, the measurements that have
not been taken, and the reasoning behind the answers that have.
