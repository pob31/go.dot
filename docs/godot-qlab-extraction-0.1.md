# Extracting a QLab show over OSC: what was measured

**Draft 0.1**, 2026-10-08. The first run of the dump that `docs/godot-qlab-import-0.1.md` §7 asked
for, against a real show. This page documents **extraction** (how to get the data out of QLab)
and **parsing** (what the data means). It specifies no Go.dot code: translation into a show bundle
comes later, in the import draft.

---

## 1. The test

- **QLab 5.6.3**, macOS, the workspace open, no OSC passcode.
- **A real show**, used on a copy: three cue lists, 514 cues. Live music with a pit
  orchestra, sound design, remote control of a **DiGiCo S21** console over OSC, and a **WFS-DIY**
  rig (the Max version) over OSC.
- A throwaway Python client (standard library only), **read-only**: it sends queries and nothing
  else.

Result: **the whole workspace came out in 0.61 s, with no errors**. The OSC interface exposes
enough of a real show to translate. §8 lists what it doesn't expose.

Cue counts, which say a lot about what a real show with external control looks like:

| Type | Count | | Type | Count |
|---|---|---|---|---|
| Network | 348 | | Memo | 7 |
| Group | 126 | | Mic | 3 |
| Fade | 12 | | Start | 3 |
| Audio | 10 | | Stop | 2 |
| Cue List | 3 | | | |

**Two thirds of the cues are OSC messages to other machines**: 206 to the console, 142 to WFS.
Only 10 cues play a file.

## 2. Transport

- **TCP on port 53000, SLIP-framed (OSC 1.1).** A frame is `0xC0 … 0xC0`, with `0xDB` escaping
  `0xC0` and `0xDB`. Use TCP, not UDP. The full `cueLists` reply is **131 KB in one message**,
  which won't fit in a UDP datagram. TCP delivers it whole.
- **Every reply comes back on `/reply/<the address asked>`** with one string argument: JSON in
  the form `{"status": "ok"|"error", "data": …, "address": …, "workspace_id": …}`. An unknown or
  inapplicable method returns `"status": "error"` with no further detail.
- **QLab also sends `/update/...` messages unprompted.** The reader skips anything that isn't a
  `/reply`.
- **Connecting:**
  1. `/workspaces` returns `[{uniqueID, displayName, version, port, udpReplyPort}]`.
  2. `/workspace/{id}/connect` returns `"ok:view|edit|control"` with no passcode.
  3. Every later query is prefixed with `/workspace/{id}`.
- **Don't send any message that can change the show.** Every `/cue/...` method is both a read
  and a write, depending on whether it gets an argument. A query must **never carry an
  argument**, except `valuesForKeys`, whose argument is the list of keys. `/go`, `/start` and
  `/panic` must not exist in the tool.

## 3. The walk

Four steps, three kinds of message:

1. **`/workspace/{id}/cueLists`**: the whole tree in one reply. Each node has only `uniqueID`,
   `type`, `number`, `name`, `listName`, `armed`, `flagged`, `colorName`, `colorName/live`, and
   `cues` (its children, in order). The order of `cues` *is* the cue list order.
2. **`/workspace/{id}/cue_id/{uniqueID}/valuesForKeys "[\"key\", …]"`**: one round trip per
   cue, with the key list chosen by type (§4). Address cues **by `cue_id`, never by number**:
   only 4 of the 514 cues in this show have a number.
3. **Workspace settings:** `/settings/audio/patchList`, `/settings/audio/cueOutputChannelCounts`,
   `/settings/audio/outputChannelNames`, `/settings/audio/minVolume`, `/settings/audio/maxVolume`,
   `/settings/network/patchList`, `/settings/mic/patchList`, `/settings/midi/patchList`.
4. **`/version`** goes in the dump too. The parser needs to know which dictionary it is reading.

The dump is the tree from step 1 with each node's `valuesForKeys` result attached under its own
key, alongside the settings, unchanged. It's 1 MB for this show, mostly from level matrices
(§5.2).

