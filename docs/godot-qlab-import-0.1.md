# Importing a QLab show over OSC

**Draft 0.1**, 2026-10-01. An idea written down, not a phase. Nothing here is scheduled, and
nothing here amends the PRD.

*Note, 2026-10-05:* the first importer actually built is Ableton Live's, and it lives **inside**
Go.dot, at the author's direction (namespace draft §29, decision QC). A Live set is a file Go.dot can
read on its own; this draft's case for an external tool rests on talking to a running QLab over
OSC, which still stands.

---

## 1. The idea in one paragraph

Don't read the QLab save file. Open the workspace in QLab and **ask QLab what is in it**, over its
public OSC API. A small external tool walks the cue lists, reads each cue's properties, and writes
a Go.dot show bundle that validates against `docs/schema/show.rng`. That's exactly the kind of
tool PRD §3.20 says the document format exists for: *"With canonical XML, stable IDs and a
published schema those are external tools in any language and need **no API surface at all**."*
Go.dot itself gains no import code.

## 2. Why OSC, and not the file or the clipboard

| Route | What you get | Why not |
|---|---|---|
| **The `.qlab4` / `.qlab5` file** | Everything, in principle | An undocumented Apple archive (a keyed binary plist, as far as we know; not yet checked for QLab 5). Reverse-engineering it means chasing a private object graph that Figure 53 may change in any point release. |
| **Copy-paste** | Cue numbers and names, roughly | The clipboard mostly holds QLab's private type, which is meant for pasting back into QLab. Any plain-text version is far too thin to rebuild a show from. |
| **OSC, against a running QLab** | Every property the OSC dictionary exposes | Needs QLab running with the workspace open. As far as we know, opening a workspace doesn't need a licence. Whether OSC reads work without one is **to verify** (§7). |

OSC wins because the API is **documented and versioned**: it's the surface Figure 53 commits to
keeping stable, because their users' own tools depend on it.

## 3. Shape of the tool

```
qlab2godot  --host 127.0.0.1  [--passcode NNNN]  --out MyShow.godot/
```

- **External, standalone, any language.** Python with `python-osc` is the obvious first cut. It
  never links against Go.dot.
- **Read-only against QLab.** It sends queries, never edits, never fires a cue. A tool that can
  GO a show by accident must not exist.
- **Two stages, with the dump kept as a file:**
  1. **Dump.** Walk the workspace and write everything QLab answered to a single JSON file,
     unchanged. No interpretation at this stage.
  2. **Translate.** JSON → Go.dot XML, plus an import report (§6).

  The split means the translation can be rerun and improved without QLab on the machine. A dump
  from a real show also becomes a test fixture, so the importer gets regression tests the same
  way the phases do.

### The walk

The address forms below are from memory of the QLab 4/5 OSC dictionary. **Check every one against
the QLab 5 documentation before writing code.**

- `/workspaces` lists the open workspaces; `/workspace/{id}/connect [passcode]` connects to one.
- `/workspace/{id}/cueLists` returns the cue lists, with nested children, as JSON on
  `/reply/...` (port 53001 when talking over UDP).
- Per cue, `/cue_id/{uniqueID}/valuesForKeys ["..."]` fetches many properties in one round trip,
  instead of one query per property.
- Over TCP (port 53000, SLIP-framed), large replies arrive whole. Prefer it to UDP for the dump.

### Properties worth asking for

Common to all cues: `uniqueID`, `type`, `number`, `name`, `notes`, `armed`, `flagged`,
`colorName`, `preWait`, `postWait`, `duration`, `continueMode`.

By type:
- **Audio:** `fileTarget`, start and end time, loop count, rate, and the level matrix (main and
  crosspoints, per `sliderLevel` / `levels`).
- **Fade:** `cueTargetId`, duration, curve shape, `stopTargetWhenDone`, which rows and columns
  are active, absolute or relative.
- **Stop and the other control cues:** `cueTargetId`, fade-and-stop duration.
- **Group:** `mode`, and the playlist's shuffle and loop settings.
- **Network and MIDI:** the message as written, and its destination patch.
- **Workspace:** the audio output patches and their channel counts (needed to build `<Audio>` /
  `<Bus>`), and the minimum-volume setting (§5).

## 4. Mapping onto Go.dot

The schema already has the receiving elements: `<List>`, `<Cue>`, `<Group>`, `<Media>`,
`<Route>`, `<Fade>`, `<Stop>`, `<Osc>`, `<Midi>`, `<Bus>`.

