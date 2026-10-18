# Writing cues into Go.dot from your own application

**Draft 0.1**, 2026-10-17. For developers adding Go.dot support to an application that controls
something - a spatial processor, a console manager, a lighting or video tool - so that it writes
its snapshots into Go.dot as cues, the way WFS-DIY and S21_HiJack do.

It is the same protocol as [`godot-authoring-protocol-0.1.md`](godot-authoring-protocol-0.1.md),
which is the contract those two applications implement and is written around them; this page is
written around yours. Where either page and the code disagree, the code is the reference
(`src/wfg/engine/tree/AuthoringCommands.cpp` for the commands, `Mount.cpp` for the description and
the checks at GO) and the page gets fixed. The verb and answer names (`mount.declare`,
`cue.capture`, `/godot/captured`...) are still draft names: keep them in one place in your code, as
both reference implementations do, so a rename is one edit.

---

## What your application gets

- **It becomes a device in Go.dot** with one message. The operator never types your address or port.
- **It writes a cue in one message**: its name, its number, its notes and every OSC message it will
  send. Go.dot answers once. Writing the same cue again updates it where it stands.
- **It describes itself**, and Go.dot offers your addresses as cascaded menus in its cue editor -
  one drop-down per part of the path, as QLab does - and checks every cue aimed at you before it
  sends anything.
- **It can drive Go.dot**: GO, fire a cue by identifier or by number, stop.

When the operator fires one of those cues, Go.dot sends its messages to your receive port as plain
OSC, one message after another in the order the cue holds them - at most 20 datagrams a tick (a
thousand a second) unless the operator changes the device's limit, and bundled up to 1,200 bytes a
datagram when you say you take bundles (§4). Your application receives exactly what it would receive
from any other controller. What it must do is take a snapshot of thousands of messages without
dropping any: a receive queue of a few hundred items, emptied a few dozen at a time, does not.

## Reference implementations

| What | Where | Language |
|---|---|---|
| WFS-DIY | `Source/Network/GoDotProtocol.h` (spelling, identifiers, chunks, answers), `Source/Network/GoDotCueBuilder.h` (the cues), `Source/Network/OSCManager.cpp` section "Go.dot Integration" (declare, send, wait), `Source/Network/OSCQueryServer.cpp` (the description) | C++, JUCE |
| S21_HiJack | `src/osc/godot_client.rs`, `src/osc/godot_cue_builder.rs`, `src/osc/godot_reply.rs`, `src/console/godot_link.rs`; `docs/godot-cueing.md` | Rust |
| A minimal processor | `tests/blackbox/authoring.py` in this repository: serves a three-node description, declares, captures, hears its own cue come back at GO | Python, standard library |
| A stand-in Go.dot | WFS-DIY's `tools/validation/control-replay/godot_mock.py`: answers the declare and the capture, fetches your description and checks your captures against it | Python, standard library |

`authoring.py` is the shortest path to something working: about 230 lines, its own test checks
included.

---

## 1. The exchange at a glance

```
your application                                          Go.dot
receives OSC on UDP 9000, serves OSCQuery on HTTP 9001    receives OSC on UDP 8010

/godot/cmd/mount/declare "/myapp" 9000 9001 "My App"  ------>
                                                      <------  /godot/declared "4X7KQ2MB" "created"
HTTP 200, the JSON tree                               <------  GET http://<you>:9001/myapp
                                                      <------  /godot/described "4X7KQ2MB" 4 ""

/godot/cmd/cue/capture "/myapp" "standby" "" "9QWJ3T0E" "Storm" "" "" ""
    "/myapp/gain" "f:-6" "/myapp/mode" "s:\"wide\""   ------>
                                                      <------  /godot/captured "9QWJ3T0E" "created" "" "Storm"

... later, the operator presses GO on that cue ...
                                                      <------  /myapp/gain -6.0
                                                      <------  /myapp/mode "wide"
```

Every answer goes to the receive port you declared (9000 here), never to the port your datagram
left from, so you may send from any socket.

