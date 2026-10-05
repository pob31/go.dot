# Bug round of 2026-10-05: S5, as built

Written on branch `bugs-ui` for the integrator to append to `docs/godot-namespace-draft-0.1.md`
§30, under the next free number. The window-side stages do not edit the draft, because it would
conflict across branches. Everything below the rule is the subsection itself.

---

### 30.N What was built: S5 - the persistent band

The list's persistent band is now drawn at the top of every list, where it stands when it has
cues, whether or not the list has a `<Persistent>` section yet. Empty, it is drawn light, with the
words *"drop media, mic, OSC or MIDI cues here to keep them running all show"* where a count would
be (`model::emptyBandWords`). It folds as before, through `list/persistentFolded`, which a list
takes with no section behind it. A list the tree does not have still draws nothing. The drag that
grows a group's empty header and footer bands no longer sweeps the persistent band away with them.

A drop on the band moves the cue into the section when the section exists. When it does not, the
drop is the new `DropKind::persistent`, naming the list. Before this, such a drop fell through to
`DropKind::footer` on the list's id, which the engine refuses. The window answers it with
`moveIntoPersistent`: `list.persistent <list>` first (the new `gesture::listPersistent`), then the
`object.move` on the pass whose tree names the section. This uses the queue a header or footer
made on the way already uses (`finishFooterMoves`, which now reads a list's section at
`/godot/list/<id>/persistent`). Its sentences are "making the persistent section", "into the
persistent section" and "the persistent section was refused". While the hand is in the air, the
band says "into the persistent section" in both cases. It used to say "into persistent" over a
section that existed.

What may go in is asked first (`model::persists`): media, mic, OSC and MIDI. Anything else is
refused, on the band and after any of the section's rows, with the reason in words:
*"only media, mic, OSC and MIDI cues can be persistent, so <name> stays where it is"*
(`Drop::refused`, which `describe` returns for a `none`). Nothing is lit for a refused drop, so
the sentence is the whole answer.

Tests (ClientTests, both locales):
- `client: a list with no persistent section still draws its band, empty, and the band makes the section`
  checks the band on `minimal` (top row, empty, no section id, its words). It checks the fold and the
  flag, then drives the two-pass shape through the real commands: the drop asks for the section,
  `list.persistent` makes it, the band names it, the move lands, and the band becomes an ordinary one.
  It also checks that asking twice answers with the same section.
- `client: the persistent band takes what the section keeps running, and refuses the rest in words`
  checks each kind on the band, with and without the section and after a section row, and that a
  group's empty header is untouched by the rule.
- `gesture::listPersistent` was added to the gestures checked against the real registry.

The first case was seen failing (no band) with the always-drawn band switched off.

- **RZ - The window follows the engine's list, not the brief's: mic goes in, memo stays out.**
  Decision S named media, OSC and MIDI. `solvePersistent` has re-asserted mic cues since Phase 9b,
  and validate's warning names those four. A memo is refused although validate does not warn about
  one, because the section asserts nothing from it either.
- **SA - "Thin" is drawn, not measured.** JUCE's list box gives every row one height, and a shorter
  row would mean replacing the list. The empty band is a row tall but drawn light: the plain ground
  of a cue row, the top rule with no rail dropping into rows that are not there, the word in the
  dimmer ink, and the sentence in place of a count.
- **SB - A cue already in the section can be reordered there, whatever its kind.** Moving it changes
  nothing about what the section asserts. Refusing it would leave a stray fade (a fixture's, or a
  file from before this) movable only out of the section.
- **SC - A refused drop is said in the air and said again after letting go.** The foot is cleared
  when the hand lets go, and a cue that did not move with nothing at the foot reads as a drop that
  failed.
