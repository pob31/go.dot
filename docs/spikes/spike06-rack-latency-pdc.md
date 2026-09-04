# Spike 06 — Live-input latency through a Rack, and PDC

## Verdict

**PASS.** PRD §3.25's warning about PDC is **confirmed as a default and retired as a problem.**

Both halves matter and neither is the whole answer:

**PDC is global and on by default.** A plugin with 250 ms of latency on **one** track delays
**every other track's file playback by exactly 250 ms**. The shift tracks the plugin's latency
precisely — 50 ms gives 50 ms, 100 ms gives 100 ms — and a Rack behaves identically to a bare
plugin. That is correct DAW behaviour and wrong show behaviour: it would put a quarter of a
second between the GO button and every cue in the system.

**One public call removes it completely:**

```cpp
edit.setLatencyCompensationEnabled (false);   // tracktion_Edit.h:542-543
```

Measured across 50/100/250 ms of latency, 48 and 96 kHz, four buffer sizes, bare and inside a
Rack — **the file-path shift was exactly zero in every configuration**. The latency track stays
late by exactly its own plugin's latency, which is that plugin doing its job; nothing else is
dragged along. The Edit still *reports* the latency, so Go.dot can know the number and place an
offset wherever it decides one belongs.

One lever that does **not** work, recorded because its name misleads:
**`Edit::setLowLatencyMonitoring`** leaves the shift unchanged. It is the wrong tool for this.

## Criterion

PRD §6.1 item 6, verbatim:

> 6. Live-input latency through a Rack, with TE's PDC behaviour on live tracks understood (it
>    may insert alignment delay exactly where none is wanted).

## What was built

`spikes/spike06_rack_latency_pdc/main.cpp`.

Two tracks, each playing the **same** transient file from the timeline, each routed to its own
stereo bus. Track F is a witness — nothing is ever done to it. Track L receives a
`LatencyPlugin` of known latency, optionally wrapped in a `RackInstance`.

**The measured quantity is deliberately not the latency of the plugin.** A plugin with latency
having latency is correct and uninteresting. What matters is the position of a *different*
track's audio:

```
file_path_shift_samples = position(track F, with latency on track L)
                        - position(track F, no latency anywhere)
```

Zero would mean TE leaves the file path alone and §3.25's warning could be retired.

## How it was run

```
build:   Debug, MSVC 19.51.36256 (VS 2026), Windows 11
engine:  Tracktion Engine develop 3.5.0 (runtime string still reports v3.1.0), JUCE v8.0.13
command: spike06_rack_latency_pdc --tracks=2 --sample-rate=48000 --buffer=128 --latency-ms=N [--use-rack]
device:  none — TE hosted audio device interface, no hardware opened
```

## Numbers

| plugin latency | file-path shift | reported graph latency | wrapped in a Rack? |
|---|---|---|---|
| none (reference) | — | 0 samples | — |
| 50 ms | **50 ms** | — | no |
| 100 ms | **100 ms** | — | no |
| 250 ms | **250 ms** (12000 samples) | 12000 samples | no |
| 250 ms | **250 ms** (12000 samples) | 12000 samples | **yes** |

With `Edit::setLowLatencyMonitoring(true, …)` enabled and the latency plugin in the bypass list:

| | file-path shift | reported latency |
|---|---|---|
| low-latency monitoring **on** | **250 ms, unchanged** | 12000 samples |

Track alignment relative to each other: `latency_track_delay_vs_file_samples = 0`. The two
tracks stay perfectly aligned — which is PDC working exactly as designed. The problem is that
"aligned" is achieved by making *everything* late.

**With `Edit::setLatencyCompensationEnabled(false)` — the result that changes the verdict.**
Five configurations, chosen to vary everything that could plausibly matter:

| rate | buffer | plugin latency | Rack? | default shift | **shift with PDC off** | latency track vs file |
|---|---|---|---|---|---|---|
| 48 kHz | 128 | 50 ms | no | 2400 | **0** | 2400 |
| 48 kHz | 128 | 100 ms | no | 4800 | **0** | 4800 |
| 48 kHz | 128 | 250 ms | **yes** | 12000 | **0** | 12000 |
| 96 kHz | 256 | 250 ms | no | 24000 | **0** | 24000 |
| 96 kHz | 64 | 100 ms | **yes** | 9600 | **0** | 9600 |

All figures in samples. Three things to read out of the last column: the file path is *exactly*
undisturbed rather than approximately so; the latency track is late by *exactly* its own
plugin's latency, so disabling compensation removes only the compensation and not the plugin;
and `nopdc.reported_latency_samples` still reports 12000, so the number remains available to
Go.dot even with compensation off.

Mono source into the stereo bus: `bus_peak_left = 0.5`, `bus_peak_right = 0`.

## What was learned

**The compensation is switchable, and the switch is public.** `Edit` carries it directly:

```cpp
/** Can be used to disable latency compensation when playing (it is enabled by default) */
void setLatencyCompensationEnabled (bool enabled);        // tracktion_Edit.h:542-543
bool isLatencyCompensationEnabled() const noexcept;
```

It is wired the whole way down. `Edit::setLatencyCompensationEnabled` (`tracktion_Edit.cpp:2416`)
stores the flag and calls `restartPlayback()`; `EditPlaybackContext`'s `setNode`
(`tracktion_EditPlaybackContext.cpp:205`) re-reads it on **every** graph build and forwards it to
`LockFreeMultiThreadedNodePlayer::setLatencyCompensationEnabled`, which reaches `SummingNode`,
`ConnectedNode` and `RackReturnNode`. Because it is re-read per build, setting it before the
player exists is enough — this spike does that rather than depending on `restartPlayback()`
under a hosted device interface.

