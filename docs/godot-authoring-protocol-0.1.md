# Writing cues into Go.dot from a processor

**Draft 0.1**, 2026-10-10. The contract WFS-DIY and S21_HiJack implement to create, update and
fire Go.dot cues, and that Go.dot implements to answer them. It replaces, for these two apps,
the QLab cue-writing exchange both use today. PRD §3.26 is the reason for it; namespace draft
§56 records the decisions and the stages. Where this page and the code disagree, the code is
being wrong and one of them gets fixed.

The author decided eight things on 2026-10-10 (§56.2, AEI-AEP): Go.dot answers a capture with
one message; a processor may declare itself as a device; a re-export updates the cue it made
before; recall stays fire-and-forget; both processors describe themselves and Go.dot offers
their commands as cascaded menus; Go.dot gains a fire-by-number verb; S21_HiJack drives Go.dot
through per-cue triggers made from templates; and the menus follow QLab's shape, one drop-down
per part of the path. **The names on this page are mine** (`mount.declare`, `cue.capture`,
`mount.described`, `cue.fireNumber`, `standby.setNumber`, the three answer addresses), until the
author renames them.

---

## 1. Why not QLab's way

QLab's API is positional. A client sends `/new "network"`, then sets the *selected* cue's patch,
text and name, asks the selected cue for its `uniqueID`, waits for a JSON reply on port 53001,
and moves the cue into a group with `/move/<uid>`. WFS-DIY and S21_HiJack both pace this at
30 ms a message with a 2 s timeout and no retry, about 0.2 s a cue. A snapshot of 275 parameters
is 275 QLab cues in a playlist group, because a QLab network cue holds one message.

Go.dot owns both ends here, so the exchange is one message out and one message back:
- **One capture is one command.** The cue, its name, its number and all of its messages arrive
  together and land as one undo step.
- **The processor names the cue.** It draws the identifier, keeps it beside its snapshot, and
  sends it again on the next export, which updates the same cue instead of making another.
- **A Go.dot OSC cue holds many messages** (namespace draft §45), sent in one tick and bundled
  where the device takes bundles. A snapshot is one cue.

## 2. Transport and spelling

- **OSC over UDP to Go.dot's port, 8010 by default**, or binary OSC over the OSCQuery WebSocket
  on 5010. Commands are addressed `/godot/cmd/<a>/<b>` for the command `a.b`
  (`EngineNamespace.cpp`). A bundle's elements are applied in order, in the same tick.
- **Identifiers are 8 characters of Crockford base32**, `[0-9A-HJKMNP-TV-Z]{8}`: no I, L, O or
  U. A processor draws its own from `0123456789ABCDEFGHJKMNPQRSTVWXYZ`.
- **Values are atom lists**, the spelling of an OSC cue's `value` row: `i:3`, `h:3`, `f:1.5`,
  `d:1.5`, `s:"text"`, `T`, `F`, `N`, `I`, space-separated, one per argument. Strings are always
  quoted, with `\\`, `\"` and `\n` escaped; an unquoted word refuses the whole list. `f:` reads
  as a double and narrows, so Rust's `{}` on an `f32` and JUCE's `String (double, 6)` both read
  back. An empty list sends a message with no argument.
- **Every string argument of a command is an OSC string**, `,s`. Go.dot refuses a number where a
  string is expected (`type-mismatch`). The two ports of `mount.declare` are `,i`.
- **A datagram stays under 1,200 bytes**, the family's ceiling (WFS-DIY's own bundles, Go.dot's
  `MountSender::bundleBytes`), so it is never fragmented on the way. A capture too long for one
  datagram is cut into chunks (§4.4).
- **Strict senders.** When Go.dot's OSC filter is "registered only", a datagram from a host that
  is not a declared device with Rx on is dropped before it is read, and nothing answers. Declare
  before the filter is switched on, or add the host in Go.dot's Network tab.

## 3. `mount.declare`: the processor becomes a device

```
/godot/cmd/mount/declare  ,siis[s][s]  prefix  port  queryPort  name  [id]  [host]
```

| Argument | Meaning |
|---|---|
| `prefix` | The processor's root: `/wfs`, `/s21`. One root. |
| `port` | Where the processor receives OSC: its cues are sent there, and so are Go.dot's answers. |
| `queryPort` | Where its OSCQuery server answers HTTP, or `0` if it describes nothing. |
| `name` | The device's name in Go.dot's Network tab. |
| `id` | Optional: the device identifier to create with. Empty or absent: Go.dot draws one. |
| `host` | Optional: the processor's address. Absent: the address the datagram came from. |