| QLab | Go.dot | Notes |
|---|---|---|
| Cue list | `<List>` | A cue list is a manual sequence group (§3.6). |
| `uniqueID` | stable ID | Keep the QLab ID in an import note. All targeting goes through stable IDs, never cue numbers (§3.8). |
| Number, name, notes | same | |
| Colour | dropped, or kept as a tag | Constraint 8: colour is never the only carrier of information. |
| Audio cue | `<Cue>` + `<Media>` | |
| Level matrix | `<Route>` to `<Bus>` | §3.9b keeps the QLab-style matrix as-is, so this is the cleanest part of the mapping. |
| Fade cue | `<Fade>` | A QLab fade is a degenerate lane (§3.10): two breakpoints and a shape. |
| Stop cue | `<Stop>`, hard stop or fade-and-stop | Targets resolve to stable IDs. |
| Memo cue | memo | |
| Network cue | `<Osc>`, wait `none` | QLab's wait is fire-and-forget. Go.dot can't add `verified` without knowing what to read back, so it imports as `none`. The report lists these cues as candidates to upgrade by hand. |
| MIDI cue | `<Midi>` | Go.dot carries every MIDI event type (§3.10), so nothing is lost. |
| Group, *timeline* mode | timeline group | |
| Group, *playlist* mode | sequence group, auto | Shuffle maps to `shuffle`. Loop maps to infinite iterations. |
| Group, *start random* mode | sequence group, `shuffle`, `play 1 of M` | Close, but not the same: QLab draws one member per GO, Go.dot materialises a round. **To verify** whether the difference is audible in practice. |
| Group, *start first and enter* / *start first and go to next* | sequence group | Each member's continue mode decides auto or manual (below). Flagged for review. |

### The hard part: continue modes

PRD §3.6: **"No auto-follow, no auto-continue. One GO equals one row."** In QLab, a chain is a
run of flat sibling rows linked by continue modes. In Go.dot, the same chain has to become a
group. The translator therefore collects each **chain**: a cue plus every following sibling
reached by a continue mode. It then wraps the chain:

- **Every link auto-follow** (the next cue starts when this one ends) → **sequence group, auto**.
  Post-waits carry over as they are.
- **Every link auto-continue** (the next cue starts this cue's pre-wait + post-wait after this
  cue was fired) → **timeline group**. Each member's pre-wait is its computed offset from the
  start of the chain.
- **Mixed** → a timeline group holding nested sequence groups, split at the mode changes. This
  is the case most likely to be wrong, so it's always flagged.

The group gets a generated name, `Chain from <first cue's number and name>`, and the report says
the group was created by the importer.

QLab's Q-numbers still sit on the rows. The new groups don't have one, which keeps the operator's
GO numbering intact: one GO in QLab is still one GO in Go.dot.

## 5. What does not come over, or comes over changed

- **Inherited settings.** Constraint 12: nothing inherits downward. Anything a QLab cue takes
  from workspace or group defaults gets written onto the cue explicitly. This is more verbose,
  and more honest.
- **Silence floor.** QLab's minimum volume, −60 dB by default and configurable, means silence.
  Levels at or below the workspace floor import as −∞. Levels above it import as they are.
- **Destination widths.** §3.9b: widths are explicit. Output patches become buses with declared
  widths. Nothing becomes a slot: QLab has no concept of one, and §3.9b says "no auto-assignment,
  ever". Assigning slots is the designer's job after import.
- **Start, Load, Pause, Reset, Devamp, Goto and Arm/Disarm cues.** These have no direct
  equivalent yet. They import as memos carrying the original intent, and they're flagged.
- **Script cues and AppleScript.** These import as **inert memos with the source text kept**,
  never as anything that runs. §3.20: loading a show must never execute it.
- **Video cues.** The file target and timing come over. Geometry, surfaces and effects wait for
  §3.19 to exist in the schema.
- **Light, camera and text cues.** Listed in the report, not imported.
- **Media paths.** `fileTarget` is an absolute path on the QLab machine. The tool copies or
  relinks media into the bundle, and lists anything missing.

## 6. The import report

The report is written next to the bundle, in Markdown. It is as much the product as the XML is.

- A count by cue type: imported, approximated, or dropped.
- Every flagged row, with its QLab number, its uniqueID, and the reason for the flag.
- Every group the importer invented (§4).
- Every network cue that could be upgraded to `verified`.
- Missing media.

Rule: the import never hides an approximation. A show that imports silently and then behaves
differently in tech is worse than a show that doesn't import at all.

## 7. First step, and what it settles

A **dump-only** script, roughly 100 lines, run against a real workspace. That one script answers
three questions:

1. Does the OSC dictionary expose enough of a real show to be worth translating? Of the
   properties in §3, which ones are actually readable?
2. Do OSC reads work on an unlicensed QLab?
3. As a side effect: it's a person at a machine with QLab on it, which is what
   `docs/godot-open-questions-0.1.md` §1 has been waiting for. Some of those checks could be
   scripted in the same session.

The translator should only be written after the dump has been seen.
