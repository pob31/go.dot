# Spike 07 — The proxy-plugin sandbox as a custom TE plugin type

## Verdict

**PASS, and comfortably.** A custom Tracktion plugin type can wrap a **synchronous
cross-process round trip**, meet a hard deadline inside the audio callback, and **survive the
child process being killed mid-playback** — degrading instead of dropping out, exactly as PRD
§3.18 specifies.

The round trip costs **0.9 µs at p50** — 0.03% of a 128-frame block at 48 kHz. Zero misses in
2250 blocks with the child alive. When the child was killed at block 500, playback continued
to block 2250 with every subsequent block timing out cleanly at the deadline and passing the
dry signal through.

The design constraint this produces is not about speed. It is that **a dead child costs the
full deadline on every block, for ever**, until something stops calling it.

## Criterion

PRD §6.1 item 7, verbatim:

> 7. The proxy-plugin sandbox as a custom TE plugin type wrapping the IPC.

and PRD §3.18 fixes the shape:

> "Synchronous cross-process: shared-memory buffers, audio thread signals the plugin process
> and waits with a hard deadline. In time → zero added latency. Missed → last buffer or
> silence, strip marked failed. Degradation instead of dropout."

**Scope, per the author's decision:** real shared memory, a real second process, a real
deadline and a real kill path — but **no plugin hosting in the child**, which is Phase 9's job
and adds nothing to the proof.

## What was built

`spikes/spike07_proxy_plugin/main.cpp`.

- **`SpikeProxyPlugin : Plugin`** — a genuine custom TE plugin type, registered with one line
  (`engine.getPluginManager().createBuiltInType<SpikeProxyPlugin>()`) and inserted on a track
  like any other.
- **A child process**: the *same executable*, re-invoked as `--child=<path>` via
  `juce::ChildProcess`. It maps the same region and applies a gain.
- **Shared memory**: a `juce::MemoryMappedFile` over a temp file, carrying lock-free atomic
  sequence counters plus the audio. No new dependency — JUCE already has everything needed.

Two decisions in the design are load-bearing, and both come from other parts of the PRD:

**The wait is a bounded spin, because §4.2 forbids anything else.** *"The audio thread is a
lipogram: no allocation, no locks, no exceptions, no syscalls, no logging."* A condition
variable, a semaphore or a sleep all enter the kernel. What remains is spinning on an atomic in
shared memory, and whether that can meet a deadline is precisely what this spike measures. The
clock is consulted once every 64 iterations rather than every pass, because `steady_clock::now()`
is not free either and checking it constantly would measure the clock instead of the round trip.

**`getLatencySeconds()` returns zero, deliberately.** Spike #6 established that any latency a
plugin declares is added by TE's PDC to *every other track in the Edit*. A proxy plugin that
declared its round trip as latency would delay the whole show. The round trip is absorbed
inside the block instead, which is what §3.18's "in time → zero added latency" means.

## How it was run

```
build:   Debug, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine v3.1.0 (runtime string), JUCE v8.0.6
command: spike07_proxy_plugin --tracks=1 --sample-rate=48000 --buffer=128 \
                              --deadline-us=N [--kill-at=BLOCK]
device:  none — TE hosted audio device interface, no hardware opened
```

Block period at 128 frames / 48 kHz is **2667 µs**.

## Numbers

**Child alive**, 2250 blocks, deadline 500 µs:

| | |
|---|---|
| plugin registered and instantiated | **yes** |
| misses | **0 / 2250** |
| round trip p50 | **0.9 µs** (0.03 % of the block) |
| round trip p99 | 12.1 µs |
| round trip max | 236.5 µs |
| worst overrun beyond deadline | **0** |

**Child killed at block 500**, deadline 500 µs:

| | |
|---|---|
| blocks processed | **2250** — playback continued to the end |
| misses before the kill | **0** |
| misses after the kill | 1750 — i.e. every remaining block |
| round trip p50 after the kill | **501.1 µs** — pinned at the deadline |
| round trip max | 561.8 µs |
| overrun beyond deadline | 61.8 µs |

**Deadline tradeoff:**

| deadline | alive p50 | alive misses | dead p50 | dead cost as % of block |
|---|---|---|---|---|
| 100 µs | 0.9 µs | **2** | 101 µs | 3.8 % |
| 250 µs | 0.9 µs | 0 | 251 µs | 9.4 % |
| 500 µs | 0.9 µs | 0 | 501 µs | 18.8 % |

## What was learned

**The mechanism works, and the healthy-case cost is negligible.** Sub-microsecond for a full
cross-process round trip is what shared memory plus a lock-free spin buys: there is no kernel
transition anywhere on the audio thread's path. §3.18's architecture is sound.