**`valuesForKeys` silently leaves out keys it can't answer** instead of failing the request.
`gang` and `sliceMarkers` came back missing on audio cues, and asking for them on their own
returned an error. The parser must therefore treat **every key as optional**, and a missing key
means "unknown", not "false".

## 4. Keys per cue type, as verified

Every key below was **read back successfully** from QLab 5.6.3. Keys that don't apply to a cue
type come back `null` or are left out.

**All cues:** `uniqueID`, `type`, `number`, `name`, `displayName`, `listName`, `notes`, `armed`,
`flagged`, `colorName`, `secondColorName`, `useSecondColor`, `preWait`, `postWait`, `duration`,
`currentDuration`, `continueMode`, `isBroken`, `isWarning`, `skipIfDisarmed`, `autoLoad`,
`secondTriggerAction`, `secondTriggerOnRelease`, `timecodeTrigger`, `parent`.

| Type | Keys |
|---|---|
| Group, Cue List | `mode`, `playlist/doLoop`, `playlist/doShuffle`, `playlist/doCrossfade`, `playlist/crossfade/duration` |
| Audio | `fileTarget`, `startTime`, `endTime`, `playCount`, `infiniteLoop`, `lastSliceInfiniteLoop`, `lastSlicePlayCount`, `rate`, `preservePitch`, `audioOutputPatchID`, `audioOutputPatchName`, `doFade`, `fadeAndStopOthers`, `fadeAndStopOthersTime`, `duckOthers`, `duckLevel`, `duckTime`, `levels` |
| Mic | `audioInputPatchID`, `audioInputPatchName`, `audioOutputPatchID`, `audioOutputPatchName`, `fadeAndStopOthers`, `fadeAndStopOthersTime`, `levels` |
| Fade | `cueTargetID`, `cueTargetNumber`, `levelsMode`, `stopTargetWhenDone`, `doLevel`, `levels`, `doRate`, `rate` |
| Start, Stop | `cueTargetID`, `cueTargetNumber`; Stop also has `targetMode` |
| Network | `networkPatchID`, `networkPatchName`, `networkPatchNumber`, `customString`, `parameterValues`, `fadeType`, `fadeFrom`, `fadeTo`, `fadeEntries`, `fadeNumberType` |

**Spelling:** the dictionary and `valuesForKeys` say `cueTargetID`, with a capital D. An earlier
probe also got an answer under `cueTargetId`. Use the dictionary's spelling.

### Enumerations (from the QLab 5 OSC dictionary, confirmed by the values seen)

- **`continueMode`:** 0 no continue · 1 auto-continue · 2 auto-follow.
- **`mode`** (Group, Cue List): 0 list *(read-only, cue lists)* · 1 start first and enter · 2 start
  first · 3 timeline · 4 start random · 5 cart *(read-only)* · 6 playlist.
- **`levelsMode`** (Fade): 0 absolute · 1 relative.
- **`fadeType`** (Network): 0 none · 1 1D curve · 2 2D path.
- **`fadeNumberType`** (Network fade): 0 integers · 1 floats. *(It came back as `true` here,
  which is the 1 of a boolean read.)*
- **`fadeAndStopOthers`:** 0 none · 1 peers · 2 list or cart · 3 all. *(Booleans `false`/`true`
  were also seen, meaning 0/1.)*
- **`secondTriggerAction`:** 0 nothing · 1 panic · 2 stop · 3 hard stop · 4 hard stop and restart ·
  5 devamp · 6/7 playlist next/previous.
- **`targetMode`** (Stop): 0 cue target · 1 patch target.

**A boolean may come back as `true`/`false` or as `1`/`0`**, even on the same key across cues
(`infiniteLoop`, `preservePitch`, `fadeAndStopOthers` all did this). Normalise on read.

## 5. Parsing rules

### 5.1 Names and identity

- **`name` is empty whenever the user never typed one.** That was 12 of 12 fades and 10 of 10
  audio cues here. Use **`displayName`** for display: it's QLab's own generated label
  (`"fade ambiances_19.WAV"`, `"ambiances_19.WAV"`). Keep `name` as well, so the import can tell
  a name somebody chose from one QLab generated (constraint 10).
