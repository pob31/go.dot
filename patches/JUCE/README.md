# Go.dot's fixes to JUCE

Fixes JUCE has already made, carried on the JUCE Go.dot pins, applied by the build
to the submodule's working tree at `ThirdParty/JUCE`.

**Why there are any.** Go.dot pins the JUCE commit Tracktion Engine pins (root
`README.md`, *Dependency pins*), and does not move ahead of Tracktion lightly. When
JUCE fixes a fault Go.dot cannot wait for, the fix is taken from JUCE's own history
and carried here instead. The author's decision of 2026-10-14, for the first of
them: rather than move to JUCE 9.0.4 ahead of Tracktion, or to 8.0.16 and its two
releases' worth of other changes, carry the one fix.

**What belongs here, and what does not.** A JUCE patch is JUCE's own commit, taken
as JUCE wrote it, plus Go.dot's notice. A change of Go.dot's own to JUCE is not
made here: it goes in `patches/JUCE-local/` (namespace draft §125, BFD).
Each patch says which JUCE release has it, and goes when the pin moves to that
release or later.

- `series` lists the patches in the order they apply. A line starting with `#` is a
  comment.
- Each patch is a `git diff` of files inside the submodule, under a header saying
  what it is and where it came from. `git apply` skips the header.
- Each file a patch touches is in no other patch, so `refresh` can split the tree's
  diff back into them.
- Every hunk that changes JUCE carries a comment saying Go.dot modified it, when,
  and in which patch. That is section 5(a) of the AGPL-3 JUCE is used under; keep
  it when editing.
- **Line endings.** JUCE stores most of its sources with CRLF in its own
  repository, so a patch to one ends every hunk line with CR LF, and those bytes
  must survive. `.gitattributes` marks `patches/JUCE/*.patch` as `-text` (never
  converted), and `scripts/patches.py` reads and writes patches as bytes. Do not
  open one in an editor that normalises line endings and save it.

**Who applies them.** `cmake/WfgJucePatches.cmake`, on every configure, before JUCE
is added - the same steps as Tracktion's (`cmake/WfgSubmodulePatches.cmake`). When
the series is already on, it touches nothing. A tree carrying anything else stops
the configure, with the commands to look at it and clean it.
`scripts/check-pins.py` check (g) asks the same question before any toolchain
exists, and `tests/ToolchainTests.cpp` checks that the binary carries it.

**By hand**, `python3 scripts/patches.py JUCE`:

| Command | Does |
|---|---|
| `status` | which state the tree is in |
| `apply [--3way]` | put the series on; `--3way` after a pin move |
| `revert` | take it off, before pulling or making a pin move |
| `refresh` | rewrite each patch from the tree after editing the submodule |
| `new <name> <file>...` | start a patch over files no other patch touches |

Moving the JUCE pin is in the root `README.md`, under *Bumping a pin*. A pin that
moves to a JUCE carrying one of these fixes drops that patch from the series in
the same commit.

| Patch | Since | JUCE has it from | What |
|---|---|---|---|
| `0001-string-malformed-utf8.patch` | 2026-10-14 | 8.0.16, 9.0.4 | JUCE's `330ef16fa0d0`, "String: Fix allocation sizes for malformed data": a `String` built from malformed UTF-8 was filled with more bytes than it was allocated. Go.dot builds such strings from every WebSocket message the OSCQuery server receives, from show files, and from text cue words an OSC message can set. |