## 2. Transport and spelling

**Where to send.** OSC over UDP to Go.dot's OSC port: 8010 unless the operator started it with
another (`--osc-port`). Binary OSC over Go.dot's WebSocket (its HTTP port, 5010 by default) is
accepted too. Either way, answers come back as UDP to your declared port. The command `a.b` is
addressed `/godot/cmd/a/b`.

**Bundles.** You may wrap commands in an OSC bundle: its elements are applied in order, in the same
engine tick. That is how "GO at cue 12" is said (§8).

**Size.** Keep every datagram under **1,200 bytes**, so it is never fragmented on the way. A cue too
long for one datagram is sent in chunks (§6.5).

**Strict senders.** When the operator sets Go.dot's OSC filter to "registered only", a datagram from
a host that is not a declared device with Rx on is dropped before it is read, and nothing answers.
Your first declare has to reach Go.dot before that filter is switched on, or the operator adds your
host in Go.dot's Network tab. The symptom is a declare that is never answered; keep declaring
(§4.3).

**Identifiers** are 8 characters of Crockford base32, `[0-9A-HJKMNP-TV-Z]{8}`, drawn from
`0123456789ABCDEFGHJKMNPQRSTVWXYZ` - no I, L, O or U. Draw your own with any random source.

**A command's own arguments** are typed as each command lists them: `s` is an OSC string, `i` an
int32. Send exactly those. A command whose arguments do not fit - too few, a number where a string
belongs, a NaN - is refused before it runs, **and nothing answers it**: the reason (`arity`,
`type-mismatch`, `non-finite`) is only in Go.dot's log and at `/godot/engine/lastError`. When an
answer never comes, check your type tags first.

**The values of a cue's messages** are written as an *atom list*: one atom per OSC argument,
separated by single spaces. This is the spelling of an OSC cue's value in Go.dot itself.

| Atom | OSC argument |
|---|---|
| `i:42` | int32 |
| `h:42` | int64 |
| `f:1.5` | float32 (read as a double, then narrowed, so any shortest float text reads back exactly) |
| `d:1.5` | float64 |
| `s:"text"` | string, always in double quotes |
| `T`, `F` | true, false |
| `N`, `I` | nil, impulse |
| `t:<unsigned 64-bit integer>` | time tag |
| `b:<base64>` | blob |

- A number uses `.` as its decimal point whatever the locale. Format numbers in the C locale: a
  French machine writing `1,5` makes the whole list unreadable.
- Inside `s:"..."`, write `\` as `\\` and `"` as `\"`. `\n`, `\r`, `\t` and `\uXXXX` are read too.
  A bare `"` inside the quotes, or a word with no tag, makes the whole list unreadable.
- An empty list is the empty string: the message goes out with no argument.

`i:2 i:3 f:-12.5` is three arguments; `s:"Act 1 \"dawn\""` is one.

## 3. Your description (OSCQuery)

A description is optional, but it is what turns your addresses into menus in Go.dot and lets Go.dot
catch a wrong address at a rehearsal rather than during the show. Without one, your device is
*opaque*: Go.dot sends whatever a cue spells, unchecked, and the operator types addresses by hand.