What Go.dot does, in one undo step:
- **A device already has this root** among its prefixes: its host, port and query port are
  written if they changed. Its name, Rx and Tx are left alone, so an operator's edits survive.
  Outcome `updated`.
- **No device has it**: one is made with the root, host, port, query port and name, Rx on, Tx on.
  Outcome `created`.
- **Refused** with `bad-value` for an empty root or name, a root under `/godot`, `/ui` or
  `/media`, or a port outside 1..65535 (the query port may be 0). A declare from a script with no
  host, or with a host that cannot be read, is `bad-value`.
- **Under the show lock**, only a declare that would edit the show is refused (`locked`): a new
  device, or one whose host or ports moved. A declare that changes nothing is answered `updated`
  and its description fetched again, so a snapshot stored during a locked show still reaches the
  menus.

The answer goes to `host:port`:

```
/godot/declared  ,ss  id  outcome          outcome: created | updated | <reason>
```

**Then the description, when `queryPort` is not 0.** Go.dot fetches `GET http://host:queryPort<prefix>`
off the tick thread (5 s), checks it as a namespace file, and keeps it in the show as
`namespaces/<id>.json`. The device becomes a *described* device: its nodes are typed and
checked, and Go.dot's OSC cue editor offers them as menus (§6). The fetch ends with:

```
/godot/described  ,sis  id  nodeCount  problem       problem empty when it loaded
```

- A failed fetch keeps the description Go.dot had before, and the device's problem cell says why.
- Under the show lock, a description is refreshed for a device that already names its file, and a
  device that had none stays without (`/godot/described <id> 0 locked`).
- **Declare again whenever the menu should change**: a snapshot stored, deleted or renamed, a
  macro added. Every declare fetches again.
- The record in Go.dot's log carries the host and the identifier as applied, so a replay needs
  no socket and draws nothing.

## 4. `cue.capture`: make or update a cue

```
/godot/cmd/cue/capture  ,sssssss[ss]...  where  target  id  name  number  notes  messageIds  [address value]...
```

### 4.1 Where it lands

| `where` | `target` | Lands |
|---|---|---|
| `standby` | ignored | After the standby cue of the focused list. No standby: at the end of that list. No focused list: the first list. No list at all: `unknown-id`. |
| `list` | a list id or name; empty = the focused list | At the end of that list. |
| `cue` | a cue id, or a cue number | Replaces that cue's content. It must be an OSC cue (`type-mismatch` otherwise). |
| `more` | the id of the cue a previous chunk made | Appends the pairs to it (§4.4). `name`, `number` and `notes` are ignored. |

**The identifier wins over `where`** (decision C). If a cue with `id` already exists, it is
updated in place, wherever it now sits, and `where` only matters when it does not. A processor
therefore always sends `standby` with the id it kept: the first export lands after the
standby, every later one updates that cue, and a cue deleted in Go.dot meanwhile is made again
after the standby.

### 4.2 The rest of the head

- `id`: the identifier to create with or to update. Empty: Go.dot draws one and the answer says
  which.
- `name`, `number`, `notes`: written when not empty, kept when empty. A new cue with an empty
  number has none.
- `messageIds`: always `""` from a processor. Go.dot writes the identifiers it drew for the
  further messages here in its log record, so a replay draws nothing.

### 4.3 The pairs

`address value` pairs, at least one (`bad-value` otherwise, and for an odd count or a value that
does not read as an atom list).
- The first pair is the cue's own address and value. The rest are its further messages, in
  order. An update replaces all of them.
- **Every address must sit under one declared device**: none is `unknown-id`, two is
  `bad-address`. The cue is an OSC cue aimed at that device.
- **Under a described device every address must be one of its nodes**, with the argument types
  it declares, or the cue fails at GO with `bad-address` or `type-mismatch`. A processor
  therefore captures exactly the addresses it describes.

### 4.4 Chunks

A capture longer than 1,200 bytes is sent as several datagrams, back to back:
- The first is the capture, with its `where` and the full head.
- Each following one is `where = more`, `target = <the same id>`, the other head fields empty
  (an `id` repeating the identifier is read the same way), and the next pairs.
- Go.dot applies them in arrival order. An update's first chunk replaces the messages and the
  `more` chunks add the rest.

### 4.5 The answer

```
/godot/captured  ,ssss  id  outcome  number  name
```

- `outcome` is `created`, `updated`, `appended` (a `more` chunk), or the refusal's reason:
  `locked`, `unknown-id`, `bad-address`, `bad-value`, `type-mismatch`.
