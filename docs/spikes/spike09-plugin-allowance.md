# Spike 09 — what a real plugin needs of a buffer, and what was taking it away

*Not §6.1's: the probe of the plugin-allowance round (namespace draft §139, stage P0), run 2026-11-20 on the
Mac mini (macOS 15.8.3, M4 Pro, 8 performance and 4 efficiency cores), Release builds of `5ae449ab`, with
Seventh Heaven (LiquidSonics, a convolution reverb, VST3). The worker's ways of waiting were also run on the
author's Windows laptop (Core Ultra 7 255H, a Debug build).*

## Why

The author, 2026-11-20: a friend trying Go.dot on an M-Pro MacBook Pro put plugins on sound cues as inserts
and "they sounded distorted". The author's own thought: "maybe we are asking too much from the plugin chains
... with very stringent processing latency on recorded media."

The proxy adds no latency (§17.6). What it had was a *limit*: the audio thread waits for the plugin's process
to answer each block for the smaller of 250 µs and a quarter of the block, and a block answered late is
silent over a 48-sample fade (CV). That figure came from a gain's round trip (spike 07, M31), and every driver
and graph test since overrode it with 20 ms or 200 ms - so the limit that ships had never been run against a
real plugin at the buffer a Mac opens with, 512 samples.

## How

M86 (`tests/ProxyTests.cpp`, skipped; `WFG_REAL_VST3=<identifier> wfg_tests --test-case="M86*" --no-skip`).
One thread in the real-time class calls the lanes once a block period, as a device callback does - a second
of noise, a second of silence - while another polls the hosts every 10 ms as the engine does, so the worker's
wait and the eight-in-a-row rule are the shipped ones. Eight seconds a row, 512 and 128 samples at 48 kHz,
each under a limit of 20 ms (what the plugin truly takes) and under the shipped one (what is lost):

- **a** one lane; **b** two lanes on one child; **c** two lanes, one reset every half second;
- **d** the plugin's child beside 1, 3, 7 and 11 children that only spin.

The worker's four ways of waiting were a switch in the child for this spike only (`WFG_PROBE_WORKER`, never
committed; the script is kept beside the logs on the farm, `~/Documents/farm-test/plugin-allowance/`).

## What it measured

### 1. The limit is a fraction of what a reverb takes

Seventh Heaven's round trip, one lane, limit 20 ms (the median is the plugin's own work):

| buffer | block period | median round trip | shipped limit | under the shipped limit, 8 s |
|---|---|---|---|---|
| 512 | 10.7 ms | **0.6 to 0.7 ms** | 250 µs | late from the first block: failed twice, then down |
| 128 | 2.7 ms | **0.19 ms** | 250 µs | 303 of 2999 blocks late (10 %), failed twice |

A larger buffer gives the plugin more to do in the same 250 µs. At 512 the plugin needs nearly three times
its limit, so the cue is not crackling but silent: eight late in a row, *stopped answering*, a restart two
seconds on, the same again, and it stays down.

### 2. The worker goes missing on a Mac, whatever the plugin

The test gain - two microseconds of work - one lane, as shipped:

| buffer | limit | blocks over 250 µs | worst round trip |
|---|---|---|---|
| 512 | 20 ms | 50 and 57 of 749 (7 %) | 12.9 and 20 ms |
| 512 | shipped | 51 and 56 of 749 late, never more than 4 in a row | - |
| 128 | 20 ms | 149 and 129 of 2999 (5 %) | 13 and 20 ms |

Only the first request after the gap between two blocks is late: a second lane asked straight after the first
(row **b**) never was. The child's worker is not on a processor when the block arrives, for milliseconds at
a time. One buffer in fifteen muted with a fade either side is the "distorted" of the report, and it needs no
heavy plugin.

The same rows with the worker waiting four ways (gain, one lane; over 250 µs / worst, of 749 at 512 and 2999
at 128):

