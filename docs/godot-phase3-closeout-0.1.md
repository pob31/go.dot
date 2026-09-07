# Phase 3 close-out

*Written 2026-09-07, at the end of PRs 3.0–3.13. Four things: what the phase amends in the PRD
(proposed, never edited here), what it deliberately left undone, what it measured that the next
phase will be built on, and what is still needed from the author.*

*This is the author-facing half. The builder-facing half — which seams Phase 4 replaces and what
they are today, the traps that cost this phase time, and where to start — is
[`handoffs/2026-09-07-phase4-handoff.md`](handoffs/2026-09-07-phase4-handoff.md).*

---

## 1. PRD amendments, proposed

**Nothing in this section has been applied.** The PRD is the author's document; `CLAUDE.md` is
§4 of it reproduced byte-for-byte and gated by `scripts/check-claude-md.py`, so an amendment
made here would either break that gate or silently rewrite the review criterion for every pull
request. Each of these is a sentence to change, with what building the thing taught.

### §3.24 — "Rate is a node, so it is automatable, fader-bindable and can carry a lane"

**This is why PR 3.10 is dropped rather than delayed**, and it is a fact about the Tracktion pin
rather than a decision.

Rate cannot change on a playing launcher clip. `AudioClipBase::setSpeedRatio` is a no-op under
`autoTempo`, which every slot clip has; the launcher's `BeatConfig` never receives a ratio; and
the 1:1 rate Go.dot gets today comes entirely from `LoopInfo::setNumBeats (seconds)` at arm,
which is on Tracktion's restart list — so changing it rebuilds the graph. A node that could only
be written between cues is not automatable, not fader-bindable, and cannot carry a lane.

What *could* be built is rate **at arm**: `setNumBeats (seconds / rate)` with the clip length
scaled to match, `TimeStretcher::Mode::disabled` for varispeed and Signalsmith for stretch. That
is a useful feature and it is not what the sentence says. Building it under this sentence would
put a row in the document that does not do what the PRD promises, which is worse than not having
the row.

> Suggested: *"Rate is a property of the cue, applied when it is armed. Live rate change needs
> a per-clip speed atomic threaded into the wave node, which this engine does not have; when it
> does, rate becomes a node like any other."*

The engine-side change is small and specific — a `std::atomic<double>` per clip read by
`WaveNodeRealTime`'s resampler — and is a reasonable thing to propose upstream or to carry in
the fork. It is the only thing standing between rate-at-arm and rate-as-a-node.

### §3.25 and devplan:123 — "Ranges map onto follow actions"

They do not, and the phase is built on the other thing.

Spike 03 established that follow actions join sample-accurately and that Tracktion's boundary
behaviour is a one-sided decay of the outgoing clip rather than a crossfade. Spike 03b (M12)
then measured the mechanism Phase 3 actually uses — a clip per range armed **looping**, with
Go.dot placing every boundary — against two alternatives, and the clip's own wrap won every one
of ten configurations by between 5.5× and 23 000× in damage energy.

More to the point, a follow action cannot express what §3.24 asks for. A range that loops **for
ever** and is left by an `advance` at the end of the pass it is on is not a follow action's
shape at all: follow actions fire at a clip's end, and an infinite range has none.

> Suggested: *"Each range is a clip in a launcher slot of its own, armed looping, and Go.dot
> places the boundary between them. Follow actions are not used."*

### §3.24 — "advance at range end"

Listed alongside `advance` without being defined, and **not built**, because the two readings
differ in a way nobody can guess:

- *leave the range at the end of the pass currently playing* — which is what `advance` already
  does, making the second phrase redundant; or
- *arm a boundary that fires automatically when the range's loop count runs out, rather than
  continuing into the next range* — a different verb, and one whose effect on a range list of
  three is ambiguous.

Go.dot builds the first meaning under the name `advance`. The second needs a sentence.

### §3.6 — "GO past the final iteration"

The author took reading A on 2026-09-06 (decision 2): the pointer leaves a manual sequence group
for the group's next sibling **the moment the last member of the last round fires**, so no GO is
ever spent on leaving. §3.6's own sentence describes the literal reading, where the pointer wraps
and one more GO exits.

