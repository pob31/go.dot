# Phase 2 close-out

*Written 2026-09-06, at the end of PRs 2.1–2.8. Three things: what the phase amends in the PRD
(proposed, never edited here), what it deliberately left undone, and what it measured that the
next phase will be built on.*

---

## 1. PRD amendments, proposed

**Nothing in this section has been applied.** The PRD is the author's document; `CLAUDE.md` is
§4 of it reproduced byte-for-byte and gated by `scripts/check-claude-md.py`, so an amendment
made here would either break that gate or silently rewrite the review criterion for every pull
request. Each of these is a sentence to change, with what building the thing taught.

### §3.11 — "Targets speak OSCQuery"

The section opens by stating as a premise something true of Go.dot's own processors and untrue
of most third-party devices. Its own parenthesis already scopes the mechanism correctly, so
this is a sentence to soften rather than a design to change.

What Phase 2 learned that bears on it: of 87 Chataigne modules surveyed, **32 declare no
feedback values at all and 11 more declare output without input**. OSCQuery was never
standardised. A mounted namespace is usually hand-written for a box that will never answer,
which is why `readback` defaults to `none` and why a `verified` cue against such a target is
refused when the show is read.

> Suggested: *"Targets that can be asked describe themselves; a cue is then an assertion with
> read-back. Most cannot, and a mount says which it is."*

### §4.2 — the lipogram, and what Tracktion does

**This is question J and it changes no code either way.** Tracktion's own device callback takes
one uncontended `std::shared_lock` per block, by design, and its node-player pool uses
semaphores. Go.dot's rule is *enforced* on Go.dot's scopes — the callback prologue, the
sub-block loop, the output plugin — and *measured* on Tracktion's, which is published at
`/godot/engine/rtForeignAllocations` and never asserted to be zero.

Two nets now hold it. The counting allocator answers "did anything call `operator new`". Clang's
real-time sanitizer answers the rest — locks, syscalls, anything that can block — and half of
that at compile time, because `nonblocking` propagates to everything a marked function calls.
Its CI job is green on its first run.

The question is whether §4.2 should say so. It currently reads as an absolute, and it is an
absolute about Go.dot's code inside a callback that is not entirely Go.dot's.

### §6.2 — the rate mismatch policy

Left open on purpose, and Phase 2 has now met it. `wfg serve --device=` **refuses** when the
device grants a rate other than the one `--sample-rate` asked for, because the tick schedule is
built before the device opens and everything downstream is arithmetic on it: a card at 44 100
against a schedule of 48 000 puts every tick, every launch instant and every fade 8.8% out,
and nothing looks wrong.

Refuse is the one of *refuse / warn / resample* that cannot be wrong quietly, so it is what the
code does until the author says otherwise. **This is not a decision that has been made** — it
is the safe default standing in for one.

The first interface anybody tried granted **480 frames for a request of 256**, which is the
same principle one level down and is why the buffer size is read back too.

### §3.25 — the resident-clip trick, if it is worth recording

Not an amendment so much as a fact the PRD does not have and the next reader will want: the
graph's *shape* is fixed at load because every launcher slot holds a **resident clip** whose
source is a generated silent WAV, and arming changes that source rather than adding a node. One
rebuild per arm, none per completion, and M4 measured that a rebuild leaves audio already
playing **bit-identical**.

---

## 2. Deliberately not in Phase 2

Carried forward, each with why it waited rather than merely that it did.

