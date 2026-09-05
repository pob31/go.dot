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

## `documents/`

`canonical.xml` is the canonical form of `messy-input.xml` — same show, badly indented, with
attributes out of order and defaults written out. The canonicaliser must turn the second into
the first, byte for byte. Both hand-authored.

## `logs/skeleton.wfglog`

Hand-written, hand-checked, and older than the document: every record in it is one the engine
must reproduce exactly on replay, and none of them needs a show to exist. It is the earliest
possible test of the replay guarantee.

## `tree/cue-F7HR8TVD.json`

The OSCQuery reply for one cue, written by hand rather than generated, so it can disagree
with the code.