- `number` and `name` are the cue's as they now stand; empty on a refusal.
- It goes to the declared device whose host is the sender's address, which is the processor
  that asked. When the sender is not a device, it goes to the device the cue is aimed at. When
  there is neither, nothing answers, and the refusal is in Go.dot's log and at
  `/godot/engine/lastError`. The device's Tx switch does not silence an answer.
- A processor waits 2 s for each answer it expects: one per datagram.

## 5. Firing Go.dot from a processor

Any client may send these; S21_HiJack's triggers and macro steps do (§7.3).

| Address | Arguments | Does |
|---|---|---|
| `/godot/cmd/go` | none | GO: fires the focused list's standby and moves it on. Held back within the show's GO window. |
| `/godot/cmd/cue/fire` | `,s` cue id | Fires that cue, standby untouched. |
| `/godot/cmd/cue/fireNumber` | `,s` cue number | Fires the cue with that number, standby untouched (new, decision F). |
| `/godot/cmd/standby/set` | `,s` cue id | Parks the focused list's standby on that cue. |
| `/godot/cmd/standby/setNumber` | `,s` cue number | Parks it on the cue with that number (new). |
| `/godot/cmd/run/stopAll` | none | Esc: everything stops gracefully, footers run. |
| `/godot/cmd/run/killAll` | none | Double Esc: everything is dropped, footers skipped. |

- **"GO at cue 12"** is a bundle of `standby/setNumber "12"` then `go`, applied in that order in
  one tick. QLab's `/go "12"` does the same.
- **A number names a cue by exact text.** `"12"` is not `"12.0"`. The focused list is searched
  first, then the other lists in order. No match is `unknown-id`.
- Go.dot has no pause or resume of everything. A processor that offers one says it cannot.

## 6. Describing yourself: what the menus show

A processor describes itself as an OSCQuery tree served at `GET <prefix>` on its query port
(the format of `docs/godot-reuse-map-0.1.md` §WFS-DIY and of `tests/fixtures/README.md`):
- The root's `FULL_PATH` is the prefix; every node's `FULL_PATH` is the root's plus its path.
- `TYPE` is the OSC type tags the node takes, `ACCESS` 2 for a command (write only) and 3 for a
  value, `DESCRIPTION` a sentence. `RANGE` gives `MIN`/`MAX`, and `VALS` the values a menu offers.
- **A command node has `ACCESS` 2 and no `VALUE`.** One with no `TYPE`, or only `N` or `I`,
  takes no argument: GO, previous. A cue aimed at it carries an empty value, and Go.dot sends it
  as a bare message.
- Go.dot sends `Connection: close` and reads until the connection closes, so the server must
  close after answering.

**What Go.dot's OSC cue editor does with it** (decision H, the author's words: "Each of the
path comes as a drop down menu. Selecting the client then shows a menu with the first item in
the path to choose from, then the next depending on the previous selection and so on."):
- The cue's `target` menu picks the device, as today.
- One menu per part of the path follows it. The first lists the children of the device's root,
  the next lists the children of what the first picked, and so on to a command or a value.
  Picking at one level clears the levels below it.
- When the chosen node has `VALS`, the value is a menu of them.
- In the foot panel's messages table, an arrow beside each message's address opens the same tree
  as one nested menu.
- A device with no description keeps the address and value as text.

## 7. The two processors

### 7.1 WFS-DIY (`/wfs`)

**Network tab.** A new protocol, "Go.dot", beside QLab: IP and port of Go.dot (8010), Rx and Tx
on, no patch number. Connecting the row declares `/wfs` with WFS-DIY's UDP receive port and its
OSCQuery port (5005 by default, 0 when OSCQuery is off).

**What it describes**, beyond today's `/wfs/input`, `/wfs/output`, `/wfs/reverb` and `/wfs/config`:

| Node | TYPE | ACCESS | VALS |
|---|---|---|---|
| `/wfs/input/snapshot/load` | `s` | 2 | the snapshot names |
| `/wfs/input/snapshot/store` | `s` | 2 | the snapshot names |
| `/wfs/cluster/<n>/lfoPresetRecall` | `i` | 2 | the stored presets, 1-based |
| `/wfs/effect/<n>/<param>` | as the effect map | 3 | |

It declares again when OSC Query starts, when a snapshot is stored or deleted, and every ten
seconds until Go.dot answers (Go.dot started after WFS-DIY).

**What it captures** (always `where = standby`, the identifier kept in the snapshot file as
`godotCueId` on `<InputSnapshot>`):