> Suggested: *"The pointer leaves a manual group when its last member fires; a GO is never spent
> on leaving. An infinite manual loop is left with `afterIteration`, `advance` or `run.stop`."*

### §3.7 — trigger kinds, and whether a calendar belongs in one

Phase 3 builds `clock` as a time of day, fired once each day it is crossed while the show is
open. That covers "the foyer opens at 19:00" and does not cover "matinees only", "not on
Mondays", or a date range for a run of a show.

Not a defect — the section says time of day and that is what was built — but the moment a
second calendar rule is wanted, the trigger row grows a field that a `HH:MM:SS` string cannot
carry. Worth deciding before rather than after.

---

## 2. Deliberately not in Phase 3

Everything the plan listed under *What is deliberately not in Phase 3* stands: prepare/commit
and `armed-verified`, the allocator and processor slots, DCA trims on faders and fader-start,
bindings, relative fade *cues*, crossfaded joins, `advance at range end`, live rate, sequenced
MIDI clips, timecode and MTC, Esc / double-Esc / Go Doh!, debounce, `LISTEN` on mounts, any UI,
mDNS.

Added to that list by the phase itself:

- **PR 3.10, rate at arm.** Dropped, for the reason in §1 above. The plan marked it droppable
  and last in the audio line; this is that decision taken.
- **`group/@play` beyond the unit suite.** "Play N of M" is in the table and honoured by the
  scheduler, and no fixture or driver exercises it. Not a gap so much as a thing nobody has yet
  needed on a stage.
- **Origin checking on engine-origin commands.** Carried over from Phase 2's list and still
  true: `RunCommands.h` says anyone may send one by design, and every new engine-origin command
  this phase added follows that.
- **`MidiSender` on real hardware.** The sink seam is tested with a recording fake on every
  platform; JUCE creates virtual MIDI ports on macOS and Linux only, and no CI runner has a MIDI
  interface. Real ports are on the hardware checklist.

---

## 3. What Phase 3 measured

Seven measurements, all on the Windows box unless said otherwise. The full numbers are in the
commit messages and in namespace draft §12.13.

| | what | answer |
|---|---|---|
| **M9** | one applied mutation with WFS-DIY's 2487-node capture mounted | **3.24 ms → 0.113 ms** in Release, 16% of a tick → 0.6%. The mounted half was twenty-nine times the rest of the tree; it is a separate cache now |
| **M10** | node identities at 1..64 tracks × 1..8 slots | **no collisions anywhere**, 128 combinations, once the question was asked of `sortedNodes` rather than `orderedNodes` |
| **M11** | callback cost at 32 tracks × 8 slots × 64 outputs @ 96 kHz | **~223 µs of a 667 µs block**, against ~216 µs at one slot. Seven microseconds for seven extra slots a track |
| **M12** | three loop joins at five block sizes and two rates | **the clip's own wrap wins every configuration**, by 5.5× to 23 000× in damage energy. `setLooping` on a non-looping clip never comes back |
| **M13** | three ranges in order, and an advance, from the render | **both boundaries inside a block + 40 samples**; the advance lands at the end of the pass it was on |
| **M15** | an auto chain's member-to-member gap | **`2 + launchLatencyTicks`**, published at `/godot/engine/sequenceGapTicks` |
| **M14** | rate exactness | **not run** — PR 3.10 is dropped |

**What the measurements changed, rather than confirmed.** M9 moved the mounted namespace into a
cache of its own. M10 changed which collection the identity check inspects — the old one could
not see a launcher slot at all. M12 removed work: Go.dot places nothing inside a looping range,
so a bed looping for four hours costs no command, no placed instant and no run record.

---

## 4. Still needed from the author

- **The five amendments in §1**, each of which is a sentence.
- **The remaining QLab checks** in `godot-open-questions-0.1.md` §1 — relative fades, absolute
  over relative, the fade-and-stop click — and the relative-fade amendment they lead to. The
  structure those need now exists: a run's level is `own + Σ ancestors' own` since PR 3.12.
- **The hardware pass**, which needs the room: MIDI in and out on the Windows box with a real
  port, the same on the M4 Pro, and a loop boundary *listened to* at 128 and 256 frames — plus
  512 at 48 kHz, which M12 added to the list because it is the one cell where the wrap leaves
  anything at all (55 samples at 5% of full scale, 1.1 ms).
