# Spike 05 — External parameter control at 50 Hz

## Verdict

**PASS, with a lot of headroom.** Driving **512** plugin parameters at 50 Hz costs **2.6 ms
of a 20 ms tick** in a Release build — about 13% of the budget — and the message thread's
timer lateness is *indistinguishable from its idle floor*. Extrapolating the measured
per-write cost, the tick saturates somewhere near **4000 parameters**, which is far beyond
anything the PRD contemplates.

The one thing to carry forward is not a limit but a constraint: `setParameter` **must** run on
the message thread. That is not a preference, it is asserted in Tracktion, so Go.dot's control
graph has no design in which plugin parameters are written from the tick thread directly.

## Criterion

PRD §6.1 item 5, verbatim:

> 5. External parameter control at 50 Hz without disturbing the message thread.

## What was built

`spikes/spike05_param_50hz/main.cpp`.

Spikes 01–04 measure audio; this one measures a **thread**, so the rig has to contain a real
message thread that can be disturbed and something honest that measures the disturbance:

| | |
|---|---|
| **main thread** | `juce::MessageManager::runDispatchLoop()` — a genuine JUCE message loop, not a stand-in |
| **a `juce::Timer` at 20 ms** | PRD §3.4's 50 Hz tick. It writes every parameter *and measures its own lateness* |
| **a worker thread** | drives the audio graph, paced to real time |

**Timer lateness is the right instrument** because it is what "disturbing the message thread"
means to a user: a callback that should arrive every 20 ms and arrives late is a UI that has
stopped responding and queued work backing up.

The timer runs **on** the message thread — which is where Go.dot's own tick would do this work
— so no `MessageManagerLock` is taken. That is deliberate: locking from a worker would measure
lock contention, a different question from the one §6.1 asks.

Two details that stop the rig flattering itself:

- **The written value changes every tick.** Tracktion short-circuits a write whose value is
  unchanged, so writing a constant would measure the early-out path and report that 50 Hz is
  free.
- **Audio is actually playing.** Measuring the message thread while the engine is idle would
  answer an easier question than the one asked.

## How it was run

```
build:   Debug AND Release, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine develop 3.5.0 (runtime string still reports v3.1.0), JUCE v8.0.13
command: spike05_param_50hz --tracks=T --sample-rate=48000 --buffer=128 --params=P --seconds=3
device:  none — TE hosted audio device interface, no hardware opened
```

Parameters come from each track's `VolumeAndPanPlugin` (volume and pan), so *P* parameters
needs roughly *P*/2 tracks.

## Numbers

**Release** — these are the numbers to quote:

| params | tick cost | % of 20 ms tick | lateness p50 | lateness p99 | xruns |
|---|---|---|---|---|---|
| 8   | 0.052 ms | 0.3 % | 0.76 ms | 2.60 ms | 0 |
| 64  | 0.285 ms | 1.4 % | 0.74 ms | 2.62 ms | 0 |
| 256 | 1.018 ms | 5.1 % | 0.89 ms | 2.85 ms | 0 |
| 512 | 2.588 ms | 12.9 % | 1.16 ms | 3.40 ms | 0 |

**Debug** — recorded only to show how misleading it would be:

| params | tick cost | % of 20 ms tick | lateness p50 | lateness p99 |
|---|---|---|---|---|
| 8   | 0.36 ms  | 1.8 % | 0.79 ms | 2.70 ms |
| 256 | 12.2 ms  | 61 %  | 1.18 ms | 3.53 ms |
| 384 | 22.6 ms  | **113 %** | 7.65 ms | 20.5 ms |
| 512 | 30.7 ms  | **153 %** | 18.7 ms | 34.2 ms |

Per-write cost: **≈4 µs in Release**, **≈45 µs in Debug** — a factor of about ten.

Notification mode, Release:

| params | `dontSendNotification` | `sendNotificationSync` |
|---|---|---|
| 64  | 3.65 µs/param | 3.76 µs/param |
| 256 | 3.97 µs/param | 3.95 µs/param |

## What was learned

**50 Hz is not a problem, and the margin is large.** At 512 parameters the tick uses an eighth
of its budget. PRD §3.4's "curves are evaluated on the tick thread" is safe against any
parameter count a show is likely to have.

**The lateness floor is the timer, not us — and it is not Windows-specific.** At 8 parameters
the writes take 52 µs — 0.3% of the tick — yet lateness is still 0.76 ms at p50 and 2.60 ms at
p99. This report originally called that floor "Windows"; the macOS run below shows the same
floor (0.61 ms p50, 3.03 ms p99 at 8 parameters), so it is `juce::Timer` granularity in
general rather than anything about the platform. It is present when the spike is doing
essentially nothing. Reading
those p99 figures as a cost of parameter writing would be wrong: **the number that scales with
*P* is the tick cost, and lateness barely moves until the tick cost approaches the budget.**
Between 8 and 512 parameters — a 64-fold increase in work — p99 lateness moves from 2.60 ms to
3.40 ms.

**What the cost actually is: the ValueTree write.** `sendNotificationSync` and
`dontSendNotification` cost the same to within noise, at both 64 and 256 parameters. So the
listener fan-out is *not* the expense, and rate-limiting notifications would buy nothing. The
~4 µs is the write path itself.

**Debug numbers would have produced the wrong conclusion entirely.** In Debug the tick
saturates at about **340** parameters and is 153% over budget at 512, with p50 lateness of
18.7 ms — a message thread that is visibly broken. The same code in Release uses 13% of the
tick at that count. Had this spike been run only in the configuration CI uses, it would have
reported a hard limit an order of magnitude below the real one.

