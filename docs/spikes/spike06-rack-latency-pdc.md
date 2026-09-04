# Spike 06 — Live-input latency through a Rack, and PDC

## Verdict

**FAIL, and this is the most consequential result of the seven.** PRD §3.25's warning is
confirmed, not retired:

> *"Watch PDC on live input: TE may insert delay to align a live track with the rest of the
> graph, which is exactly what the rack path must not have."*

It does exactly that. A plugin with 250 ms of latency on **one** track delays **every other
track's file playback by exactly 250 ms**. The shift tracks the plugin's latency precisely —
50 ms gives 50 ms, 100 ms gives 100 ms — and a Rack behaves identically to a bare plugin.

This is correct DAW behaviour and wrong show behaviour. In a show it means a reverb on a live
microphone puts a quarter of a second between the GO button and every cue in the system.

**The documented escape hatch does not work**, and **Tracktion's Edit layer offers no way to
disable latency compensation**, though the lever exists one layer below it.

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
engine:  Tracktion Engine v3.1.0 (runtime string), JUCE v8.0.6
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

Mono source into the stereo bus: `bus_peak_left = 0.5`, `bus_peak_right = 0`.

## What was learned

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

**The lever exists, one layer below where TE uses it.** `tracktion_graph` supports disabling
compensation — `LockFreeMultiThreadedNodePlayer::setNode(..., bool disableLatencyCompensation)`,
`NodePlayerUtilities::createNodeGraph(..., disableLatencyCompensation)`, and the flag is
honoured in `SummingNode`, `ConnectedNode` and `RackReturnNode`. But
`EditPlaybackContext.cpp:207` calls the **three-argument** `setNode`, so the parameter defaults
to `false`, and nothing in the Edit layer ever passes `true`. Grepping the whole engine, the
only place `disableLatencyCompensation` is even read is inside the graph nodes themselves.

So the capability Go.dot needs is present in the graph library and unreachable through the
public engine API. That is a much better position than "impossible", and a much worse one than
"supported".

**Mono stays mono.** A mono source on a track routed to a stereo bus produces signal on the
left channel only (`0.5` / `0`) — no automatic spreading or centre-panning. Combined with
spike #1's finding that mono *destinations* work exactly, the picture for the spatial workflow
is consistent: what you declare is what you get, in both directions.

## Consequences for the PRD

- **§3.25, "Plugins and the rack are where TE earns its keep"** — the warning becomes a
  **finding**. Proposed wording: *"PDC is global: a plugin's declared latency delays every
  other track in the Edit by the same amount, Rack or not. `Edit::setLowLatencyMonitoring` does
  not avoid it. `tracktion_graph` can disable compensation
  (`LockFreeMultiThreadedNodePlayer::setNode`'s `disableLatencyCompensation`) but
  `EditPlaybackContext` never passes it, so reaching it requires going below the Edit API."*
- **§4.1, "The GO path is small, boring and merciless. GO never blocks."** — this is where the
  finding bites hardest. Any latency-bearing plugin anywhere in the Edit adds its latency to
  every cue's start. That is a direct conflict with a stated law, not a performance detail.
- **§3.18, the live rack** — "The rack has a stated latency budget" now has a second reason to
  exist: the budget is not only about the live path's own delay, it is about what that delay
  does to *everything else*.
- **§3.19c, latency offsets** — the user-set signed offsets described there are for aligning
  *devices*. They do not address this, which happens inside one Edit's graph.

## Open questions for the author

1. **Which of the three routes does Go.dot take?** This is an architectural decision and it is
   yours:
   - **(a)** Reach below the Edit API to pass `disableLatencyCompensation` — Go.dot then owns
     alignment entirely, which it arguably should anyway since it owns time (§3.25).
   - **(b)** Keep latency-bearing plugins out of the shared Edit — the live rack lives in a
     separate Edit or a separate engine instance, which reopens §6.1's "multiple active Edits"
     question that no spike has yet addressed.
   - **(c)** Accept the latency on the GO path — which contradicts §4.1 and is listed only for
     completeness.

   My recommendation is (a), because it matches the inversion §3.25 already commits to: Go.dot
   owns time and TE is the player. But it means depending on a `tracktion_graph` entry point
   that the engine layer does not use, which is a maintenance cost to accept knowingly.
2. **Does the rack's latency budget (§3.18) become a hard cap rather than a target?** If PDC
   stays enabled, the budget *is* the added GO latency for the whole show.
3. **The hardware half is not run.** Real round-trip latency through the Digiface, and whether
   `setLowLatencyMonitoring`'s buffer-shrinking behaves differently against a real driver, both
   need the interface attached.