**Re-measured on Tracktion develop (3.5.0): the default behaviour is unchanged.** The file-path
shift is still exactly 250 ms for a 250 ms plugin and `setLowLatencyMonitoring` still does not
help. 404 commits of engine change did not touch this, which makes the *default* a design
property rather than a passing defect.

**PDC delays the whole graph by the worst plugin latency, and there is no per-track escape.**
The compensation is global by construction: `SummingNode` and `ConnectedNode` insert latency
nodes so that every branch arrives together. That is the right answer for a mixdown and the
wrong one for a show, where the file path must not wait for the live path.

**A Rack changes nothing.** `--use-rack` produced identical numbers to the bare plugin, so the
rack wrapper neither adds nor avoids the problem — it is the plugin's declared latency that
propagates, and `RackInstance::getLatencySeconds()` reports it upward like any other plugin.

**`setLowLatencyMonitoring` did not help.** This is the only Edit-level lever TE offers and its
name suggests it addresses exactly this. It does not: the file-path shift was unchanged at
250 ms. Its implementation (`tracktion_Edit.cpp:2301-2330`) shrinks the *device buffer* and
**bypasses** the listed plugins, so even where it works its mechanism is "remove the plugin's
latency by removing the plugin" — which is incompatible with a live rack whose whole purpose is
to be in circuit. **Caveat:** the buffer-shrinking half cannot be exercised through the hosted
device interface, so this particular measurement may understate what it does on real hardware.
The bypass half is not in doubt.

**How the flag actually reaches the graph.** The `disableLatencyCompensation` parameter is
threaded through `NodePlayerUtilities::prepareToPlay`, `transformNodes` and
`Node::TransformOptions`, and is read in `SummingNode`, `ConnectedNode` and `RackReturnNode`. It
does **not** arrive as an argument to `setNode` — neither public overload takes it, and the
`prepareToPlay` that does is private. It arrives as **state**: the player holds
`std::atomic<bool> disableLatencyComp`, set by
`LockFreeMultiThreadedNodePlayer::setLatencyCompensationEnabled`, which
`EditPlaybackContext::setNode` calls on every graph build from
`edit.isLatencyCompensationEnabled()`.

**Mono into a wider destination now spreads — this changed between Tracktion versions.**
A mono source on a track routed to a *stereo* bus produced `left = 0.5, right = 0` on
v3.2.0 and produces `left = 0.5, right = 0.5` on develop (3.5.0). Mono is now canonically
upmixed into a wider destination rather than left in channel 1.

That is a better default for most hosts and a slightly awkward one for §3.9b's *"Width is
explicit, never automatic"*, which is now no longer true of the engine. A **mono destination**
still stays mono — spike #1 measures that at up to 64 mono direct outs — so the spatial
workflow is unaffected. It is only mono-into-wider that spreads, and Go.dot must declare
destination widths explicitly if it wants to control that.

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

- **§3.25, "Plugins and the rack are where TE earns its keep"** — the warning becomes a
  **configuration note**. PDC is global: a plugin's declared latency delays every other track in
  the Edit by the same amount, Rack or not. `Edit::setLowLatencyMonitoring` does not avoid it.
  `Edit::setLatencyCompensationEnabled(false)` does, completely, and is public and documented.
  Go.dot should call it at Edit construction and treat alignment as its own responsibility —
  which §3.25 already says it should, since Go.dot owns time.
- **§4.1, "The GO path is small, boring and merciless. GO never blocks."** — no longer in
  conflict. With compensation off, a latency-bearing plugin delays only its own path. Left on,
  it would add its latency to every cue's start, so the call is not optional — it is what keeps
  §4.1 true once any plugin declares latency.
- **§3.18, the live rack** — "The rack has a stated latency budget" now has a second reason to
  exist: the budget is not only about the live path's own delay, it is about what that delay
  does to *everything else*.
- **§3.19c, latency offsets** — the user-set signed offsets described there are for aligning
  *devices*, and do not address this, which happens inside one Edit's graph. But with
  compensation off they become the **mechanism** for the cases where alignment *is* wanted:
  `EditPlaybackContext::getLatencySamples()` still reports the graph's latency, so Go.dot can
  read the number and apply an offset deliberately rather than having one applied globally.
- **§6.1, "multiple active Edits"** — stays a fallback held in reserve. It was briefly the
  fallback the PDC decision depended on; it no longer is, because the live rack can stay in the
  single Edit.

## Open questions for the author

No architectural decision is required: PDC is turned off with one public call, so neither a
second engine nor an upstream request to Tracktion is needed. What remains is smaller:

1. **Does the rack's latency budget (§3.18) still need to be a hard cap?** Its original second
   reason to exist — that one plugin's latency taxed the whole show — is gone. The budget is now
   about the live path's own delay only, which is a smaller and more ordinary question.
2. **Where does Go.dot apply alignment deliberately?** With compensation off, Go.dot owns it. The
   engine still reports the number via `EditPlaybackContext::getLatencySamples()`, so the
   question is a product one: which paths should be aligned to which, and is that per-destination
   (§3.19c's model) or per-cue?
3. **The hardware half is not run.** Real round-trip latency through the Digiface, and whether
   `setLowLatencyMonitoring`'s buffer-shrinking behaves differently against a real driver, both
   need the interface attached. Neither bears on the verdict.
