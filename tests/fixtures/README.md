# Test fixtures

Committed inputs the tests read. Three of them are goldens — a file the engine must
reproduce byte for byte — and the rest are things the engine must be able to read.

Provenance matters here more than it looks. A golden produced by the code under test only
proves the code agrees with itself, so the hand-authored files below were written by a
person and checked by a person, and the one captured file says exactly where it came from.

## `bundles/minimal/`

A show bundle in the shape PRD §3.20 describes: the manifest, `show.xml`, `state.xml` and
`namespaces/`. Hand-authored. Two mounts are declared, and they are deliberately different
kinds of thing:

- **`namespaces/console.json`** — hand-written, describing a lighting console that does not
  exist. It is the counterexample: PRD §3.22 says a hand-written template and a captured one
  must be indistinguishable to the engine, and this file is how that claim gets tested. It is
  also the only fixture carrying a write-only node, an event, a `VALS` enum, a `UNIT` and a
  `GODOT` vendor key — none of which the captured file below contains.

- **`namespaces/wfs-diy.json`** — **a real capture**, not written by anyone. See below.

## The WFS-DIY capture

```
captured   2026-09-05
from       WFS-DIY, GET http://127.0.0.1:5005/wfs
session    12 mono inputs + 2 stereo inputs, 24 outputs, 9 reverbs, reverb algorithm SDN
size       2 480 nodes, 1.04 MB, nesting depth 3
```

**Why the subtree and not the root.** WFS-DIY publishes everything under a `/wfs` container
of its own. Capturing `GET /` and mounting it at prefix `/wfs` would address every node as
`/wfs/wfs/input/1/positionX`. Capturing `/wfs` gives a description whose root *is* `/wfs`,
and the addresses come out as anyone would expect.

**What it is, precisely.** It is what one machine answered at one moment. The `VALUE`s in it
are that moment's state and the mount reader drops every one of them on load, deliberately
(PRD §4.10) — they are read only to infer whether a node is a state or an event, and then
discarded. Nothing in this file is a decision anybody made.

**It is not the whole of WFS-DIY.** The capture is the OSCQuery surface, which is narrower
than the application: the `/wfs/cluster` family is absent entirely, and so is the impulse
response file path. `docs/godot-reuse-map-0.1.md` records the full comparison against
WFS-DIY's own parameter tables and what it means.

**Re-capturing.** Enable *OSC Query* in WFS-DIY's Network tab — it is off by default — then
`curl -s http://127.0.0.1:5005/wfs`. The result is CRLF and has to be converted to LF before
committing, which `.gitattributes` would otherwise do silently on the next `git add`. A
capture of a different session is a *different file*, not a correction of this one: the tree
describes the show that was open, so the channel counts above are part of what this file is.

Measured while replacing the 17-node placeholder this file superseded: the unit suite goes
from 1.2 s to 4.2 s and the whole suite from 3.2 s to 9.9 s, because every mount test case
reloads and reparses the megabyte. That is the price of testing against something real, and
it is recorded here so that the next person to wonder where the three seconds went does not
have to bisect for it.

## `bundles/triggers/` and `logs/triggers.wfglog`

Three cues fired by three things that are not a person: a lighting desk's datagram, a MIDI
note, and a time of day. None of those exists on a machine replaying a log — there is no
socket, no MIDI interface, and it is a different hour on a different day — which is exactly
why a trigger firing is a **command**. The matching happens outside the model, on whichever
thread the input arrived on, and what reaches the engine is `trigger.fire <trigger>`.

The origin says where it came from and nothing else. Nothing anywhere enforces an origin, by
design, so the trigger's identity travels as the argument where it can be checked; `udp:`,
`midi:` and `clock` are there for the person reading the log at two in the morning.

**And the standby does not move**, not once, anywhere in the file — which is the property the
whole feature rests on. Nothing in the log says so, and that is the point: no record moved it,
because no command that moves it was sent.

Two refusals close it: a trigger identifier that names nothing, and a cue's identifier, which
is the same rule seen from the other side — `trigger.fire` takes the trigger, and firing a cue
by name is `cue.fire`.

## `bundles/rounds/` and `logs/rounds.wfglog`

A shuffled ambience bed, pruned during the show and left at a boundary. The engine draws each
round with a random number generator at a moment nobody typed, so the round is **written
down** — `run.round` carries the seed the run is drawing from and the members in the order
they will play — and this replay reads it back. The generator is not consulted at all, which
is the assertion.