| the worker | 512 | 128 |
|---|---|---|
| as shipped: real-time class, spins without pause while a lane is in | 58 / 13.5 ms | 160 / 20 ms |
| the same spin, **no real-time class** | 0 / 10 µs | 0 / 7 µs |
| real-time class, **naps** 100 µs between requests | 0 / 105 µs | 0 / 106 µs |
| real-time class, sleeps to 1 ms before the next block is due | 0 / 24 µs | 0 / 246 µs |

So it is the two together: macOS takes the processor from a thread in the real-time class that never gives it
back. (Phase 9a's open note said as much in September - *"macOS demotes an RT thread that overruns its
constraint"* - and the macOS-only flakes of the fx drivers since are probably this.) A workgroup makes no
difference either way.

**On Windows the spin is not punished**: the same rows on the laptop, as shipped, 0 late of 562 and of 2250,
worst 239 µs.

### 3. A reset is milliseconds, and it ran inside the limit

| Seventh Heaven's `reset()` | median | worst |
|---|---|---|
| at 512 | 12.6 ms | 18 ms |
| at 128 | 2.1 ms | 3.9 ms |

Every switch-in asks for one (2026-09-28: an insert switched in starts from silence), so every cue's first
blocks through a heavy plugin were late - and at a small buffer, eight of them: the plugin failed as its cue
began. Spike 08's "first block after idle, 2.5 to 15 ms" was this, not a wake.

### 4. Spinning children drown the machine

Seventh Heaven's median round trip at 512, limit 20 ms, beside children that do nothing but spin:

| children in all | 1 | 2 | 4 | 8 | 12 |
|---|---|---|---|---|---|
| median | 0.71 ms | 0.72 ms | 0.88 ms | **4.9 ms** | **8.8 ms** |
| blocks over 3.2 ms, of 749 | 38 | 41 | 67 | 436 | 516 |

Eight performance cores hold four spinning workers and little more. A child's worker spins for as long as
any of its lanes is switched in, and nothing switched a lane out when its cue ended (§133.8, owed), so a
session that had played a few plugins had that many cores gone for good - which is the friend's Cmd+Tab
full of Go.dots (§138) seen from the other side.

## Verdict

Not latency. Four faults, each enough alone:

1. the limit was a gain's, not a plugin's, and did not grow with the buffer;
2. on a Mac the worker's spin got it thrown off the processor one block in fifteen;
3. a reset ran inside the limit, and stalled the child's other lanes while it ran;
4. every plugin that had ever played kept a core.

And one found reading for it: after a late block the parent wrote the next one over audio the child was
still answering into, so the plugin could be handed its own output with new input on top.

## And after

The same case on the same Mac with §139's stages applied (the request and the reset, the allowance, the
napping worker), the rule as it now ships; Seventh Heaven's own lane in every row:

| | before | after |
|---|---|---|
| 512: blocks late of 749 | every one, failed twice, down | **0**, and none starved |
| 512: round trip, median / p99.9 / worst | 0.6-0.7 ms / 20 ms / 20 ms | 0.17 ms / 0.27 ms / 0.27 ms |
| 128: blocks late of 2999 | 303, failed twice | **0** |
| 128: round trip, median / worst | 0.19 ms / 20 ms | 0.08 ms / 0.18 ms |
| the gain alone, 512: over 250 µs / worst | 50-57 / 13-20 ms | 0 / 0.10 ms |
| 512 beside 1 / 3 / 7 / 11 other children, median | 0.72 / 0.88 / 4.9 / 8.8 ms | 0.18 / 0.16 / 0.18 / 0.17 ms |
| a reset beside a sounding lane | the sounding lane late for it | 0 late; the reset made within a block |

The plugin's own work is some four times quicker, which was not the aim: its thread is no longer demoted in
the middle of it, and its arithmetic now runs with denormals off.

**Windows naps badly.** With the worker napping on a high-resolution timer the laptop's round trip went from
13 µs to **1.0 ms**, 1.6 at worst (the timer's half millisecond is a whole one by the time a thread runs
again) - inside the allowance, and a millisecond of the audio thread's time a callback for nothing. Windows
keeps the spin.

