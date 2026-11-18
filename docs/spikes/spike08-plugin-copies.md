# Spike 08 — a plugin copy made while its sibling sounds; what a copy costs

*Not §6.1's: the spike of the per-cue chains plan (namespace draft §133, stage S0), run 2026-11-18 on the
Mac mini (macOS 15.8.3, M4 Pro, 12 cores), Release build of `3cfa13d5`, with two plugins the author uses -
Seventh Heaven (LiquidSonics, a convolution reverb) and Oxford Envolution (Sonnox), both PACE-protected VST3.*

## The questions

The plan pooled plugin copies - a parked copy per plugin **and state** in use - and made copies inside a
running child process, beside copies that may be sounding. Three things only real plugins could say:

1. Does making a copy disturb a sibling that is sounding in the same child?
2. How long does a copy take to make, and a cue's whole state to load into one that exists?
3. What does a copy cost in memory?

## How

M35 (`tests/ProxyTests.cpp`) as it stands: a two-lane child, voice one playing a block every 1.33 ms at the
default deadline, voice two loaded with two states in turn, twenty times, then the same length idle. For the
copy, a switch in the child for this spike only (`WFG_SPIKE_REMAKE`, never committed): before a state goes in,
the lane's instance is replaced by a fresh `makeInsertInstance`, on the message thread with the lane parked -
what a pooled host's loader would do. Memory: M31's children of 1, 8 and 16 instances, the child's resident
size sampled twice a second.

## What it measured

| | Seventh Heaven | Oxford Envolution |
|---|---|---|
| a state into a copy that exists | 6.6 / 6.7 / 7.8 ms (min / median / max) | 2.3 / 2.4 / 2.7 ms |
| ... voice one's misses meanwhile, and idle | 0, 0 | 0, 0 |
| a fresh copy, then the state | 240-400 / 540-670 / 700-1100 ms | 8.3 / 8.6 / 9.3 ms |
| ... voice one's misses meanwhile, and idle | **45, 49, 40, 91** against 32, 7, 13, 5 | 0, 0 |
| resident memory, child of 1 / 8 / 16 copies | 361 / 2027 / 3861 MB - **about 235 MB a copy** | not taken |

Voice one in M35 is a plain thread, not a real-time one, so the idle column is this test's own noise; the
copies' column stands well clear of it for Seventh Heaven and not at all for Envolution.

Also seen, M31 with Seventh Heaven: the first block after the child's worker has idled took 2.5 to 15 ms
(test-gain: 1 µs), and at eight lanes of 256 samples sixteen blocks of 2000 were missed with one of 20 ms. A
lead for the *stopped answering: 8 blocks late in a row* the author's Mac logged that night on bx_boom, not
followed here.

## Verdict

1. **Yes, for a heavy plugin**: making a Seventh Heaven beside a sounding one drops blocks of the sounding
   one. A copy must never be made in a child that has a lane sounding.
2. **A state is cheap, a copy is not**: 2 to 8 ms against half a second and more. A cue's state can go in at
   standby - or at a cold GO, which is then late by milliseconds - into a copy made long before.
3. **A copy is dear in memory**: 235 MB each. The engine as it is gives every voice one: sixteen voices,
   3.9 GB for one reverb. A copy per cue would be no better on a show that uses it on many cues.

## What it changes in the plan

- **Copies are pooled by plugin, not by plugin and state**: as many as sound at once and one to spare, never
  one per cue. The cue's state goes into a free copy when the cue comes to standby (`wantState`, as built).
- **No instance protocol in the child** (the plan's region v6 and `InstanceLoader` are dropped): a child is
  launched with its copies made before it says ready, as today. More copies of a plugin are another child,
  which disturbs nobody; the loader makes them nearest the standby first.
- Memory and opening time then follow how many cues of one plugin overlap, not how many cues have it.

Open: a state that changes what a plugin has loaded (another impulse response) may cost far more than the
7 ms measured, whose two states differed by one value; to be read on real shows through `stateLoadMs`.

## The switch, for whoever runs it again

In `RealHost` (`src/wfg/engine/plugin/PluginHostChild.cpp`): `create` keeps its description, width, rate,
block size and preset; `loadState` begins, when `WFG_SPIKE_REMAKE` is set, by making a fresh instance with
`makeInsertInstance` from them, releasing the lane's old one and putting the fresh one in its place. Then
`WFG_REAL_VST3=<identifier> wfg_tests --test-case="M35*" --no-skip`, with and without the variable.
