# Go.dot's patches to Tracktion Engine

Changes Go.dot makes to Tracktion Engine, applied by the build to the submodule's
working tree at `ThirdParty/tracktion_engine`. Decision DQ (2026-09-28): a patch the
build applies, not a fork. The engineering record is namespace draft §22.3.

- `series` lists the patches in the order they apply. A line starting with `#` is a
  comment.
- Each patch is a `git diff` of files inside the submodule, under a header saying what
  it does and why. `git apply` skips the header.
- Each file a patch touches is in no other patch, so `refresh` can split the tree's
  diff back into them.
- Every hunk that changes Tracktion carries a comment saying Go.dot modified it, when,
  and in which patch. That is GPL-3's section 5(a) for a modified work; keep it when
  editing.

**Who applies them.** `cmake/WfgTracktionPatches.cmake`, on every configure, before a
Tracktion source is read. When the series is already on, it touches nothing. A tree
carrying anything else stops the configure, with the commands to look at it and clean
it. `scripts/check-pins.py` check (g) asks the same question before any toolchain
exists.

**By hand**, `python3 scripts/te-patches.py`:

| Command | Does |
|---|---|
| `status` | which state the tree is in |
| `apply [--3way]` | put the series on; `--3way` after a pin move |
| `revert` | take it off, before pulling or making a pin move |
| `refresh` | rewrite each patch from the tree after editing the submodule |
| `new <name> <file>...` | start a patch over files no other patch touches |

Moving the Tracktion pin is in the root `README.md`, under *Bumping a pin*.

| Patch | Since | What |
|---|---|---|
| `0001-auto-tempo-clips-may-resample.patch` | 2026-09-28 | `EngineBehaviour::autoTempoClipsUseDefaultTimeStretcher()`: an auto-tempo clip left at `disabled` may be resampled instead of stretched. Upstreamable. |
| `0002-launched-clip-speed.patch` | 2026-09-28 | `LaunchHandle::SpeedSource`: a launched clip's speed, which can move while it plays, read by `WaveNodeRealTime` and `SlotControlNode`; the varispeed gate and the stretched freeze at nought; the stretcher primed at one and aimed for the speed it will play at (its output latency reported on its own, in `TimeStretcher`), and primed again at its first read when its file was not there to prime it at the build; the Lagrange reader tracking the interpolator exactly (no click at a ratio that is not a whole number of frames a block, nor at a rebuild mid-play), and low-passing the file above one so a speed does not alias; a launched clip's loop below its resampler and its stretcher (`UnrolledLoopReader`), so a stretched loop does not start its stretcher again at every pass. |
