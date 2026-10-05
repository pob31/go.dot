# Bug round of 2026-10-05: S7, as built

Written on branch `bugs-ui` for the integrator to append to `docs/godot-namespace-draft-0.1.md`
§30, under the next free number. The window-side stages do not edit the draft, because it would
conflict across branches. Everything below the rule is the subsection itself.

---

### 30.N What was built: S7 - importing media

The triage held. The author's WAVs are ordinary 24-bit PCM, and the non-ASCII "Danse en Chœur +
C10.wav" imported fine. Two files that no cue names sit in their show's `media/`, left by a
session of 4 October that was never saved. Dropping either again was refused, because a name
already in `media/` was refused outright (`copyIn`, `replaceExisting` always false from an
import). The operator was told "could not copy X into the show", the same sentence as for three
other causes. The copy also ran on the message thread, and the import's patience was counted in
the window's passes.

**A name already in `media/`** is decided by content. The sizes are compared first, and the
bytes only when the sizes agree. The same file is used silently, under the name the disk has,
and the cue is made. Another file of that name brings up a question: *"Another X.wav is in the
show"*, *"The show already has a file called X.wav, and it is not this one."* It names the cues
that play that file and says that replacing it changes them too, and it gives the name Keep both
would use. The buttons are **Keep both** (Return), **Replace it**, and **Use the one in the show**
(Escape). These are the stage brief's words, not yet put to the author. Keep both copies the
file in as "X 2.wav", the first free number. Names are compared
case-blind everywhere (`model::sameFileName`), because Windows and macOS keep "x.WAV" and "X.wav"
as one file and a show travels between machines. Letters are folded through Latin-1 and Latin
Extended-A, and past that the disk's own answer is asked as well. The helpers are
`model::freeName` and `model::nameAmong`. A file dropped on a media cue (`linkMedia`) asks its
own question first, as before, when the cue already plays something. The name question is then
the import's, so the two gestures cannot come to mean different things.

**Each failure says its own cause**:
- "X has nowhere to go: the show has no folder yet"
- "X could not be found"
- "X could not be read"
- "X could not be copied into the show: <the system's words>", for example a full disk, or "the
  one in the show could not be replaced - it may be in use"
- "X is in the show, but the cue for it was refused"

Each is said as it happens. The last word of the import says the first one again, with a count:
"imported 7 of 8 files - X could not be found (and 1 more)". When nothing failed, the last word
is "imported 3 files (1 already in the show)", "imported X.wav as X 2.wav", or "X.wav is on the
cue". The show's own warnings follow it: no audio tracks, no direct out.

**The bytes are copied off the window's thread.** `ui::MediaCopier` is one worker thread, owned
by the window and stopped and joined in its destructor. It is handed one file at a time by
`model::MediaImports`, which holds every decision and is what the tests hold. The bytes go into a
hidden part-file beside the target and are moved over it whole (`juce::TemporaryFile`). So a full
disk, a stop, or a Replace that cannot write over a file in use leaves `media/` as it was. While
a file copies, the foot says "Copying 3 of 8: Thunder.wav", once per file, so anything else said
meanwhile stays readable. Each cue is asked for on the message thread when its file has landed.
A second import queues behind the first. Nothing in `media/` is ever deleted.

**The patience is counted in the engine's ticks**: `importPatienceTicks`, 250, five seconds of
the engine's clock. Before, it was counted in the window's passes.

The two stale comments in `engine/audio/MediaInfo.h` (`publish`) and `MediaInfo.cpp` now say
that `durations()` learns a file imported mid-session. It has done so since 2026-09-22.

Tests:
- ClientTests, both locales:
  - `client: a name already in media/ is compared as a case-blind disk compares it, and the free one is the first number`
    covers the ladder, names with dots, no extension, case, the author's "Chœur", É/é, Ÿ/ÿ and
    Ž/ž, and that two different letters stay different.
  - `client: the same file is used silently, and its bytes are read only when its size agrees`
    counts the reads.
  - `client: an import looks at every file before copying any, and makes its cues in the order picked, each after the one before`
    runs on a real engine. A stranger is inserted from the page while the files copy, and the
    case checks the final order.
  - `client: an import waits for its cue in the engine's ticks, not the window's passes, and a second waits behind it`
    runs a thousand passes on an unmoved tree, then lets the engine's clock run past the
    patience.
  - `client: another file of the same name is asked about, one at a time, and an answer can stand for the rest`
    covers the question's flow and its words.
  - `client: each way a file is not imported says its own cause, and the last word says them again`
    covers the failure sentences, the endings, Keep both's cue name, and the link.
  - `client: the question names the cues that play the file a replace would change`
- wfg_audio_ui_tests, both locales:
  - `media copier: a look says what a picked file meets in media/, and reads bytes only to tell the same from another`
  - `media copier: a copy lands whole under its name, Keep both takes the first free number, and Replace writes over`
  - `media copier: a copy stopped part of the way leaves nothing behind, and says it did not land`

  These three run on real files in a folder of their own.

The implementer's calls:

- **SI - Every picked file is looked at before any is copied.** A look is a name and a size, and
  the bytes only when a size agrees. Every question about a name therefore comes within moments
  of the drop, never three minutes into a copy, when a hand may be on GO and a box would take
  the keys. The one exception is two files of one name in one drop. Both are free when looked
  at, so the second is asked about when its copy finds the first already there.
- **SJ - One question per file, with "The same for the other files whose names are taken" while
  more of the same import may meet a name.** A re-rendered set of eight would otherwise be eight
  boxes. Return is Keep both, because it loses nothing. Escape is Use the one in the show,
  because it changes nothing. Copies of files that met nothing go on while the box is up.
- **SK - Compared byte for byte, not hashed.** Both files are on this machine, so a digest has
  nowhere to travel. A compare stops at the first difference, which in two renders of one sound
  is usually inside the first megabyte, while a hash reads both files to the end.
- **SM - Patience in the engine's ticks, not its revisions and not the wall clock.** A create is
  applied on the first tick after it is sent. A refused create moves no revision, so a lone
  refusal would wait until somebody edited something else. A wall clock gives up on an engine
  that is only busy. The engine's clock stops when it stops working, and runs on past a refusal.
  The new-cue row's creates, groupings and moves into a section still count passes
  (`importPatience`), as before.
- **SN - Each cue is placed after the one before it, once that one is seen.** The drop's position
  is turned into the member the hand let go after, so a long copy cannot move it. Before, it was
  a number fixed at the drop, and a cue inserted above during a copy left every later file
  looking at a stranger. Only one create is in flight at a time, which keeps the cues in the
  order picked across imports. A cue made under Keep both is named after the file it plays
  ("X 2"). A link takes the same road with no create.

Known limits of Replace, left as they are:
- On Windows, a file the engine has open cannot be written over. The copy then fails with "the
  one in the show could not be replaced - it may be in use", and the old file stays.
- A file the show named when it was opened keeps the length frozen then (`MediaInfo`, the law in
  §14.12). A replaced file of another length is therefore read at its old length until the show
  is reopened.
- Other cues' `channels` are not re-read.

Not built, for the author: a "Remove unused media…" command. It would clear what an unsaved
session strands in `media/`, as it stranded MUT C2-C3.wav and MUT C3-C4.wav.