**The constraint that survives, and it is a design constraint rather than a performance one.**
`tracktion_AutomatableParameter.cpp:1073` is

```cpp
jassert (juce::MessageManager::getInstance()->currentThreadHasLockedMessageManager());
```

unconditional in the branch a non-curve-following write takes. So plugin parameters can only
be written from the message thread (or under its lock). Go.dot's 50 Hz tick derives from sample
time on its own thread (§3.4), so there is a **handover** between that thread and the message
thread on every tick that writes a plugin parameter. This spike shows the handover is
affordable; it does not remove the need for it.

**A note on what "xruns" counts here.** Warm-up blocks are excluded, the same way spike #4
settles before marking its baseline: the first blocks after graph allocation are cold and
routinely exceed budget once, which is not what a spike about *sustained* 50 Hz load is asking.
Both figures are reported — `xruns` and `xruns_including_warmup` — so the exclusion is visible
rather than flattering.

## The machine these numbers came from

Every measurement in this report was taken on:

```
Lenovo 21Q8CTO1WW laptop
Intel Core Ultra 7 255H - 16 cores, HYBRID (performance + efficiency cores)
31.5 GB RAM, Windows 11
Background load at time of measurement: ~18%, with Firefox and two VS Code windows resident
```

**This matters for anything that depends on sustained throughput or timing reproducibility.**
A hybrid-core mobile CPU migrates threads between performance and efficiency cores under
scheduler and thermal pressure, and a measurement thread moved to an E-core mid-run produces
exactly the kind of *intermittent* non-reproducibility seen here. It also explains why
real-time pacing did not help: pacing controls when work is submitted, not which core runs it.

So the throughput and reproducibility limits below are **properties of this machine under this
load**, not established properties of Tracktion Engine. They are recorded because they are what
was measured, and flagged because they are the findings most likely to move on different
hardware. Sample-accuracy, routing correctness and API behaviour do not depend on any of this
and are not affected.

A cross-check on a Mac mini M4 Pro - desktop thermals, different scheduler, and the macOS
platform that CI currently only *builds* on - is the cheapest way to separate the two.

## Consequences for the PRD

- **§3.4, the two clocks** — worth stating the handover explicitly. The 50 Hz tick runs on its
  own thread derived from sample time, but any tick that writes a *plugin* parameter must
  cross to the message thread to do it. That is a Tracktion requirement, not a choice.
- **§3.21, the control-rate dataflow graph** — measurement supports the design: a tick's worth
  of parameter writes is microseconds, so the graph can drive plugin parameters directly
  without a coalescing layer. No rate limiting is needed for cost reasons.
- **§3.10, bindings and automation** — no amendment. The cost is not in notification, so
  bindings may notify freely.

## Open questions for the author

1. **How many plugin parameters does a real show drive at once?** Nothing in the PRD states a
   figure, and this spike deliberately does not invent one — it reports the curve instead. The
   answer only matters if it is in the thousands, which seems unlikely.
2. **Where does the tick-thread → message-thread handover live?** §3.4 puts the tick on its own
   thread; Tracktion requires the message thread for plugin parameter writes. Whether that is a
   queue drained by the message thread, or the tick taking `MessageManagerLock`, is a design
   decision this spike informs but does not make. The lock version was deliberately not
   measured here, because it answers a different question — see "What was built".
3. ~~**Does the same margin hold on macOS and Linux?**~~ **macOS: answered, yes.** See the
   section below. Linux is still unmeasured.

## macOS — Mac mini M4 Pro, Release

These are the first macOS figures this spike has produced. Until the fix recorded in
[cross-check-m4pro.md](cross-check-m4pro.md), it returned zeros on macOS and reported PASS
anyway: its tick was driven by `runDispatchLoop()`, which is `[NSApp run]`, and a console
binary has no `NSApp`. 48 kHz, buffer 128, `--seconds=3`, one run each.

| P | tick cost | % of 20 ms | lateness p50 | lateness p99 | µs / param | xruns |
|---|---|---|---|---|---|---|
| 8 | 0.079 ms | 0.4% | 0.61 ms | 3.03 ms | 9.93 | 0 |
| 64 | 0.468 ms | 2.3% | 0.82 ms | 2.85 ms | 7.30 | 0 |
| 256 | 1.590 ms | 8.0% | 1.34 ms | 3.10 ms | 6.21 | 0 |
| 512 | 2.901 ms | 14.5% | 1.32 ms | 2.92 ms | 5.67 | 0 |

**The headline holds.** 512 parameters cost 14.5% of a 20 ms tick on macOS against 12.9% on
Windows — the same conclusion with slightly less margin. Per-write settles at ~5.7 µs against
Windows' ~4 µs, the same order. Extrapolated saturation is ~3500 parameters.

**The lateness floor is the same on both platforms**, which is what retires the "floor is
Windows" reading above: p50 sits between 0.6 and 1.3 ms and p99 between 2.85 and 3.10 ms
across a 64-fold increase in work, exactly as on Windows.

**Where macOS does fail: tight buffers.** Across the full grid, this spike fails at buffer 32
and 64 — and at 96 kHz more broadly — on the `xruns == 0` invariant, usually by a single
block. That is the expected consequence of asking for a 0.67 ms buffer while a 50 Hz writer
runs, and it is the same region where spike #4 records this machine straining. It is a
statement about those buffer sizes, not about parameter control.