- **`number` is almost always empty.** Identity is `uniqueID`, a UUID that stays the same across
  saves. All targets resolve through `cueTargetID`; `cueTargetNumber` was empty on every fade.
- **`notes` carries real information.** 72 cues have notes. They hold **the stage manager's cue
  lines** ("When house lights go down"), **the reasons behind cues**, and **patch maps written as
  prose**. The group that sets up the console has the console's control-group numbering in its
  notes. The WFS group has the WFS input ↔ console channel map in its notes. Notes must come
  over word for word, and they're the first thing to show to whoever reviews the import.

### 5.2 Level matrices

- **`levels` is a matrix of rows, in dB.** Row 0 holds the output levels: column 0 is the cue's
  main level, columns 1…128 are the per-output faders. Rows 1…n are the input channels of the
  file or mic, and each one's columns are its crosspoints to each output. A stereo file has
  3 rows; every row has 129 columns.
- **Trim the columns to the patch's `cueOutputChannels`** (from `/settings/audio/patchList`):
  20 here, not 128. Columns beyond that have no meaning.
- **The silence floor is the workspace's `minVolume`, −80 dB here, not the −60 dB default** the
  import draft assumed. Read it from the dump, never assume it. A value at or below the floor is
  −∞.
- **On a Fade cue, `levels` means nothing on its own.** Only the cells that **`doLevel`** marks
  `true` are faded; every other cell is left alone. Every one of the 12 fades in this show had
  exactly one active cell, `(0,0)`, the main level. A fade imports as *"target's main level → X
  dB over D seconds"*, plus `stopTargetWhenDone`. `willFade` is the pre-5.5 name for `doLevel`
  and is deprecated.
- **A relative fade** (`levelsMode` 1) holds a dB offset in the same cells, not a target level.
  This show uses 4.

### 5.3 OSC messages in Network cues

`customString` holds the message **as QLab's own text syntax**, and `parameterValues` holds the
same thing as a one-item array. Rules (QLab 5 *Network cues* page):

- **Separator:** one space between the address and each argument. A string containing spaces is
  wrapped in double quotes.
- **Types:** digits only → **int32**; digits with a decimal point → **float32** (`0.` is a
  float); `\T`/`\F` → **true/false**; `\I` → impulse; `\N` → nil; anything else → string.
- **The decimal point is a dot in the stored string.** QLab converts a comma on output. The
  parser must be locale-independent anyway (the `fr_FR` rule).
- **Fades in Network cues** (`fadeType` 1): the message contains **`#v#`**, which is replaced by
  the value on each send. It can appear in the address as well as in an argument. The value runs
  `fadeFrom` → `fadeTo` over `duration`, along `fadeEntries` (`[{x: seconds, y: value}, …]`; all
  3 here were straight two-point ramps). 2D fades use `#x#` and `#y#`.

What this show sent, by address pattern, with arguments typed as above (283 floats, 126
booleans, 103 ints, 3 placeholders):

| Patch | Pattern | Count |
|---|---|---|
| S21 | `/channel/N/mute \T\|\F` | 120 |
| S21 | `/channel/N/fader <dB float>` | 77 |
| S21 | `/channel/N/eq/lowpass/enabled`, `/channel/N/send/N/level` | 9 |
| WFS | `/wfs/input/N/positionXYZ x y z` | 60 |
| WFS | `muteMacro`, `FRactive`, `commonAtten`, `constraintXYZ`, `LFOactive`, `LFOperiod`, `lfo/xyz`, `latency`, `curvature`, … | 77 |
| WFS | `/wfs/input/all/...` | 5 |

**The WFS addresses are WFS-DIY Max's**, not necessarily today's WFS-DIY namespace. Whether
they map one-to-one is a separate question, for whoever owns that namespace. The
`/wfs/input/N/lfo/xyz` message packs nine positional numbers, and the show's author decoded them
by hand in each cue's notes ("X Shape: Random, Y Shape: Sine, …"). That's the strongest argument
in this show for OSCQuery-described parameters over raw strings.

### 5.4 Audio cues