**Serve it** over HTTP at `GET <prefix>` on your query port: `GET /myapp`, not `GET /`. Answer 200
with the JSON, then close the connection. Go.dot sends `Connection: close` and reads until the socket
closes, which is what an HTTP/1.0 server does (Python's `http.server` by default). Go.dot gives up
after 5 seconds. It fetches away from its engine thread, so a slow answer never stalls a show; it
only delays the menus.

**Its shape** is an OSCQuery tree whose root's `FULL_PATH` is your prefix:

```json
{
  "FULL_PATH": "/myapp",
  "CONTENTS": {
    "gain":  { "FULL_PATH": "/myapp/gain", "TYPE": "f", "ACCESS": 3,
               "RANGE": [{ "MIN": -90, "MAX": 10 }], "UNIT": ["dB"],
               "DESCRIPTION": "Master gain" },
    "mode":  { "FULL_PATH": "/myapp/mode", "TYPE": "s", "ACCESS": 3,
               "RANGE": [{ "VALS": ["narrow", "wide"] }] },
    "scene": { "FULL_PATH": "/myapp/scene",
               "CONTENTS": {
                 "recall": { "FULL_PATH": "/myapp/scene/recall", "TYPE": "s", "ACCESS": 2,
                             "RANGE": [{ "VALS": ["Opening", "Storm", "Finale"] }],
                             "DESCRIPTION": "Recalls a scene by name" } } },
    "go":    { "FULL_PATH": "/myapp/go", "ACCESS": 2, "DESCRIPTION": "Next scene" }
  }
}
```

What Go.dot reads, and what it does with it:

| Field | Effect |
|---|---|
| `CONTENTS` | The children. A name must not be empty and must not contain `/`. |
| `FULL_PATH` | Checked against where the node is nested. A mismatch refuses the whole description. |
| `TYPE` | The node's OSC type tags, **one per argument**. A cue aimed at the node must carry exactly that many atoms. No `TYPE`, or only `N` and `I`: the node takes no argument, and a cue aimed at it carries an empty value - a command such as `/myapp/go`. Go.dot can convert to `i`, `h`, `f`, `d`, `s`, `b` and `T` (a boolean is `T`); a node of any other tag cannot be written. |
| `ACCESS` | `2` write-only, `3` read-write. **Absent means read-only**, and a cue aimed at a read-only node fails (`read-only`). Set it on every node a cue should reach. |
| `VALUE` | Never taken as the node's current value. It is only read to tell a command from a value: a node with `ACCESS` 2 and no `VALUE` is a command (an event), anything else a value (a state). |
| `RANGE` | One entry per argument. `VALS` in the first entry turns the value into a drop-down in Go.dot's editors. `MIN` and `MAX` scale Go.dot's fades, curves and controller bindings. Neither is enforced when a cue fires: a value off the list or out of bounds is still sent. |
| `UNIT` | Shown beside the value. |
| `DESCRIPTION` | Shown in the menus. |
| `GODOT` | Go.dot's own hints, all optional. `KIND` (`"state"`, `"event"` or `"container"`) overrides the guess above. `RATE_CAP`: at most how many messages a second Go.dot sends the node, during a fade or a curve (50, one a tick, when absent). `ANTICIPATABLE`: whether Go.dot may send to the node ahead of GO (false when absent). `PANIC`: the node's resting state, `"park"`, `"snap"` or an array of values - read and published, not yet applied. |

The whole description is refused - and the previous one kept (§4.2) - when it is not valid JSON,
when a `FULL_PATH` disagrees with the nesting, when two nodes share an address, or when it holds no
node at all.

**At GO, against a described device**, each message of the cue is checked against the description:
an address that is not a node is `bad-address`; the wrong number of atoms, or an atom that cannot
become the node's type, is `type-mismatch`. Numbers convert between `i`, `h`, `f` and `d` - a float
sent to an `i` node is truncated toward zero, so send `i:` for an integer. `T` takes `T`, `F` or an
integer. A string never becomes a number, and a number never becomes a string.

**One table for both.** Generate the tree you serve and the cues you write from the *same* table of
address and type. WFS-DIY's `GoDotProtocol.h` is that table: its OSCQuery server and its cue builder
both ask it, so the two cannot disagree, and a parameter added to WFS-DIY's router reaches both
without further work. Type each value by its parameter, never by whatever variable happens to hold
it: a value read back from a file may be a string when the parameter is a float.

## 4. `mount.declare`: become a device

```
/godot/cmd/mount/declare  ,siis[s[s[i]]]  prefix  port  queryPort  name  [id  [host  [bundles]]]
```

| Argument | Meaning |
|---|---|
| `prefix` | Your one root, such as `/myapp`. It starts with `/` and has no trailing `/`, no space and no `//`; it is not `/godot`, `/ui` or `/media`, nor anything under them. |
| `port` | The UDP port you receive OSC on, 1 to 65535. The messages of your cues and all of Go.dot's answers are sent there. |
| `queryPort` | The HTTP port of your OSCQuery server, or `0` when you describe nothing. |
| `name` | The device's name in Go.dot's Network tab. Not empty. |
| `id` | Optional. The device identifier to create the device with. Empty or absent: Go.dot draws one. |
| `host` | Optional. Your IP address as Go.dot should reach it, a literal address and not a host name. Absent: the address the datagram came from, which is nearly always what you want. |
| `bundles` | Optional, an integer: `1` if you take OSC bundles, `0` if you do not. Go.dot then sends your cues' messages bundled, up to 1,200 bytes a datagram - a few dozen messages in one. `1` switches Bundles on, when the device is made and whenever you declare again; `0`, or leaving it out, never switches it off. To send it, send `id` and `host` too; an empty string for either means absent. |

### 4.1 What Go.dot does

The device is found by its **root**: whichever device already holds `/myapp` among its prefixes is
you, whatever the operator has called it since.

- **Found**: its host, port and query port are updated where they changed, and Bundles is switched
  on if your `bundles` is `1`. Its name, Rx and Tx stay as the operator set them. The answer is
  `updated`.
- **Not found**: a device is made with your root, host, ports and name, Rx and Tx on, and Bundles as
  your `bundles` says. The answer is `created`.
- Either way it is one undo step in Go.dot, and a declare that repeats the last one changes nothing.

```
/godot/declared  ,ss  deviceId  outcome          outcome: created | updated | <reason>
```

Refused with `bad-value` for an unusable root, an empty name, a port out of range or no host to
answer; with `locked` when the operator has locked the show and the declare would make a device or
move one. A declare that changes nothing is still answered `updated` under the lock.

### 4.2 Then Go.dot reads your description

When `queryPort` is not 0, **every** declare makes Go.dot fetch
`GET http://<host>:<queryPort><prefix>`, check it (§3), keep it inside the show as
`namespaces/<deviceId>.json`, and answer:

```
/godot/described  ,sis  deviceId  nodeCount  problem          problem is empty when it loaded
```

A fetch that fails keeps the description Go.dot had, and the device shows the problem in Go.dot's
Network tab. Because the description lives in the show, its menus and its checks work when the show
is opened without your application running.

### 4.3 When to declare

- **When the connection to Go.dot is set up**, and at every launch.
- **Whenever what you describe changes**: a parameter added, a snapshot or preset stored, renamed or
  deleted - anything that appears in a `VALS` list. Go.dot reads your description again only when
  you declare. It never polls you.
- **When your OSCQuery server starts or moves to another port**, so that Go.dot does not keep the 0
  of a declare sent before it was running.
- **Every 10 seconds or so until a `/godot/declared` arrives**: Go.dot may start after you.
  WFS-DIY does exactly this.

## 5. Keep your cue identifiers

The protocol's one rule worth remembering: **the identifier wins**. When a cue with the identifier
you send already exists in the show, Go.dot updates that cue where it stands, whatever `where` says.
So:

- Draw an identifier the first time something - a snapshot, a preset - is exported to Go.dot, and
  **store it beside that thing**, in your own file. WFS-DIY writes `godotCueId` (the cue of the
  snapshot's parameters) and `godotLoadCueId` (the cue that loads the snapshot) on the snapshot
  file's root; S21_HiJack keeps `godot_cue_id` on its snapshot.
- Send the stored identifier on every later export. The operator's cue is updated; its place, its
  number, its triggers and its waits are not touched.
- When the operator has deleted the cue meanwhile, the same identifier makes it again at the landing
  place.
- For something meant to be placed many times in a show - WFS-DIY's sampler-set cue - draw a new
  identifier on each press.

## 6. `cue.capture`: create or update a cue

```
/godot/cmd/cue/capture  ,ssssssss(ss)...  from  where  target  id  name  number  notes  messageIds  address value  [address value]...
```

Every argument is a string. `from` is your root, exactly as you declared it (`/myapp`): it is how Go.dot
knows who to answer (§6.6).

### 6.1 Where a new cue lands: `where` and `target`

| `where` | `target` | A new cue lands |
|---|---|---|
| `standby` | ignored, send `""` | Right after the standby cue of the list the operator is in - inside the same group when the standby is in one - or at the end of that list when nothing stands by. |
| `list` | a list's identifier or name; `""` for the list the operator is in | At the end of that list. |
| `cue` | a cue's identifier, or its number | Nowhere new: that existing cue's content is replaced. It must be an OSC cue. |
| `more` | the identifier of the cue being sent in chunks | Nowhere new: the pairs are added to that cue (§6.5). |

Use `standby` unless you have a reason not to. It puts the cue where the operator is working, which
is where a QLab user expects a new cue to appear.

### 6.2 The head

| Field | Meaning |
|---|---|
| `id` | Your stored identifier (§5). `""`: Go.dot draws one, and the answer says which. |
| `name`, `number`, `notes` | Written when not empty, left alone when empty. A cue with no name shows a name Go.dot makes from its content; a new cue with no number has none. |
| `messageIds` | Always `""`. Go.dot fills it in its own log so that a replay of the log is exact. |

Sending an empty `number` is the polite default: operators renumber cues, and a number you send
overwrites theirs on every export.

### 6.3 The pairs

At least one `address value` pair, where `value` is an atom list (§2).

- The first pair is the cue's own message; the rest are its further messages, in order. Go.dot sends
  them all in the same tick, in that order, when the cue fires.
- An update **replaces all of them**.
- **Every address must be under one declared device** - yours - and strictly under its root:
  `/myapp/gain`, not `/myapp`. An address under no device is `unknown-id`; addresses under two
  devices are `bad-address`.
- At capture time Go.dot checks only that, and that each value reads as atoms. **It checks your
  addresses against your description at GO, not before** (§3). Capture only addresses your
  description has.

### 6.4 Refusals

In this order, before anything is written:

| Outcome | When |
|---|---|
| `locked` | The operator has locked the show. |
| `bad-value` | `where` is not one of the four words; no pair, or an odd number of strings after the head; an address not starting with `/`; a value that does not read as atoms. |
| `unknown-id` | An address under no declared device; or, for `cue` and `more`, no cue matches the target; or, for `standby` and `list`, there is no list to land in. |
| `bad-address` | Addresses under more than one device. |
| `type-mismatch` | The cue to update is not an OSC cue. |

A refusal writes nothing.

### 6.5 Chunks

A capture that would pass 1,200 bytes is split:

1. The first datagram is the whole head - `from`, `where`, `target`, `id`, `name`, `number`,
   `notes`, `messageIds` - followed by as many pairs as fit.
2. Each following datagram has `from` = your root again, `where` = `"more"`, `target` = the
   identifier, `id` = the same identifier, `name`, `number`, `notes` and `messageIds` empty, then
   the next pairs.

Go.dot applies them in the order they arrive: on an update, the first chunk replaces the messages
and the `more` chunks add the rest. Each chunk is its own undo step and gets its own answer
(`appended` for a `more`). A chunked capture needs an identifier - send your own, never `""`.

Counting bytes: an OSC string takes its UTF-8 length plus a zero terminator, padded to a multiple
of four; the type-tag string is `,` and one `s` per argument, padded the same way; add the address.
A single pair too long for a datagram of its own still goes, alone, in its own chunk.
`captureMessages` in WFS-DIY's `GoDotProtocol.h` does all of this in about fifty lines.

Space the chunks a few milliseconds apart: WFS-DIY leaves 5 ms, its gap between bundles. A snapshot
of seventy inputs is nearly two hundred chunks, and two hundred datagrams back to back can overrun a
receive buffer on the way even when nothing is wrong with the network. Where datagrams may be
reordered, wait for the first chunk's answer before sending the `more` chunks: a `more` that
overtakes its first chunk on a new cue is refused `unknown-id`.

### 6.6 The answer

```
/godot/captured  ,ssss  id  outcome  number  name
```

- `outcome` is `created`, `updated`, `appended`, or the reason it was refused (§6.4).
- `number` and `name` are the cue's as they now stand - so you can say "Cue 14, Storm, updated" -
  and empty on a refusal.
- It is sent to the device whose root is `from`, at the host and port it declared - where your
  declare's answer went. It does not matter which socket you sent from, nor which device the cue is
  aimed at: S21_HiJack captures cues aimed at a DiGiCo desk and is still the one answered.
- Until Go.dot holds a device with that root - you have not been declared yet - it cannot use
  `from`, and guesses as it does for a capture without one: the device at your address whose port
  the datagram left from, else the one the cue is aimed at, else the only one, else every device at
  your address. **So ignore a `/godot/captured` whose identifier you did not send.**
- Expect one answer per datagram. WFS-DIY waits 2 seconds for them, reports the first refusal at
  once, and otherwise reports when they have all arrived. No answer means Go.dot is not running or
  not reachable, has dropped you (strict senders, §2), or refused a command whose type tags were
  wrong (§2).

### 6.7 What WFS-DIY captures, as an example

The four cues WFS-DIY writes, each with `where` = `standby`:

| Export | The cue's messages |
|---|---|
| Snapshot recall | `/wfs/input/snapshot/load s:"<snapshot name>"` |
| Snapshot, parameter by parameter | one message per parameter in the snapshot's scope: `/wfs/input/<n>/positionX f:2.5`, `/wfs/input/<n>/mutes s:"0,1,0,0"`, ..., then the effects', `/wfs/effect/<n>/<name>` |
| Sampler set | `/wfs/input/<n>/samplerSet i:<set>` |
| Cluster LFO preset | `/wfs/cluster/<n>/lfoPresetRecall i:<preset>` |

A snapshot of a few hundred parameters is one cue of a few hundred messages, sent in a handful of
chunks, where QLab needs one cue per parameter in a group.

## 7. Reading the answers

All three answers arrive on your receive port, among whatever else your application receives there.
Intercept `/godot/declared`, `/godot/described` and `/godot/captured` before your own address
routing, which would otherwise take them for unknown parameters. Read them leniently: a missing
argument is an empty string or 0.

| Answer | Arguments |
|---|---|
| `/godot/declared` | `s` device id, `s` outcome |
| `/godot/described` | `s` device id, `i` node count, `s` problem (empty when it loaded) |
| `/godot/captured` | `s` cue id, `s` outcome, `s` cue number, `s` cue name |

## 8. Driving Go.dot

Any client may send these; S21_HiJack's per-cue triggers do.

| Address | Arguments | Does |
|---|---|---|
| `/godot/cmd/go` | none | GO: fires the standby cue of the list the operator is in, and moves the standby on. Held back inside the show's GO debounce. |
| `/godot/cmd/cue/fire` | `s` cue id | Fires that cue; the standby does not move. |
| `/godot/cmd/cue/fireNumber` | `s` cue number | Fires the cue with that number; the standby does not move. |
| `/godot/cmd/standby/set` | `s` cue id | Parks the standby on that cue. |
| `/godot/cmd/standby/setNumber` | `s` cue number | Parks the standby on the cue with that number. |
| `/godot/cmd/run/stopAll` | none | Esc: everything stops gracefully. |
| `/godot/cmd/run/killAll` | none | Double Esc: everything stops at once. |

- **GO at cue 12** is a bundle of `/godot/cmd/standby/setNumber "12"` then `/godot/cmd/go`,
  applied in that order in one tick - QLab's `/go "12"`.
- **A number names a cue by its exact text**: `"12"` is not `"12.0"`. The list the operator is in
  is searched first, then the others in order. No match is `unknown-id`.
- These verbs do not answer. A refusal is in Go.dot's log and at `/godot/engine/lastError`.
- Go.dot has no pause or resume of everything.

## 9. When your application changes

**New parameters need no change in Go.dot.** Go.dot holds no list of your parameters - nor of
WFS-DIY's. Everything it knows about your device comes from your description, fetched when you
declare and kept in the show. So:

1. Add the parameter to your router and to your description (one table, §3).
2. Declare again - at launch, which covers a new version of your application, or at once.
3. Go.dot reads the description again: its menus offer the new address, GO accepts it, and your
   captures may carry it.

What to watch for:

- **Declare before you capture a new address.** The capture lands, since capture does not check the
  description, but the cue fails `bad-address` at GO until Go.dot has read a description that has
  the address.
- **Adding is free; renaming, removing and retyping are not.** A cue already in a show that holds a
  renamed or removed address fails `bad-address` at GO; one whose node changed its number of
  arguments, or went from a number to a string, fails `type-mismatch`. Keep old addresses alive in
  your router and your description, or export the affected cues again: with their stored
  identifiers they are updated in place.
- **A show remembers the last description it read.** A show opened with an older version of your
  application takes that version's description at its declare, and cues using addresses the older
  version lacks fail at GO.
- **New names in a `VALS` list** - a snapshot stored - reach the menus at the next declare. A value
  not in the list is still sent: `VALS` is a menu, not a rule.

**What does need Go.dot's code**: a new verb or a new answer in this protocol; a new key under
`GODOT` that should mean something; a type tag outside `i h f d s b T`.

## 10. A main and a backup Go.dot

With two Go.dot machines run as main and backup, send to the main. A backup that is following
ignores OSC over UDP - so sending to both is harmless - and applies what the main applied, your
declares and captures included. A following backup fetches no description and sends no answer: all
answers come from the main.

## 11. Known limits

- **One root per application.** Two copies of one application declaring the same root share one
  device in Go.dot, the second moving the first's host. Give each its own root.
- **Captures that do not name their sender.** WFS-DIY up to 1.0.0beta56 and S21_HiJack 0.3.0 send
  captures without `from`, beginning at `where`; Go.dot still reads them, and tells the two forms
  apart by the first argument alone - a root is empty or begins with `/`, `where` never does. Their
  answers are guessed (§6.6), and two such applications on one computer may both receive one.
  WFS-DIY 1.0.0beta57 and S21_HiJack 0.3.1 send `from`, and need a Go.dot from 2026-10-17 or later.
- **Recall is fire-and-forget.** Go.dot does not wait for your application to confirm that it
  recalled anything.
- **Go.dot asks nothing after the declare**: no polling, no subscription.

## 12. Checklist

1. A UDP port you receive OSC on, with a receive buffer and a queue that take a cue of thousands of
   messages at Go.dot's pace (20 datagrams a tick by default), and a socket to send to Go.dot's
   port, 8010.
2. `GET <prefix>` served as OSCQuery JSON, the connection closed after the body: `ACCESS` on every
   node a cue should reach, `TYPE` one tag per argument, `VALS` where a menu helps.
3. A declare when the connection is set up, at launch, when the description changes, when your
   OSCQuery server starts, and every 10 seconds until answered.
4. Identifiers of 8 Crockford base32 characters, drawn once and stored beside what they recall.
5. Captures beginning with your root as `from`, then `where` = `standby` and the stored
   identifier; atoms written in the C locale, strings quoted and escaped; datagrams under 1,200
   bytes, chunked with `more`, each chunk with `from` again.
6. The three answers intercepted before your own routing; a `/godot/captured` for an identifier
   you did not send ignored; 2 seconds per expected answer; the outcome, the cue's number and its
   name shown to the operator.
7. Tried against `godot_mock.py` (`python godot_mock.py --port 8010`, and `--refuse locked` for a
   refusal), then against Go.dot: capture, GO, hear the messages arrive, capture again and read
   `updated`.
