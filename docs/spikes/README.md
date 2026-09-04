# Spike reports — PRD §6.1

One file per spike, each with a verdict against §6.1's own wording and what was
learned (devplan:43-44). These are the written half of a spike; the exit code
covers only the mechanically checkable part.

PRD §9.2 gives the decision rule for the whole set:
**"The polyphony model stands unless #2 or #4 fails."**

| # | Spike | Verdict | Report |
|---|---|---|---|
| 4 | Graph stability under sustained launching | **PASS** — zero rebuilds at every configuration | [spike04-graph-stability.md](spike04-graph-stability.md) |
| 2 | Launcher start at an arbitrary in-file offset | **PASS** — honoured to the nearest sample; §6.1's "one genuine gap" premise is false | [spike02-launch-offset.md](spike02-launch-offset.md) |
| 1 | Launcher clip → multichannel bus routing | **PASS** — mono, stereo and mixed 64-ch rigs all exact; limit is throughput (~72 objects @48k, ~40 @96k) | [spike01-bus-routing.md](spike01-bus-routing.md) |
| 3 | Follow-action join quality | **PASS** on sample-accuracy; **NO** crossfade without a custom clip; artefact is buffer-dependent; overlapping copies are sample-aligned (no comb filtering) | [spike03-join-quality.md](spike03-join-quality.md) |
| 5 | External parameter control at 50 Hz | **PASS** — 512 params = 13% of a 20 ms tick (Release); ~4 µs per write | [spike05-param-50hz.md](spike05-param-50hz.md) |
| 6 | Live-input latency through a Rack, PDC | **FAIL** — PDC delays the whole graph by the worst plugin latency; §3.25's warning is real | [spike06-rack-latency-pdc.md](spike06-rack-latency-pdc.md) |
| 7 | Proxy-plugin sandbox as a custom TE plugin type | **PASS** — 0.9 µs round trip, survives the child being killed | [spike07-proxy-plugin.md](spike07-proxy-plugin.md) |
| — | *Also verify*: TE transport chasing MTC | not yet run | — |
| — | *Also verify*: multiple Edits summed by the DeviceManager | not addressed by any spike; fully open | — |

Rows are in the devplan's priority order (devplan:42), which is **not** §6.1's
numbering: #4 first because it is what the polyphony model rests on.

## Running them

```
cmake --preset spikes && cmake --build --preset spikes-debug
bash spikes/run-spikes.sh --ci   build/spikes/spikes-bin/Debug     # what CI runs
bash spikes/run-spikes.sh --full build/spikes/spikes-bin/Release   # the author's sweep
```

Spikes take `--tracks`, `--sample-rate` and `--buffer` with **no defaults**: the
fixed track count and the target rates and buffer sizes are open author decisions
(devplan:49-50), and a default written anywhere in the tree would answer them.
