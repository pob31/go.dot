# Handoff — the plugin-voice CI flakes (`blackbox.phase9a-fx`), what is known

*Written 2026-09-26 by the session that built the D700 arrows and the master dial, for the session
building the live rack and live sampling (9b). Everything below is on `main` at 6dcf8a5 or later.
This session is not editing plugin, proxy or audio code, and will not.*

## In one paragraph

`blackbox.phase9a-fx` fails now and then on CI, mostly on macOS, never locally on Windows, and never
on a commit that touched audio. It has failed six times in two days, on three different checks, and
each time a re-run passed. The driver cannot yet say *why* a window is wrong: a dry block and a
block processed at gain 1.0 read the same, and the proxy's count of late blocks is published
nowhere. There is also one real ordering bug in `HostPlayer::serviceArms`, found while reading for
this, that is probably not the cause of these runs but will matter to anything that arms a voice
and sets its plugin state together, which the rack and live sampling may.

## Already done (do not redo)

| commit | what |
|---|---|
| `45882c5` (yours) | each level check's detail line gives the share of the window's blocks at each level, and where the first and last blocks sit |
| `b284a4e` | a *different* flake, `blackbox.phase9b-plugins` "and so does the first, again": after `plugin.load` the driver waited for the second entry and read the first once, and a Linux runner caught it still `loading`. It now waits for both. Seen on `688730e` and `8e02faa`; green since |

## The sightings

| commit | job | check | measured / expected | note |
|---|---|---|---|---|
| `6fb7025`, `f7e0582` | macOS C | whole state: "with the state's Pad on, … a quarter of a half" | 0.25 / 0.031 | fully dry |
| `4dd9b28` | macOS C | the same | 0.125 / 0.031 | processed, **no Pad**: the state was not heard |
| `4dd9b28` | macOS fr_FR | "at its baseline, … a half" | 0.1875 / 0.125 | 1 % of blocks late |
| `963105c` | **Linux** fr_FR | Pad | 0.125 / 0.031 | processed, no Pad |
| `c799b7d` | macOS C | both Pad checks | — | re-run passed |
| `b284a4e` | macOS fr_FR | "p0 at three quarters moves the render to three quarters" | 0.25 / 0.1875 | "100 % of the blocks late (dry)" for the whole second |

Three different shapes: **dry**, **processed without the state** (Pad missing), and **a window at
the wrong value**. They may not share a cause.

## Findings

### 1. The driver cannot tell dry from gain 1.0

The test gain is `gain = p0` linear (`PluginHostChild.cpp`, `TestGainWorker::run`), so a voice
processed at `p0 = 1.0` sits at `SOURCE` (0.25), exactly where a dry block sits. `dry_fraction` and
`45882c5`'s `shares` both call a 0.25 block "dry". In the `b284a4e` failure the window read 0.25
throughout; "late" is a guess the numbers cannot confirm.

### 2. A second of late blocks should have failed the plugin, and nobody looked

`ProxyLane` counts misses (`misses()`, `consecutiveMisses()`, `ProxyLane.h:179-186`), but the only
reader is `ProxyHost.cpp:421`: **8 late in a row** (`missesBeforeFailure`) marks the plugin
`failed` with *"the plugin stopped answering: 8 blocks late in a row on …"*. A whole second late at
64-sample blocks is 750 in a row, so if the `b284a4e` window really was late, the plugin was
already failed **before** the driver sent the kill (`p1 = 1`). The driver never reads the state
before the kill, and the check after it ("the entry reads failed") passes either way.

Two other ways a block passes dry **without** counting a miss: the lane switched off or not to be
called, and the lane **parked** while a state loads (`lanes.parked`). Checked and ruled out for
that window: a live `p0` write goes through `AudioHost::setTrackFxParameter` (`AudioHost.cpp:2080`),
which sets a parameter and loads no state; `requestFxState` is only sent when `stateFile` changes
before the launch (`Runner::applyFx`).

**Cheapest next step, driver only:** just before sending the kill, and after each window, read
`/godot/plugin/<id>/state` and `/problem` and put them in the detail line; after the session, grep
the driver's log for "stopped answering". That alone says whether it was misses. **The product
step** is to publish the miss count (a read-only `plugin,misses` row, or per insert), which would
also let an operator see a plugin that is not keeping up; that one is the author's to agree to.

### 3. `serviceArms` applies every queued state before every queued arm

`HostPlayer::serviceArms` (`HostPlayer.cpp:68-147`) swaps both queues, applies **all** state
requests (`wantState`, lines 79-81), then **all** arms, and an arm's `snapTrackFx` calls
`wantState` again with the arm's own path (`AudioHost.cpp:2101`). So an arm and a *later*
`requestFxState` landing in the same 10 ms batch end with the arm's **older** path: the newer state
is lost. A real bug; the fix is one queue, applied in the order it was filled. Probably not what
these drivers hit (neither re-arms the standby after `fx.create`), but it produces exactly the
"processed without the state" shape, and the rack and live sampling are likely to arm and set
state close together.

## Suggested order

1. The driver-only readings in finding 2, pushed alone, then let CI fail once more: the next
   failure then says "misses", "parked/off", or "processed at the wrong value".
2. Fix `serviceArms`' ordering (finding 3), with a unit test that queues an arm then a state for
   the same lane and checks the state wins.
3. Depending on 1: the macOS proxy worker's priority/scheduling (the old suspicion), or the
   parameter path, or the render-window alignment (`moved_at` comes from `frames_on_disk`, which
   lags the render by up to the header's one-second rewrite interval).
4. Ask the author about publishing the miss count.

## Traps

- **macOS cannot be run here.** The author has a Mac mini for cross-checks.
- **A push to `main` cancels the CI run in progress on `main`.** Wait for a run to finish first.
  `gh run rerun <id> --failed` and `gh run view --log-failed` only work once the **whole** run has
  finished; for one finished job while others run, use
  `gh api repos/pob31/go.dot/actions/jobs/<job-id>/logs`.
- **Never run `wfg_audio_ui_tests` or any audio test while the author's session holds the MADIface**
  (`Get-Process wfg` first).
- **This checkout is shared** between sessions. Commit your own paths
  (`git commit -m "…" -- <paths>`), not `git add -A`.
