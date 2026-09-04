# Upstream report — duplicate node IDs from Tracktion's `hash_combine`

Everything below the rule is a **post written for the JUCE Forum, Tracktion Engine category**, kept
here verbatim so the finding does not live only in a browser tab.

**Status: posted 2026-09-04.**
<https://forum.juce.com/t/duplicate-playback-graph-node-ids-from-tracktion-hash-combine/69430>
— no replies yet.

Tracktion do not accept third-party pull requests — *"We don't accept third party GitHub pull
requests directly due to copyright restrictions but if you would like to contribute any changes
please contact us"* — so the forum is the route, and this reports the problem rather than proposing
a patch.

The finding, the measurements behind it, and the reasons a candidate fix was built and then
deliberately **not** proposed are in [cross-check-m4pro.md](cross-check-m4pro.md). The short version:
a splitmix64 mixer took the collision sweep from 24 of 63 track counts to 0 of 63 and broke nothing
in either test suite, but it makes `hash_combine`'s output depend on its two arguments only through
their sum — which collides `MidiInputDeviceNode`'s `hash (midiSourceID, targetID)` deterministically —
and `PatternGenerator`'s hash is persisted in the Edit file, so changing the mixer silently stops
pattern auto-update in previously-saved documents. Both are Tracktion's calls to make, not ours.

Record Tracktion's answer here when it comes — in particular whether they treat `hash_combine`'s
output as a compatibility surface across versions, since that is the question the report puts to
them and the one that decides what a fix can look like.

**Nobody had reported this before.** A six-angle sweep of the forum found no prior thread. The
negative rests on exact identifiers rather than fuzzy phrasing — `areNodeIDsUnique`,
`node_player_utils`, `findNodeWithID`, `ArrangerLauncherSwitchingNode`, `SampleFader`,
`0x9e3779b9`, quoted `"hash_combine"`, `splitmix`, `avalanche` all return zero topics, and no
thread in the whole Tracktion category contains both "hash" and "node". Re-run with
`before:2026-09-01` to confirm the result is not just our own post. The `nodeID` hits are all
`juce::AudioProcessorGraph::NodeID`, an unrelated type.