| Export | The cue's messages |
|---|---|
| Snapshot load cue | `/wfs/input/snapshot/load s:"<name>"` |
| Snapshot, per parameter ("Write to Go.dot") | one message per in-scope parameter, `/wfs/input/<n>/<param>` with `f:`, `i:` or `s:`, and the effects' at `/wfs/effect/<n>/<param>` |
| Sampler set | `/wfs/input/<n>/samplerSet i:<set, 1-based>` |
| Cluster LFO preset | `/wfs/cluster/<n>/lfoPresetRecall i:<preset, 1-based>` |

- `<n>` is the permanent channel number, as in QLab's cues today.
- A mute row is always the list form, `/wfs/input/<n>/mutes s:"0,1,0,…"`, one entry per output:
  the node is a string. On a one-output rig the row is a lone number, which WFS-DIY's router
  refuses as a row, so it is left out (the snapshot load cue still recalls it).
- A parameter is typed by WFS-DIY's bounds table, `i` or `f`, and is `s` when the table has no
  entry for it; the tree's input and effect nodes are typed the same way.
- There is no "store" twin of the load cue: Go.dot's cue recalls, and storing stays in WFS-DIY.
- A target of each kind is written to: QLab targets as today, Go.dot targets as here.

### 7.2 S21_HiJack (`/s21`)

**Setup tab.** "Cueing system: QLab | Go.dot", saved in the show file. Under Go.dot: Go.dot's IP
and port (8010). Connecting declares `/s21` with the trigger port (53001) and the web port as
the query port (8080; 0 when the web server is off).

**The trigger listener accepts its addresses under `/s21` as well as bare**, so Go.dot's cues
and QLab's both reach it: `/s21/snapshot/recall`, `/s21/snapshot/recall_full`, `/s21/cue/go`,
`/s21/cue/previous`, `/s21/cue/fire`, `/s21/macro/fire`.

**What it describes**, at `GET /s21` on the web port:

| Node | TYPE | ACCESS | VALS |
|---|---|---|---|
| `/s21/snapshot/recall` | `s` | 2 | the snapshot names |
| `/s21/snapshot/recall_full` | `s` | 2 | the snapshot names |
| `/s21/cue/go` | none | 2 | |
| `/s21/cue/previous` | none | 2 | |
| `/s21/cue/fire` | `f` | 2 | the cue numbers |
| `/s21/macro/fire` | `s` | 2 | the macro names |

It declares again when a snapshot, a cue or a macro is added, removed or renamed.

**What it captures** (always `where = standby`, the identifier kept on the snapshot as
`godot_cue_id`):

| Button | The cue's message |
|---|---|
| Go.dot recall cue | `/s21/snapshot/recall s:"<snapshot uuid>"`, or `recall_full` with "ignore scope" |
| Go.dot console capture | one message per parameter, aimed at the DiGiCo device the operator declares in Go.dot by hand (`/channel /console /digico`, its GP OSC port) |

The recall carries the snapshot's UUID, so a rename does not break the cue; S21_HiJack resolves
a UUID first and a name second. The menu offers names, which the same resolver takes.

### 7.3 S21_HiJack drives Go.dot

Per-cue triggers, made from templates, as QLab's are (decision G):

| Template | Sends |
|---|---|
| Go.dot: GO | `/godot/cmd/go` |
| Go.dot: fire this snapshot's cue | `/godot/cmd/cue/fire s:<godot_cue_id>` |
| Go.dot: GO at this snapshot's cue | bundle `/godot/cmd/standby/set s:<godot_cue_id>`, `/godot/cmd/go` |
| Go.dot: fire cue [N] | `/godot/cmd/cue/fireNumber s:"N"` |
| Go.dot: GO cue [N] | bundle `/godot/cmd/standby/setNumber s:"N"`, `/godot/cmd/go` |
| Go.dot: Esc | `/godot/cmd/run/stopAll` |
| Go.dot: double Esc | `/godot/cmd/run/killAll` |

- "This snapshot's cue" is read when the trigger fires: a snapshot never captured to Go.dot sends
  nothing and the log says so.
- Macro steps follow the cueing system: GO → `go`, GO cue N → the bundle above, Stop → `run.stopAll`,
  Panic → `run.killAll`. Pause and Resume fail with a sentence.

## 8. Not in this draft

- **A recall that answers** *(proposed)*: the processor reports the snapshot it loaded, under its
  prefix, and the cue waits for that report (a `heard` wait). Recall is fire-and-forget today
  (decision D).
- **Two processors with one root** — two WFS-DIY boxes both declaring `/wfs`: the second
  overwrites the first's host. One of them needs another root.
- **VALS are not checked** when a cue fires: a value off the menu is still sent.
