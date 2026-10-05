# Bug round of 2026-10-05: S6, as built

Written on branch `bugs-ui` for the integrator to append to `docs/godot-namespace-draft-0.1.md`
§30, under the next free number. The window-side stages do not edit the draft, because it would
conflict across branches. Everything below the rule is the subsection itself.

---

### 30.N What was built: S6 - the preset mark

A member let go on its own group's header band is now marked instead of moved (decision QZ). It
stays where it is and takes `preset` naming that group, with one `node.set`. A drag carries one
cue, so it is one write. "Its own group" means any group whose BODY the cue sits in, at any depth:
a cue two groups down, dropped on the outer group's band, is marked for the outer group. The new
`model::headerMarkFor` asks this. `dropAtDepth` asks it before anything else, because the answer
depends on where the dragged cue sits, which `dropFor` (one row at a time) cannot see. Everything
else on that band moves into the header as before: a cue from outside the group, and a cue from
the group's own header or footer. The band lights in the mark's tone, and the words in the air are
"prepare it in Scene's header, keeping it in its place". When the hand lets go, the foot says
"prepared in Scene's header, and kept in its place". Every gesture that writes the mark now says
it at the foot: the alt-drop on a title and a dragged header line, as the ctrl/⌘-arrows always
did. The engine publishes the reading of the mark (`headerDerived`) on the next pass, and the list
draws it there: an italic line in the header band, the cue's own row unmoved.

The inspector's `preset` row is now a menu, `Control::groupRef` (`offerTheGroupsAround`). Its
items are "not prepared ahead", then the cue's own group, then each group outside it, innermost
first. Each group reads as its row does ("2.1 Inner") and is written as its identifier. The walk
is the tree's `/parent` chain, so a header's or a footer's cue is offered its group too: the
engine prepares a mark wherever under the group the cue sits. The menu is its own control rather
than a `choice`, because a `choice` is the parameter table's closed set, while these answers
come from where the cue sits, as an output menu's come from the rig. So it joins the menus the
show writes: the same drawing, a commit by key, and a refill when the groups change while the
cue stays picked.

Over a band, `describe` named the band's own word ("header"), so a move that made a header said
"into header's header". `model::groupNamedBy` now gives it the group's row for a mark and for a
header or footer move: "into Inner's header".

Tests:
- ClientTests, both locales:
  - `client: a member dropped on its own group's header band is marked and stays, and a cue from outside is moved in`
    runs on the `descent` fixture: a member two groups down at every hand depth, a direct member,
    the stranger, the footer's cue and the header's own cue, after a header row, and the footer
    band. It writes the mark through the real `node.set` and reads the reading in the tree and in
    the rows (`count` 2, one derived line, the own row still in Inner). It also checks the drop
    again ("already prepared"), the mark moving inward onto a grown band, the outsider making
    Inner's header, and the mark cleared.
  - `client: a cue's preset is a menu of the groups around it, innermost first, and over several only theirs in common`
    covers every place in `descent`, three selections, a picked mark, and a mark naming no group
    around the cue.
- wfg_audio_ui_tests, both locales: `inspector UI: a cue's preset is drawn as a menu of the groups around it, and a pick writes the group`
  finds the drawn menu, checks its items and that it is visible, and checks that a pick sends
  the group's identifier to `/godot/cue/<id>/preset`.

Both ClientTests cases were seen failing (26 assertions) with `headerMarkFor` and the menu
switched off.

- **SD - The menu's first item reads "not prepared ahead", and a mark naming no group around the
  cue stays in it.** The phrase is the one the arrows and a dragged header line already say when a
  mark comes off. A stale mark (validate's "not a group this cue is inside") is shown as its own
  item, "2 Scene (not a group it is in)". Without that, the menu would show nothing picked, which
  says the cue has no mark.
- **SE - Several cues are offered only the groups around every one of them.** The pick is written
  to all of them, so an item only some of them sit under would mark the rest for a header that
  never reaches them. "Not prepared ahead" is around everything, so the menu is never empty.
- **SF - Only the body, and only the band.** A cue in the group's own header or footer is not a
  member, and is moved as before, as is a cue from outside. A member dropped AFTER one of the
  header's own rows is still moved into the header at that place. One consequence: while a header
  is empty, a member cannot be put into its own group's header with one drag. It has to be dragged
  out first, or dropped after a header row.
- **SG - Dropped where it is already marked: nothing is written, and the foot says so.** The words
  are "already prepared in Scene's header", through the refusal line S5 made, so a drop that
  changed nothing is not read as one that failed.
- **SH - No validate warning for a media cue in a header.** A header's media is armed at the park
  and launched by the GO. That is §3.12's prepare and commit extended to a header ("preload media
  ... then GO commits only the perceptible part"), so a warning would call a working show wrong.
  It is left for the author: their own show still has media cues moved into headers before this
  stage, and those play at the group's GO until each is dragged back to the body and marked.
