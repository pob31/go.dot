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

## Where these numbers came from, and which of them are portable

All runs so far are on a **Lenovo laptop with an Intel Core Ultra 7 255H** — 16 cores, hybrid
performance/efficiency, Windows 11, with ordinary desktop background load.

Findings split into two kinds, and the distinction matters when reading any report here:

- **Portable.** Sample accuracy, routing correctness, PDC behaviour, API shape, whether a
  mechanism exists at all. These are properties of Tracktion Engine and will not change on
  other hardware.
- **Machine-dependent.** Throughput ceilings, reproducibility under load, and every timing
  figure. A hybrid-core mobile CPU migrates threads between P- and E-cores under scheduler and
  thermal pressure, which is the most likely cause of the intermittent non-reproducibility seen
  at 96 kHz in spikes 01, 03 and 04.

The machine-dependent half is worth re-running on a **Mac mini M4 Pro** — desktop thermals, a
different scheduler, and the macOS platform CI currently only builds on:

```
git clone --recurse-submodules https://github.com/pob31/go.dot.git   # or: bash scripts/bootstrap.sh
cmake --preset spikes && cmake --build --preset spikes-release
bash spikes/run-spikes.sh --full build/spikes/spikes-bin/Release
```

## Audio workgroups, and the JUCE 9 question

Two related questions came up while reading these results: whether **audio workgroups** would
stabilise the timing, and whether the project should be on **JUCE 9.0.1**. Both were checked
against the actual sources rather than reasoned about, and the answers are independent.

### Workgroups: yes, and they are available now — TE has them, switched off

Tracktion already supports them, and the plumbing is complete:

```
EditPlaybackContext.cpp:38   static bool useAudioWorkgroup = false;      // OFF by default
EditPlaybackContext.h:158    static void enableAudioWorkgroup (bool);    // the opt-in
EditPlaybackContext.cpp:47   e.getDeviceManager().deviceManager.getDeviceAudioWorkgroup()
LockFreeMultiThreadedNodePlayer.cpp:233
                             threadPool->createThreads (numThreadsToUse, audioWorkgroup);
```

So when enabled, TE takes the workgroup from the audio device and creates its **graph worker
threads inside it** — which is exactly the right place, since those threads are what a hybrid
scheduler would otherwise migrate onto efficiency cores.

**It is one call, `EditPlaybackContext::enableAudioWorkgroup(true)`, and it needs no JUCE
upgrade** — `juce::AudioWorkgroup`, `AudioIODevice::getWorkgroup()` and
`AudioWorkgroup::join(WorkgroupToken&)` are all present at the pinned JUCE 8.0.6.

**But it will not change any number in these reports.**
`JUCE_AUDIOWORKGROUP_TYPES_AVAILABLE` is defined in `juce_AudioWorkgroup_mac.h` and is 0
everywhere else, so on this Windows laptop `AudioWorkgroup` is an empty shell and enabling it
is a no-op. It matters on **Apple Silicon**, which is precisely where the Mac mini cross-check
would run — and it is the reason that cross-check should enable it rather than measure the
default.

On Windows the comparable lever is not a workgroup at all but MMCSS
(`AvSetMmThreadCharacteristics` with "Pro Audio"), which is a different mechanism with a
different owner.

### JUCE 9.0.1: not possible today, and the blocker is Tracktion, not us

Attempted, measured, reverted:

- JUCE moved to **9.0.1**, configure succeeded, and the build failed with **20 errors across 5
  Tracktion source files** — `PluginManager.cpp`, `ExternalPlugin.cpp`,
  `AudioFormatManager.cpp`, `FloatAudioFileFormat.h`, `PluginScanHelpers.h`.
- The causes are documented JUCE 9 removals, not accidents. The JUCE docs for
  `AudioPluginFormatManager::addDefaultFormats` read: *"This function has been removed. To add
  default formats to the manager, use one of the new functions `addDefaultFormatsToManager()`
  or `addHeadlessDefaultFormatsToManager()`."* `AudioProcessor::TrackProperties` and
  `AudioFormat::createWriterFor` changed shape likewise.
- **Upstream Tracktion is not there either.** `develop` is 404 commits ahead of `v3.2.0` and
  still pins JUCE **8.0.13**; the single commit mentioning JUCE 9 touches only its *Examples*.

So JUCE 9 needs a Tracktion release that supports it. Three options, and the choice is the
author's:

| | what it buys | what it costs |
|---|---|---|
| **stay at JUCE 8.0.6** (current) | matches TE v3.2.0's own pin exactly; known-good | no JUCE 9 |
| **TE `develop` + JUCE 8.0.13** | 404 commits of fixes, including real bug fixes visible in the log (a data race in `Oscillators`, a `toBitSet()` bug) | tracking a non-release branch |
| **patch TE for JUCE 9** | JUCE 9 now | forking TE; 20 errors is only what the compiler reached before stopping |

Note that `scripts/check-pins.py` anticipated exactly this: check (b) asserts our JUCE pin
matches TE's own, and `--allow-skew` exists so that moving JUCE ahead of Tracktion has to be
said out loud rather than happening quietly.

## Engine version these results were measured against

All seven spikes were first written and measured against **Tracktion Engine v3.2.0** with
**JUCE 8.0.6**, then re-run after the project moved to Tracktion's **develop** branch
(**3.5.0**) with **JUCE 8.0.13**.

| finding | v3.2.0 | develop 3.5.0 |
|---|---|---|
| #4 graph stability — zero rebuilds | PASS | **PASS, unchanged** |
| #2 offset honoured to the nearest sample | PASS | **PASS, unchanged** |
| #3 join sample-accurate, 40-sample artefact | PASS | **PASS, unchanged** |
| #3 overlapping copies sample-aligned | PASS | **PASS, unchanged** |
| #6 PDC delays the whole graph | FAIL | **FAIL, unchanged** |
| #7 proxy sandbox, deadline and kill path | PASS | **PASS, unchanged** |
| #1 **track width capped at 2 channels** | hard `constexpr` limit | **REMOVED — discrete multichannel, verified to 8** |
| #1/#6 **mono into a wider destination** | stays in channel 1 | **canonically upmixed** |

Only the last two moved, and both are consequences of 3.5.0's new
`ChannelConfiguration` / `BusLayout` system. Everything the polyphony model rests on is
unchanged across 404 commits of engine development, which is a useful thing to know
independently of the findings themselves.

Two API migrations were needed in the spike code itself, and the `spikes` CI job is what
would have caught them: `WaveDeviceDescription`'s constructors were replaced by
`withNumChannels(...)`, and `Plugin::getBusses()` became pure virtual.