The seed is the document's, so the bed plays the same order at every performance, which is how
a shuffled scene gets rehearsed. Left at nought, a fresh one is drawn when the run starts and
written into the log: different every night, and every night reproduces.

It also carries the two things an operator does to a bed that is already running. A member
**pruned** out of the round in progress, which is not an edit — the cue stays in the group,
enabled, and tomorrow it is back. And **`afterIteration`**, which leaves an infinite loop at a
boundary it was going to reach anyway rather than cutting it; the file says out loud why that
costs one more round when the press lands in the same tick as the boundary.

Captured from a live session and rewritten by hand, then replayed to confirm the rewrite.

## `bundles/chain/` and `logs/auto-chain.wfglog`

One press, and a whole scene runs itself: five cues, a nested timeline group and a footer. An
automatic sequence group advances on its own — a member reports done, the scheduler notices,
the next one starts — and that noticing happens in a tick hook, which `wfg replay` does not
run. An engine that advanced its chain from inside a hook would replay a session in which the
first member played and nothing ever followed it.

Its first job is to fail if a **handler** reports. A replay re-injects every record *and*
re-runs every handler, so a handler that submitted a report of its own would produce it twice —
once from the file and once from itself — and this log would grow on replay. The hook decides,
the handler applies, and a handler never submits; this is where that is checked.

The chain's arithmetic is left visible in the tick numbers rather than described somewhere
else: a member ends at 3, the next is spawned at 4 and launched at 5. Two ticks, plus whatever
the audio side needs to place a launch, which is what `/godot/engine/sequenceGapTicks`
publishes.

Captured from a live session and then rewritten by hand — the identifiers made readable, the
origin of the press made `cli` — and replayed to confirm the rewrite. Every record was checked
against the model it claims to show.

## `bundles/descent/` and `logs/manual-descent.wfglog`

The scheduler, replayed. Every decision a manual sequence group takes is the Runner's, taken
in a tick hook, and `wfg replay` runs no hooks at all — so each of them leaves as a command,
and this pair is the proof that the whole of a descent reproduces from the records: the
pointer starts three levels down where `state.xml` left it, a header runs before any member,
three presses walk the pointer out of the inner group and then out of the outer one, and a
footer blocks the group's end.

Hand-written and hand-checked, and then checked a second way, which is worth recording because
a replay alone cannot do it. Replaying proves every record is applied and that no handler adds
one; it cannot prove the engine would have written this log in the first place. So the same
session was driven live over the socket and the two record sequences compared: nineteen
records, same order, differing only in the origin of the three presses — `cli` here, a
WebSocket address there.

And a thing this fixture cannot do, said here so nobody assumes it: it does not catch a
scheduler that spawns the same member twice. That decision is taken in a tick hook, a replay
runs no hooks, and the extra runs would therefore never appear. The unit cases in
`tests/GoTests.cpp` are what fail if that returns.

The show is two nested manual groups with a header and a footer on the outer one, plus cues
either side of it, and no audio at all. It is the bundle to reach for when something needs a
document that nests.

## `documents/`

`canonical.xml` is the canonical form of `messy-input.xml` — same show, badly indented, with
attributes out of order and defaults written out. The canonicaliser must turn the second into
the first, byte for byte. Both hand-authored.

## `logs/skeleton.wfglog`

Hand-written, hand-checked, and older than the document: every record in it is one the engine
must reproduce exactly on replay, and none of them needs a show to exist. It is the earliest
possible test of the replay guarantee.

## `logs/edit-built.wfglog`

Hand-written and hand-checked, like the skeleton, and for the same guarantee seen from the
other side. Go.dot's rule is that state transitions are events and continuous readouts are
not, so the playback graph coming into being arrives as a logged command rather than as a
variable somebody set. This fixture is that rule, replayed: seven records — the report
applied, applied again, applied for a show with no audio at all, refused for a mangled count,
refused for a mistyped argument, sent from a UDP client, and refused for missing arguments.

None of them needs a document, a device or a Tracktion engine, which is the point. A log
taken during a performance has to reproduce on a laptop with no sound card, or it is not a
log.

## `bundles/first-sound/` and `logs/first-go.wfglog`

The phase's own guarantee, as a fixture: a cue fired, played and finished. The log is
hand-written and hand-checked like the other two, and `wfg replay --bundle` re-applies every
record of it with no Tracktion, no device and no sound.

