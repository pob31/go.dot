# Go.dot's own changes to JUCE

Changes Go.dot makes to JUCE itself, for what JUCE does not do yet, carried on the
JUCE Go.dot pins and applied by the build to the submodule's working tree at
`ThirdParty/JUCE`, beside JUCE's own fixes in `patches/JUCE/`.

**Why a second series.** `patches/JUCE/` holds only fixes JUCE has already made,
taken as JUCE wrote them; its README says a change of Go.dot's own is not made
there. Some changes are Go.dot's own and still have to be in JUCE: the author's
decision BFD of 2026-10-31 (namespace draft §125) gives them this folder, so the
two kinds are never mixed and each goes for its own reason - a fix when the pin
reaches the JUCE release that has it, a change of Go.dot's own when JUCE does the
same or Go.dot stops needing it.

- `0001-macos-touch-sources.patch` (2026-10-31): `canUseTouch()` answers true on
  macOS, so the USB touch panel reader (spatcore's `MacHidTouchManager`) can hand
  JUCE each finger as a touch source of its own. spatcore's own patch, which
  WFS-DIY carries too, with Go.dot's notice. Goes when JUCE lets a touch source
  exist on the Mac, or when the panel reader no longer injects.

The rules are `patches/JUCE/`'s, and so is the machinery:

- `series` lists the patches in the order they apply.
- Each file a patch touches is in no other patch **of either series**, so
  `refresh` can split the tree's diff back into them; `scripts/check-pins.py`
  check (g) says so if two patches meet on one file.
- Every hunk carries a notice saying Go.dot modified the file, when, and in which
  patch - section 5(a) of the AGPL-3 JUCE is used under.
- JUCE stores its sources in CRLF, so a patch to one carries CR on every hunk line;
  `.gitattributes` keeps these files byte for byte (`-text`).
- Applied on every platform, whatever file a patch touches, so every build tree has
  one state.

**Who applies them.** `cmake/WfgJucePatches.cmake`, on every configure, after
JUCE's own fixes, with its own stamp (`wfg-applied-JUCE-local` in the submodule's
git directory). **By hand**, `python3 scripts/patches.py JUCE-local` with
`status`, `apply [--3way]`, `revert`, `refresh` or `new <name> <files>`.