Two caveats on reading that silence as rarity. The clip launcher is new (TE v3, 2024) and the
category carries few launcher reports of any kind, so some of the quiet is an absence of users on
that path. And the nearest prior art is JUCE-side and about the same *shape* of defect rather than
this bug: [56390](https://forum.juce.com/t/correct-hash-generation-of-plugin-parameters/56390) and
61278 argue that JUCE's plugin-parameter ID hashes are not guaranteed unique and that a
`jassert`-only guard is inadequate — answered there with "I don't recall any reports of developers
encountering VST3 parameter ID collisions." That is precisely the dismissal a concrete colliding
value is meant to pre-empt.

## Addendum drafted for reply

Not yet posted. The filed report omits the #367 disambiguation, which is the first thing a
maintainer will reach for. Post as a reply into 69430, not as a new thread.

---

Three things I should have put in the original post.

**This is not issue #367.** That one was *"Launcher clips click on every playback-graph rebuild —
node state transfer never engages inside SlotControlNode"*, and its root cause was the opposite of
this: the child nodes were **excluded** from the flat lookup list, so `findNodeWithID` found
nothing. Here they are present but ambiguous. Your fix for it (`d760ce8c1bd`, exposing
`SlotControlNode` children as internal nodes) is in the tree I tested, and since it changes what
`getInternalNodes()` returns — which `ArrangerLauncherSwitchingNode` folds into its own ID — I
checked whether it introduced this. It did not: building against `d760ce8c1bd~1` reproduces the
collision at the same track counts. It predates that fix.

**Why the aliasing is the part that matters.** From the node graph thread, on how a rebuild carries
state:

> Persistent data (plugins, automation, playheads, time-stretchers etc.) are stored with smart
> pointers and not re-created when the graph rebuilds.

That is exactly the mechanism the duplicate ID subverts. Two same-type nodes with one ID both
resolve to the same predecessor and adopt its state by `shared_ptr`, so the sharing is not a
one-off mismatch — it is re-established on every subsequent rebuild, between two nodes that have no
dependency edge and can therefore run on different threads in the same block.

**One correction to my own post:** the `hash (7653239033668669842, track->itemID)` call is
`tracktion_ArrangerLauncherSwitchingNode.cpp:41`; line 40 is the seed constant.

For what it is worth, the duplicate-ID diagnostic inside `areNodeIDsUnique` has been in the tree
and read by users for a couple of years without anyone reporting it fire, so I do not think this is
a known-noisy assertion — it appears to be a first sighting rather than something long tolerated.

---

**Duplicate playback-graph node IDs from `tracktion::core::hash_combine`**

Reporting rather than proposing — I gather you don't take outside pull requests, and having looked at
what a fix would touch, I think the design call is genuinely yours rather than mine. Here is the
diagnosis and the evidence.

I'm an independent developer building show-control software on the Engine. One of my test programs
builds an Edit with N audio tracks, calls `prepareToPlay` and plays it. Above a certain number of
tracks it started tripping

```
jassert (node_player_utils::areNodeIDsUnique (nodeGraph->orderedNodes, true));
```

in `tracktion_NodePlayerUtilities.h:122`. I assumed a bug in my own code. I no longer think it is.

**Environment**

tracktion_engine `v3.2.0-404-gb88a6ee5191` (develop, 3.5.0), JUCE 8.0.13, macOS 15.7.9, Mac mini M4
Pro, arm64, Debug build.

**Symptom**

Two `ArrangerLauncherSwitchingNode`s on different tracks — raw `EditItemID`s 1010 and 1022 — come out
of `getNodeProperties()` with the identical `props.nodeID` 16973511083447948622.

**Mechanism**

```
seed ^= std::hash<T>()(v) + 0x9e3779b9 + (seed * 65537u) + (seed / 3u);
```

With the seed fixed this reduces to `seed ^ (h(v) + K)` — affine in the value, not a hash of it. On a
bit-flip test, about 2 of 64 output bits change per flipped bit of the *value* (ideal 32), and about
24 of 64 per flipped bit of the *seed*. So "it doesn't avalanche" would be too broad: the specific
defect is that the value argument is essentially not mixed, so small differences between values
survive instead of being destroyed.

To be clear about two things it is *not*. `std::hash<EditItemID>` being the identity
(`tracktion_EditItem.h:168`) is a co-cause, not a defect — both libstdc++ and libc++ do this for
integral types, and a combine is expected to do the mixing itself. And it isn't really a divergence
from Boost either; Boost's 32-bit-era shape run at 64-bit width clusters similarly, which is why
Boost itself grew a separate 64-bit mixer in 1.81.

The failure is difference preservation and cancellation rather than clustering as such.
`tracktion_ArrangerLauncherSwitchingNode.cpp:40` computes `hash (7653239033668669842,
track->itemID)`, and the IDs handed out to consecutive tracks are small and regularly spaced, so my
eight tracks (raw IDs 1003/1010/1013/1016/1019/1022/1025/1028) gave base hashes spanning **48**:

```
4306145609409080878, ...913, ...916, ...923, ...926, ...925, ...896, ...903
```

Those bases are all distinct — the base step alone is injective, so bases cluster but never collide.
The collision is manufactured at line 55, folding in the child `SummingNode` ID: a difference in a
value survives the next combine multiplied by roughly 65537, and can be annihilated by an opposite
difference arriving from the child chain.

That reconstructs exactly, and this part depends on the formula alone rather than on my rig.
Inverting the line-55 combine gives the child IDs required to reach the observed final —
173965249108218 for track 1010 and 173965248321758 for track 1022 — and both verify:

```
hash_combine (4306145609409080913, 173965249108218) ==
hash_combine (4306145609409080925, 173965248321758) == 16973511083447948622
```

**Why it is worse than an assertion**

`prepareToPlay` (line 88) uses that ID to carry state across a graph rebuild:
`findNodeWithID<ArrangerLauncherSwitchingNode> (*oldGraph, props.nodeID)`. With two same-type
duplicates, both new nodes resolve to the *same* old node, and the adoption at lines 90–105 is
`shared_ptr` assignment, not copy. So the two live nodes end up aliasing one `SampleFader`, one
`ActiveNoteList` and one `std::atomic<ArrangerLauncherSwitchingNode*>` — and it re-establishes at
every subsequent rebuild. The faders are guarded by a channel-count check; `arrangerActiveNoteList`,
`activeNode` and `midiSourceID` are adopted with no guard at all.

Two ALSNs on different tracks have no dependency edge, so under `LockFreeMultiThreadedNodePlayer`
they can be scheduled on different threads in the same block. From reading the source the
consequences would be two tracks accumulating note-ons into one bitmap (with `createNoteOffs`
calling `reset()`, so one track's note-offs wipe the other's held notes), a shared `activeNode` so
only the last-processed node runs the `updatePlaySlotsState()` latch, and two audio threads mutating
one non-atomic `ActiveNoteList`/`SampleFader`.

Which of two equal-ID nodes `findNodeWithID` returns is unspecified anyway — `NodeAndID::operator<`
compares the ID only and `std::sort` isn't stable — so in the field this would be rare and
non-reproducible. `findNodeWithID` is `dynamic_cast`-filtered, so a duplicate between two *different*
node types is inert; only same-type duplicates do damage.

`ArrangerLauncherSwitchingNode` is just where I caught it, not the population. The assertion covers
every node in `orderedNodes`, and the same combine feeds `SummingNode`, `ConnectedNode`,
`InsertSendNode` and `LatencyNode` (which carries a latency buffer across rebuilds by the same
route). The same unguarded adoption exists at `tracktion_LoopingMidiNode.cpp:1469`, and
shape-guarded but not identity-guarded at `tracktion_PluginNode.cpp:332`.

`tracktion_WaveNode.cpp:1727` is the counter-example — it already re-checks identity after the ID
lookup:

```cpp
if (other.editItemID != editItemID)
    return;
```

which suggests the hazard was recognised at least once.

**The sweep, with its limits**

Track counts 2..64 in my rig, one clip per track: 24 of 63 counts trapped. Caveats I'd rather state
than have you find:

- Debug-only observable. In Release the assertion is compiled out and any collisions happen silently.
- On macOS a `jassert` gives a bare trap, so "24 of 63" is really "24 of 63 trapped at an assertion";
  only the runs I instrumented were confirmed down to the colliding pair.
- `areNodeIDsUnique` fires on duplicates across all node types while `findNodeWithID` is type-filtered,
  so that is an assertion rate, not a rate of *harmful* same-type duplicates. I did not measure the
  harmful subset.
- My rig allocates IDs on a perfectly regular lattice, which puts it on a resonance between the track-ID
  and clip-ID strides. Jittering the strides in a model drops the rate to a few per cent rather than to
  zero — which I read as a worse story rather than a better one, since a rare non-reproducible field
  bug is harder to diagnose than a deterministic one.

**One thing worth knowing before changing the mixer**

I initially assumed the only cost of changing `hash_combine` was cached renders regenerating once
(`ContainerClip::getHash()` and the `WaveNode` proxy/time-stretch keys are on this path). That part
is true, and those values were never portable anyway — the hard-coded seeds such as
`7653239033668669842` are `std::hash<std::string_view>` outputs and so toolchain-specific.

But there is one that isn't a cache miss. `PatternGenerator::hashNotes (seq, 2)` is built on
`core::hash` (`tracktion_Musicality.cpp:2183`) and its result is **persisted in the Edit** as
`IDs::hash` (`:825`). `getAutoUpdate()` (`:2205`) compares stored against recomputed. Change the
mixer and they never match, so chord/arp/bass/melody clips in Edits saved by an older build would
silently stop auto-regenerating on move or resize, repairing only if the user re-triggers
`generatePattern()`. It's symmetric across a version skew in either direction. The failure direction
is at least the safe one — it errs towards "the user has hand-edited these notes" — but it looks
like it would want a `hashNotes` version 3 or a migration rather than just cache invalidation.

That is really why I'm reporting rather than patching: whether `hash_combine`'s output is a
compatibility surface across versions is a question only you can answer, and it seems to be the
actual decision here.

**What I have not established**

I have not observed the audible failures — only the duplicate ID and the code path that shares the
state; my rig never rebuilds a graph mid-playback. I have not measured how often a duplicate is
same-type. And I don't know whether this still reproduces on current develop or whether you've
already changed it.

Happy to send the sweep programs, the arithmetic that reconstructs the 1010/1022 collision, or tests
shaped to live in your own suite — a pure avalanche/injectivity test in `tracktion_core`, and an
N-track `createTestEdit` → `prepareToPlay` → expect-unique-IDs test using the `areNodeIDsUnique`
wrappers already in `tracktion_TestUtilities.h`. Take, adapt or ignore whichever part is useful.