**The kill path behaves exactly as specified.** The child was killed mid-playback and the
process did not stall, crash or drop out. Every subsequent block hit the deadline, took the
passthrough branch, and playback ran to completion. That is "degradation instead of dropout"
demonstrated rather than asserted.

**Passthrough was chosen over silence**, of §3.18's two options. For a show a momentarily
unprocessed strip is far better than a hole: the dry signal is already in the buffer, so it
costs nothing to leave it there.

**The finding with design consequences: the deadline is the price of failure, not of success.**
The alive cost is 0.9 µs *whatever the deadline is* — the deadline never comes into play when
the child answers. But a **dead** child costs the full deadline on **every block**:

- at 500 µs that is 18.8 % of a 128-frame block, per proxy strip;
- with five dead proxies at 500 µs the block is gone entirely;
- and it continues for ever, because nothing in the mechanism itself notices the child is dead.

So §3.18's *"strip marked failed"* is **not cosmetic**. It has to actually stop calling the
proxy, or one crashed plugin quietly consumes the audio budget of the whole show. That is a
requirement on the design, and it is the most useful thing this spike found.

**Choosing the deadline** follows from the same table: pick the smallest value that gives zero
misses while healthy, because that value is also what a failure costs. At 100 µs there were
already 2 misses with the child alive, so 100 µs is too tight here; 250 µs was clean and costs
9.4 % of a block when dead. That trade is per-machine and belongs in §3.18's stated latency
budget.

**The clock-check granularity is visible and bounded.** The worst overrun past the deadline was
61.8 µs, which is the cost of checking the clock every 64 spins rather than every one. It is a
deliberate trade — checking every pass would make the measurement mostly clock reads — and 62 µs
of slack on a 500 µs deadline is acceptable. It should be stated in any budget rather than
discovered.

**What this does not prove.** The child applies a gain, not a VST. Hosting a real plugin adds
its own scan, state and crash surface, and the round-trip cost measured here is the *transport*,
not the *processing*. What is established is that the transport and the deadline are not the
hard part.

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

- **§3.18, plugin hosting and the live rack** — the architecture is **confirmed feasible**.
  Worth adding the measured transport cost (sub-microsecond round trip) and, more importantly,
  that "strip marked failed" must *disable the call*, not merely annotate the UI, because a
  failed strip otherwise costs its full deadline every block for the rest of the show.
- **§3.18's "stated latency budget"** — now has a concrete second component: the deadline is
  simultaneously the recovery time and the per-block cost of a failure, so the budget must be
  stated as *deadline × maximum simultaneously-failed strips*.
- **§4.2, the audio-thread lipogram** — **no conflict**, which was the open risk. The bounded
  spin performs no allocation, takes no lock, throws nothing, makes no syscall and logs nothing.
  The only concession is reading a clock, once per 64 spins.
- **§3.25** — the proxy plugin declares zero latency, so it does not interact with the PDC
  problem spike #6 found. Worth stating explicitly, since the obvious implementation (declaring
  the round trip as latency) would delay the entire show.

**Audio workgroups are the missing half of this on Apple Silicon.** The deadline measured
here is met by a bounded spin on a thread the OS is free to move. Apple's audio workgroups
exist precisely for this case — a helper *process* that must meet the host's audio deadline is
the problem AUv3 out-of-process plugins have, and workgroups are Apple's answer to it. TE
already supports them (`EditPlaybackContext::enableAudioWorkgroup`, off by default) for its own
graph threads, but a proxy sandbox would additionally need the **child** process to join the
same workgroup, which is a separate piece of work and is not covered by this probe.

On Windows there is no equivalent; the nearest lever is MMCSS "Pro Audio" thread
characteristics. See `docs/spikes/README.md` for the detail.

## Open questions for the author

1. **What is the deadline, and what is the maximum number of simultaneously failed strips?**
   Together they set the worst-case audio-thread cost. 250 µs was clean on this machine; the
   number is per-machine and belongs in §3.18's budget.
2. **How is a failed strip detected and disabled?** The mechanism cannot notice on its own — it
   just keeps timing out. A miss counter with a threshold, on the message thread, is the obvious
   answer, but the policy (how many misses, whether it can recover, whether the operator is told
   mid-show) is a product decision.
3. **A pure spin burns a core while it waits.** At 0.9 µs that is irrelevant; at a 500 µs
   deadline on a dead strip it is a core spinning for 19 % of every block. Whether that is
   acceptable, or whether the spin should yield after some threshold and accept the §4.2
   violation on an already-failed strip, is a judgement about which rule matters more when
   something has already gone wrong.
