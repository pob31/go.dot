# Bug round of 2026-10-05: S4, as built

Written on branch `bugs-ui` for the integrator to append to `docs/godot-namespace-draft-0.1.md`
§30, under the next free number. The window-side stages do not edit the draft, because it would
conflict across branches. Everything below the rule is the subsection itself.

---

### 30.N What was built: S4 - the Surfaces tab

The triage held. A DCA reaches a fader only through a strip, with `<Strip role="dca" dca="<id>">`,
and the tree points a strip at `/godot/dca/<id>/trim` only when its role is `dca`. ADD DCA declares
a DCA and nothing more. In the tab, the way to put a DCA on a fader took two steps, and the second
was hidden. You chose DCA in the Role cell, and only then did the DCA cell beside it take a click.
Until then it was a dash. The engine already moved strips (`object.move` into the surface, the
index derived from the element's position, §16.2), but the tab offered no gesture for it. The
author's show has four DCAs and twelve bare strips. Its three sampler members marked with DCA 1
are cue marks, not faders, so no fader rode any of the four DCAs, and nothing on the tab said so.

**One menu in the Role cell** (`model::roleChoices`). It offers "Sampler", then "DCA: <name>" for
each DCA the show declares, in the show's order and by the whole name, as `dcaChoices` names them.
A show with no DCA gets one greyed item instead: "No DCA yet: ADD DCA below makes one". Choosing a
DCA sends `node.set /godot/slot/<id>/role dca` and then `.../dca <id>`, in that order
(`model::roleWrites`). Choosing Sampler sends `role sampler` and clears `dca`. Anything already
set is not written again, and the writes are worked out against the strip as it is when the
choice lands, not as it was when the menu opened. The cell shows the words of the item that
matches the strip now (`model::roleWords`): "Sampler", "DCA: Band".

**Each DCA names the faders riding it** (`model::fadersRiding`), in a new read-only column,
"Fader", between Short name and Inside. It reads "Fader 3", "Faders 3 and 7" or
"Faders 3, 5 and 7". With more than one surface in the show, each fader is named with its
surface: "Asparion D700 · faders 3 and 7; Virtual panel · fader 2". A pad is called a pad. A DCA
no fader rides reads "no fader", drawn greyed. Only a strip whose role is `dca` counts, because the
engine reads `dca` on no other strip. The column is redrawn when a strip's role changes, when a
strip moves and when a surface is renamed.

**Strips are reordered by dragging**, as the outputs, the inputs and a rack chain are. A row
carries its identifier as `strip:<id>`, and a line between two rows shows where it would land.
Letting go sends one `object.move <strip> <its surface> <position>`. The position follows
model/Reorder.h's rule, in the list as it stands with the dragged strip still counted
(`model::stripMovePosition`). Let go in the gap above or below itself, the strip is not moved and
nothing is sent. Each row's number has a grip, "≡", drawn while the show is unlocked. Under the
lock there is no drag, and anything dropped is refused.

Tests:
- ClientTests, both locales:
  - `client: a strip's Role is one menu, Sampler then each DCA, and a choice sends the role before the DCA`
    covers the menu for a sampler strip, a DCA strip, a DCA strip riding none, a strip naming
    an undeclared DCA, and a show with no DCA. It covers the writes for every change and for
    every choice that changes nothing. It then sends two choices through the registry and the
    engine (`node.set`, `checkArgs`, one tick) and reads the strip riding Band's trim in the
    tree, then the same strip back as a sampler strip with no DCA.
  - `client: each DCA says which faders ride it, in words, and no fader when none does` runs on
    two surfaces, then on one after the desk is deleted. It covers two and three faders, a
    sampler strip still carrying the DCA (not counted), and pads.
  - `client: a strip dragged in the Surfaces tab lands where its line was, and the engine renumbers the surface`
    checks the gap arithmetic, edges included. It then sends a real `object.move` that makes the
    panel's first strip the fourth: the indices come back re-derived, and the DCA's read-out
    follows its fader from 2 to 1. A second move puts it back.
- wfg_audio_ui_tests, both locales:
  `show settings UI: a strip's Role is one menu of Sampler and the DCAs, and a strip dragged to a new place is moved`.
  It clicks the drawn Role cell and reads the chooser's items and what is selected. It picks
  "DCA: Keys" and finds two `node.set`s in order. A second pick of the same item sends nothing,
  and Sampler sends the role and an empty DCA. It drags through the page's own
  `DragAndDropTarget` and finds `object.move <strip> <surface> 3`. Letting go on either side of
  the strip sends nothing, and a plugin is not taken. With no DCA, the greyed item is disabled.
  Locked, there is no drag and nothing dropped is taken. The case was run against the page as it
  was before this stage and failed on its first menu read: two items ("sampler", "DCA"), not three.

- **ST - The DCA column is gone; the Role cell says it.** "DCA: Band" in one cell is the menu that
  set it, so a second column would say the same thing again. The Role cell takes the old DCA
  cell's room, up to 360 px. Its colour follows the old DCA cell's: greyed for "DCA: none
  chosen", the failure tone for a DCA the show no longer declares, and the words carry both
  (§4.8). The Strip list's headings are now Strip, Role, State.
- **SU - One choice, two undo steps.** The role and the DCA are two `node.set`s on two addresses,
  and the document joins only repeated writes to one address into one step. So Ctrl-Z takes back
  the DCA first and the role second. Merging them would need an engine change (a command that
  writes both, or a joining rule), which is out of scope on a window-side stage. If one step
  matters, it is the engine's to give.
- **SV - A fader's surface is named the way the strip menu names it, "Asparion D700 · fader 5".**
  The brief suggested "Surface 2, fader 5". The middle dot is how a sampler member's strip menu
  and the level lane's taken fader already name a fader on a surface, and one fader should not
  be named two ways. It also keeps the comma free for the list "3, 5 and 7". The surface is
  named whenever the show declares more than one, even if every rider is on the same surface,
  because "fader 3" alone would not say which desk.
- **SW - Drag, not up/down buttons.** The table could take the output list's gesture as it is.
  The strips list's click now fires on mouse-up (`setRowSelectedOnMouseDown (false)`), so a press
  that becomes a drag does not open the Role menu under the hand on its way down. A click that
  slips a few pixels becomes a drag that lands on itself and does nothing. A strip moves only
  within the surface picked above. The engine would take it into another surface, but the tab does
  not offer that.
- **SX - The strip's current state is always one of the items.** A DCA strip riding none reads
  "DCA: none chosen". One naming a DCA the show no longer declares reads "DCA: <id>  (not
  declared)". Both come last, so the menu never opens with nothing selected. Choosing Sampler
  also clears a DCA left on a sampler strip from before, which nothing shows.
- **SY - ADD DCA's tooltip points back at the Role.** "Declare a DCA: a trim that the cues and
  groups marked with it follow. A fader rides it once a strip's Role, above, names it." It was
  "..., ridden from a dca strip", which named a cell nobody could find.

Not done, for the bench: moving a strip while a sampler group holds it. A run's claim follows the
strip's identifier, so the clip should go with the strip to its new fader. S3 owns how the D700
shows that, and this stage did not touch the bridge.