It needs the bundle because GO reads standby out of the show. It needs no media, and that is
worth knowing rather than assuming: with no audio side there is nothing to arm, so the file is
never looked for — which is why there is no WAV in `media/` and why the fixture stays valid
whether or not anybody ever puts one there.

The bundle is also a real two-cue show with two buses at different channel offsets, so it is
the one to reach for when something needs a document with audio in it.

## `bundles/fade-stop/` and `logs/fade-stop.wfglog`

What a log of a fade looks like, which is the question the other fixtures do not raise. A fade
produces fifty values a second and none of them is written down — §3.15 keeps continuous
readouts out of the log — so what is here is the GO that started it and the tick its run
finished on, and everything between the two is recomputed on replay from this log and the
bundle beside it. That is also why a fade reproduces with no audio side at all.

The show is one media cue and three cues that act on it: a two-second sCurve fade, a
three-second fade-and-stop, and a fade fired at a cue that has already finished — the no-op
that §3.8 says is applied rather than refused. Between them they cover every path `beginFade`
has.

It paid for itself before it was committed. It failed, and what it had found was a fade
takeover reporting from inside a command handler: `wfg replay` re-injects the log AND re-runs
every handler, so the record arrived twice and a perfectly deterministic session did not
reproduce. The rule that came out of it — only the tick hook reports, and a hook is not run by
a replay — is now written into `Runner::advanceFades`, and the signatures of `fireFade`,
`fireStop` and `beginFade` no longer take an `Engine` so it cannot quietly stop being true.

## `bundles/network/` and `logs/network-cue.wfglog`

The moment a mount stops being a stub, as a fixture. The show is three network cues and one
mounted lighting desk — a hand-written namespace, because most devices are: OSCQuery was never
standardised and a captured description is the exception. One cue's address is a typo somebody
made months ago, which is the only way anybody was ever going to find it.

The log covers both halves of §4.11's claim that a client reaches the model through named
commands and nothing else: a cue writing a mounted node and a client writing the same node
take the same path, produce the same record and are refused in the same words.

**A replay puts nothing on any wire, and that is the feature.** `wfg replay` is what somebody
runs at three in the morning to find out why a cue misfired, on a laptop that may still be
plugged into the show network — so the replay wiring installs the mount write and deliberately
no sender. The write has to happen (the tree must reach the state it reached live); the
datagram must not.

## `logs/verified-chain.wfglog`

The seventh fixture, against the `network` bundle, and the one that shows why a read-back had
to be a **command** rather than a return value. A verified cue asks a device what a value is,
over HTTP, on a thread of its own, to a box that is not here — none of which can be replayed.
What can is the *answer*, because §3.15 makes it an event: another machine said something, so
it enters the model as a logged command applied on the tick it arrived. A replay re-injects it
and the cue reaches the same verdict on the same tick.

It covers the three endings a verified cue has, and the two failures are different words on
purpose: `disagreed` means the device is there and is not doing what it was told; `timeout`
means nothing answered. One sends somebody to look at the device, the other at the network.

There is deliberately no record of the silence in the timeout case — a device that did not
reply has told us nothing, and a log of what did not happen would be a log of the network
rather than of the show.

## `bundles/waits/` and `logs/waits.wfglog`

The seventh fixture, and the one that says a **deadline** reproduces.

A wait is the easiest thing in this engine to get right live and wrong on replay. The obvious
way to write one is a counter in a tick hook — and `wfg replay` runs no hooks at all, because it
applies the ticks the log HAS and skips the thousands between them. A cue with a two-second
pre-wait would then fire perfectly in the theatre and sit in `waiting` for the whole of the
replayed session.

So a wait coming due is a **command**: `run.fire` at the near end, `run.done` at the far one.
Every wait in this log elapses because the log says it did, on the tick the log says, with no
clock of its own in the room.

The show is four memo cues — a pre-wait, a post-wait, both, and one that waits ten seconds and is
killed in the middle of it. `Audio tracks="0"` is a real answer rather than a placeholder: a show
of memos with no audio side is exactly what a designer writes on a train, and it is the
configuration every replay is in.

The fourth cue is the case that had no owner. A waiting run holds no voice, no job and no level,
and `run.kill` writes `stopping` over the `waiting` that said who was looking after it — so before
the Runner swept for runs nothing was going to end, it stayed `stopping` until the show closed,
and a group holding on it would have held for ever.

## `tree/cue-F7HR8TVD.json`

The OSCQuery reply for one cue, written by hand rather than generated, so it can disagree
with the code.