- **`fileTarget` is an absolute path** on the QLab machine.
- **`rate` is used as a creative tool**, not left at 1. The machinery ambiences play at
  0.50–0.60. **`preservePitch` is mixed** (both 0 and 1 in the same group), and that's a
  decision someone made. Go.dot needs an answer for varispeed, with and without pitch
  correction, before these cues can import faithfully.
- **Looping:** `infiniteLoop` plus `playCount` (1–3 here), between `startTime` and `endTime`.
  Slices exist but couldn't be read (§3).
- **`duration` is the effective length after rate and loops**, not the file's length.

### 5.5 What `isBroken` means

`isBroken` was true on every Audio and Mic cue and on the groups holding them, because **the
S21's MADI interface wasn't connected** on the machine doing the dump (the patch names end in
"(disconnected)"). It describes **the machine at dump time, not the show**, so it is **never
imported** (constraint 10). It *is* worth one line in the report, as "QLab could not play these
here".

## 6. What a real show's structure looks like

This is the most useful thing the dump showed. The cue list isn't flat:

```
<show>                               Cue List (mode 0)
└─ 1 - <scene name>                  Group, start first and enter (1), auto-continue   ×42
   ├─ <operator step>                Group, playlist (6)                               ×77
   │  ├─ Network  /channel/118/mute \F        auto-continue
   │  ├─ Network  /wfs/input/21/positionXYZ…  auto-continue
   │  └─ Network  /wfs/input/23/LFOactive 1   (last: no continue)
   ├─ <operator step>                Group, playlist
   │  └─ Group, timeline (3)          ← simultaneous audio + fades
   └─ …
```

- **Scenes are "start first and enter" groups.** A scene's notes hold its stage-manager cue line.
- **Each operator GO is a playlist group** whose children are **all auto-continued except the
  last**. That's 71 of the 77 playlist groups. None of them loops, shuffles or crossfades. The
  author is using *playlist* to mean **"fire this block of settings in order, now"**, not to mean
  "playlist".
- **Timeline groups appear only to start audio together with its fades** (4 of 5).
- **Pre-wait is never used, and post-wait only 3 times.**

The import draft (§4) had chains of auto-continued **siblings** as the hard case. In this show,
those chains are **already wrapped in a group by the author**. For a playlist group of
auto-continued instantaneous cues, Go.dot's **sequence group, auto** is a literal translation:
each OSC cue is done when sent, so the chain completes in order on one GO. It also gains
something QLab doesn't have: such a chain can wait for `verified` (§3.11) when that's worth it.

## 7. Questions the dump leaves open

1. **Auto-continue on the scene groups.** 41 of 42 scene groups have `continueMode` 1. In a "start
   first and enter" group, does that make QLab fire the next scene too, or is it inert once the
   playhead enters? This is the author's knowledge of QLab and of this show, not something the
   dump can tell us.
2. **Does OSC reading work on an unlicensed QLab?** Still open: this dump ran on a machine whose
   licence state wasn't checked.
3. **Network patch destinations aren't exposed.** `/settings/network/patchList` returns only
   names and IDs (`"OSC Message - S21"`). The host, port and protocol behind a patch can't be read
   over OSC, at least under any address the dictionary documents. The import has to ask the user
   for them, or match the patch name against the devices Go.dot already knows.
4. **Audio fade curve shape.** Not exposed for Fade cues as far as the dictionary shows
   (`fadeEntries` is the Network-cue curve). Treat fades as QLab's default shape, and flag them.
5. **Slices.** `sliceMarkers` errored on these cues. That may just be because there are no
   slices, which hasn't been checked on a cue that has some.
6. **Mic cues** are live inputs with reverb tails, started and stopped from another cue list by
   Start/Stop cues aimed at them. Their counterpart in Go.dot is the live rack (§3.18). Mapping
   them is a design question, not a parsing one.

## 8. What extraction cannot give, summed up

- network patch destinations (host, port, protocol)
- audio fade curve shapes
- slice markers (unconfirmed)
- `gang`
- anything about the media beyond its path: the file has to be read from disk, or found missing
  there

Everything else this show uses came out.