**Linux naps as the Mac does.** The NUC (Ubuntu 24.04, 16 cores, a Release build), the gain alone with the
napping worker: no block late at 512 or 128, 0.10 ms at worst but for one of 0.17. The spin was not run there.

## What it changed

Namespace draft §139. The plugin's time comes from the buffer (BHB: a lane may wait three tenths of the block
period, every wait in a callback ends six tenths of the period after the callback began); a request is never
written over and a reset is made apart from the answering, on the child's message thread with its lane
parked (BHC); the worker naps a tenth of a millisecond between requests where it spun, except on Windows
(BHG); a late block is said (BHE); and a lane whose cue is over rests (BHF).

## Addendum, 2026-11-23: Windows, and where a late buffer's time went (M89)

*Namespace draft §148. The author's Windows laptop (Core Ultra 7 255H: six performance cores, eight efficiency,
two low-power; Windows 11, the Balanced plan), Release builds of `80dc5bdb` with §148 on top, no interface: M86's
rig, extended. Likyū (the author's dynamic EQ, VST3, in the state of his session, keyed) and Unfiltered Audio
Tails (a reverb, VST3, as it loads).*

**Why.** His session logged Likyū late for 135 buffers in forty minutes and Tails for 46 in one, at 48 kHz and
128 frames. "Windows does NOT punish the spin" above rested on eight seconds of a gain on a quiet machine.

**What was added to measure with.** The parent stamps each request with the steady clock; the child notes how
long the request waited for its worker and how long the worker then took, and the lane keeps the longest of
each (`region::noteTimes`). M86 gained a key, busy ordinary threads beside the row, a second real plugin after
the first, and the worker's class by `WFG_PROXY_WORKER`. `wait` and `work` below are those longest times.

**Tails takes the time itself.** Alone, the limit wide (20 ms), twenty or thirty seconds a row:

| frames | median | p99 | p99.9 | worst | over 0.3 of the period |
|---|---|---|---|---|---|
| 512 | 250 µs | 822 µs | 1.72 ms | 2.08 ms | 0 of 1874 |
| 128 | 68 µs | 341 µs | 1.46 ms | 1.69 ms | 9 of 7499 |
| 128, mono | 60 µs | 288 µs | 1.44 ms | 1.76 ms | |
| 128, again | 71 µs | 333 µs | 1.43 ms | 3.57 ms | |

One burst of 1.4 to 1.8 ms at each onset of the rig's sound (a second of noise, a second of silence), the same
at either buffer. Under the shipped limit at 128 frames (0.8 ms): 10 late in 20 s, the session's rate; at 512
(3.2 ms): none. After Likyū on one channel, a minute: 30 late with the worker in the multimedia class, 31
time-critical. No class mends a plugin that needs more than a lane is allowed.

**Likyū is kept waiting, by the class its worker was in.** Its own work: 10 µs median, 0.2 ms at p99.9. Late
buffers at 128 frames under the shipped limit, and the longest wait for the worker:

| | multimedia class (as shipped) | time-critical | time-critical, away from the first processor |
|---|---|---|---|
| beside 16 busy ordinary threads, 1 min (22499) | **2313**, wait 2.19 ms | **0**, wait 0.04 ms | |
| beside 6, 1 min | 1 (work 1.04 ms) | 0 | |
| beside 3, 2 min (44999) | 18, wait 1.24 ms | 1, wait 1.03 ms | |
| alone, 5 min (112499), three times | 0, 0, 26 | 2, 0, 9 | |
| two at one moment, 10 min (224999) | 8, work 2.03 ms | 2, wait 0.80 ms | |
| three at one moment, 10 min | 43, wait 2.02 ms | 4, wait 1.36 ms | 4, wait 0.83 ms |

The two milliseconds are the multimedia class's own: a thread in it that uses its whole share of a processor is
dropped to priorities 1 to 7 for the rest of each ten milliseconds, and "can continue running only if no
low-priority threads are ready to run" (Microsoft's words). A worker that never sleeps always uses its whole
share. Its worst buffers in that class were on efficiency cores, where the system puts what it has demoted. The
rows "alone" are a machine with a person at it and do not compare run apart - the third of each coincided with
files being copied to the farm - so the last two were run at one moment.

**What is left under time-critical** is a wait of 0.8 to 1.4 ms a few times in ten minutes, and 13.5 ms once:
interrupts, on a laptop its owner does not trust for them. The first three were on the machine's first
processor, so a wish for another performance core was tried (the thread's ideal processor, not a pin): late as
often, 4 against 4. Not adopted.

**Verdict.** On Windows the worker takes `TIME_CRITICAL` and no multimedia class (BJR). A spell of late buffers
ends with a note in the session's log that says whether the worker was kept waiting or the plugin took the time
(BJP). A plugin like Tails needs a larger buffer of its own, at the cost of that much delay: asked for by the
author the same evening, and the next round's.

**Not run.** The Balanced plan against Best performance; a second Windows machine; an interface. On the farm the
change itself was built and tested (macOS, GCC 15 strict, Clang 21 with the real-time sanitizer: no report).

## Addendum, 2026-11-24: a plugin played later, and how it is measured (M90)

*Namespace draft §149. The same laptop and plugins as M89, Release builds of `cea84b63` and what followed. M86 as
a show gives its plugins: a queue on the lane (`WFG_M86_QUEUE`), and `WFG_M86_LATE` to start a lane as late as a
build would from a measure.*

**Played later.** 128 frames, the shipped limit, a minute each unless said:

| | before §149 | a queue, more at once (LB.2) | and the wait to the callback's end (LB.3) |
|---|---|---|---|
| Tails alone: silent buffers | 30 a minute, as long as it plays | 2 in all, then none | 2 in all, then none |
| where it ends | - | 256 samples later | 256 samples later |
| Likyū then Tails on one channel: Tails / Likyū | 30 a minute / 1 | 2 in all / 1 | 2 in all / 0 |
| Likyū alone | 2 min: 2 (waits of 0.8 ms) | 2 min: 2, 256 later | 5 min: 2 (one stall of 1.65 ms), 256 later |
| the audio thread's time in Tails' lane, median | 68 µs, waiting | 1.4 µs | 1.5 µs |

A lane played later costs the callback a copy and nothing else. What moves Likyū is the laptop, and the lane gives
it back at the cue's next arm or its rest.

**How Tails has to be measured.** The longest block of 128 at 48 kHz, three runs each:

| | Tails | Likyū |
|---|---|---|
| alone in a child, as fast as it goes, the lesser of two passes | 0.09 / 0.20 / 0.38 ms | 6 / 68 / 13 µs |
| alone, a block a period, an ordinary thread, the worst of one pass | 1.4 / 2.6 / 3.0 ms | 0.24 / 0.33 / 0.31 ms |
| alone, a block a period, a worker's class, the lesser of two onsets | 0.26 / 0.33 / 0.26 ms | 0.15 / 0.15 / 0.16 ms |
| through a hosting child, as a show plays it, the median of three quarters | **1.38 / 1.39 / 1.37 ms** | **34 / 24 / 31 µs** |

The second row is the measuring thread being held up, not the plugin; the third is the plugin with nothing to
hold it up. Only the last sees what M89 saw. The difference between the third and the last is the hosting child's
message loop, which a plugin's own timers run on. So the scan measures through the real path, eight seconds a
plugin and four at a time: 3 min 48 s for the author's 67 plugins, 63 measured, once.

**Started where its measure says** (two minutes at 128 frames): Tails one buffer later from the start, one silent
buffer in all - a burst of 2.5 ms, which that spell then makes its measure; started as before, two.

**Verdict.** A plugin that cannot answer inside the buffer is played later, more at once when that is not enough,
eight buffers at most, and given back when nothing sounds through it (§149). Its measure is taken as it is
played, at the scan, and raised by what a show shows.