| | why it waited |
|---|---|
| **Groups, and triggers other than GO** | Phase 3. A group organises time, order and lifetime; every question about it (a group fade over its members', a member's pre-wait as an offset) needs the answer to how fades compose, which is open — see `godot-open-questions-0.1.md` |
| **Relative fades** | The author raised them on 2026-09-06. `fade/@level` is a destination in dB and never an offset. A run's level would become `base + Σ offsets`, which is the same structure a DCA needs — so the two are one feature and building either alone is building half of it twice |
| **Prepare/commit proper** | §3.12. Arming is prepare-lite: a voice reserved and a file made ready. The revocability rule, the anticipation horizon and `armed-verified` are Phase 4 |
| **`LISTEN` on mounts** | Phase 4. `verified` polls `?VALUE` because that is what WFS-DIY's own client had to do — a server sends no current value on `LISTEN` — and polling leaves no subscription state to reconstruct when the log is replayed with no network |
| **The other three read-back mechanisms** | A polled get-convention (ADM-OSC), a subscription (ETC Eos), a bespoke sync command (grandMA3). `mount/@readback` names the *mechanism* rather than a boolean precisely so each becomes another word here |
| **Rate capping on the mount sender** | `mount/@rateCap` is declared and nothing reads it. Phase 4's prepare/commit is what will, and capping before there is anything to cap would be inventing behaviour nobody has measured |
| **OSC bundles to own processors** | §3.12's timetagged tick-N+1 values. One message per datagram today; bundle support is uneven in the field and the timetag half is a different feature with a different reason |
| **`audio.deviceStarted` and the tick rebase** | The command is specified and `switchSample` is reported; nothing rebases. It matters only for a device that stops and restarts mid-show, which is M8's subject and needs the hardware |
| **M8, the mid-show rate change** | The plan says to *measure and write down, not design*. Needs the Dante |
| **Device hot-swap and `TeSession` generations** | Designed in the plan, unbuilt. Nothing swaps a playback context yet |
| **Esc / double-Esc / Go Doh!** | Phase 10. `run.kill` is the primitive they will use, and Phase 2 found it stopping nothing at all — it marked a run `stopping` and never told the audio side |
| **Origin enforcement on engine-origin commands** | The namespace draft says `audio.*` and `run.*` should be rejected from any origin but `engine`. No engine-origin command has ever checked, so adding it to one would be a rule with one member. It is a small, uniform change and it belongs in one commit of its own |

---

## 3. What Phase 2 measured

Numbers the next phase is built on, gathered rather than assumed. The full workings are in
§11.8 of the namespace draft and in the commit messages; this is the index.

| | result |
|---|---|
| **M1** routing exactness | Every output carries its coefficient exactly or is digital silence, from 1 channel into 2 up to 8 into 64. Unity is unity through the master chain |
| **M2** node-id collisions | Collision-free generation at 2–64 tracks with every slot populated |
| **M3** callback cost | 32 cues into 64 outputs at 96 kHz fits its real-time budget with room |
| **M4** arming during playback | A graph rebuild leaves audio already playing **bit-identical** — not close, identical |
| **M5** launch landing | On the sample it was placed at, across 3 rates × 5 block sizes. Never early; never more than one block late, which is the audio thread's try-lock and is one-sided |
| **M6** stop landing | The same, on the way out, across 3 rates × 3 block sizes. Never exercised by anybody before this project |
| **M7** rendered fade | Follows its curve to **1.9 × 10⁻⁶ dB**, tick by tick, over fifty ticks. Monotonic in the audio. −120 dB renders exact zeros. A fade-and-stop has no discontinuity anywhere, including at the stop |
| **rtsan** | Clang 20, `-fsanitize=realtime`, the matrix marked `nonblocking`: green on its first run |
| **The device** | Granted 48 kHz / 480 frames / 6 outputs for a request of 256 frames. The callback drives Go.dot's clock by exactly one block per interrupt |

Seven replay fixtures, each replayed record-for-record with no audio engine, no socket and no
device — including a **verified** network cue, which reproduces because a read-back is an event
rather than a return value.

---

## 4. Still needed from the author

1. **The WFS-DIY namespace capture**, mounted as a fixture, and `verified` tried against the
   real WFS-DIY by hand. This is the devplan's own *Needs from the author* item for Phase 2 and
   is the last thing between `verified` and having been used in anger.
2. **The hardware pass** — `docs/handoffs/2026-09-06-audio-hardware-checklist.md`.
3. **The three PRD amendments above**, or a decision that they are not wanted.
4. **The QLab checks** in `godot-open-questions-0.1.md`, which decide how fades compose and
   therefore a good deal of Phase 3.
