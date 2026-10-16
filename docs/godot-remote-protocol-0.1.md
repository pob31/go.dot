# The remote: what a tablet app and Go.dot say to each other

**Draft 0.1**, 2026-10-15. The contract the Android remote (`github.com/pob31/go.dot-remote`)
implements to read Go.dot's state and drive it, and that Go.dot implements to answer it. PRD §3.17
is the reason for it; namespace draft §68 records the decisions and the stages. Where this page and
the code disagree, the code is being wrong and one of them gets fixed.

The author decided four things on 2026-10-15 (§68.2): the app lives in its own repository; it talks
to Go.dot over the WebSocket, as the page does, never over the UDP port; it is pointed at a machine
by a typed address and port, with no discovery; and it may adjust levels, sends, EQ, DCAs, waits,
speed, the on/off switches, flags and notes - but never the shape of the cue list. **The words on
this page are mine** (`STREAM`, `UNSTREAM`, `HELLO`, `FIND`, `network/remotes`, `remote.find`) until
the author renames them.

Nothing here is only for Android. Any client that wants the whole state without polling - a second
desktop, a script, an iPad app one day - can speak it.

---

## 1. Why a stream, and why the WebSocket

A tablet that is swiped away and back must be **live at once**: no reconnect, no full dump, no
handshake. On Android a foreground service keeps the connection open while the app is in the
background, so the only question is what travels on it.

- **The WebSocket on Go.dot's HTTP port (5010 by default), not the UDP port (8010).** It is TCP:
  nothing is lost, so the app needs none of the dump / verify / re-request machinery a UDP remote
  needs. And it is what a backup machine passes on (§58, AGD): a command a backup hears on the
  WebSocket is applied by the main, while one it hears on the UDP port is a device's and ignored.
- **One bundle per tick, everything that changed.** The OSCQuery `LISTEN` is one address at a time,
  no wildcards; a tablet showing a cue list, the running cues and a surface would LISTEN to
  thousands of addresses and re-LISTEN at every new run. `STREAM` is one request for a whole subtree.
- **The tick is the clock.** Every bundle opens with `/godot/engine/tick`. The app never runs a timer
  of its own for anything the engine decides (the Doh! window, the debounce): it counts ticks.

## 2. Connecting

0. `GET http://<host>:<port>/?HOST_INFO`. A Go.dot whose `EXTENSIONS` has no `"STREAM": true` is older
   than this contract: the app says so in words and asks again every few seconds, rather than
   waiting for bundles that will never come.
1. Open a WebSocket to `ws://<host>:<port>/` (the path is `/`; the page uses the same).
2. Send the text frame `{"COMMAND": "STREAM", "DATA": "/godot"}`. From the next tick on, binary
   frames arrive (§3). Keep them; do not apply them yet.
3. Send the text frame `{"COMMAND": "HELLO", "DATA": "<id>\t<name>"}` (§5).
4. `GET http://<host>:<port>/godot` - the whole tree as OSCQuery JSON (§4). Read
   `/godot/engine/tick` out of it: that is the tick the snapshot is.
5. Apply the snapshot. Then apply every kept bundle whose tick is **later** than the snapshot's,
   in order, and every bundle after that as it comes. A bundle whose tick is the snapshot's or
   earlier is already in it.

The STREAM goes first so nothing falls between the snapshot and the stream. A snapshot of 500 cues is
about 8.6 MB; read it as a stream of tokens, never into a DOM.

## 3. The stream

```
{"COMMAND": "STREAM",   "DATA": "/godot"}      start, or move to another prefix
{"COMMAND": "UNSTREAM", "DATA": "/godot"}      stop
```

- **One OSC bundle per tick**, time tag immediate. Its first element is always
  `/godot/engine/tick ,h <tick>`; then one message per node under the prefix whose value changed
  this tick, or that appeared this tick, carrying the value it has now - the same typed values a
  LISTEN push carries (`i h f d s b T F N I`), the tag string the node's TYPE at that moment (a list
  row's grows with its value).
- **A tick with nothing else changed still sends a bundle** with the tick alone: 50 a second is the
  heartbeat (§7).
- **Nothing is withheld from the client that caused it.** A LISTEN push skips the client whose write
  made the change and a client touching the node; the stream does not, so the app's mirror is always
  the engine's. The app ignores pushes for a control under the operator's finger, and takes the
  engine's value again when the finger lifts.
- **One prefix per connection.** A second STREAM replaces the first.
- **A client that stops reading is let go.** Each connection may have 100 frames written and not yet
  taken (two seconds); past that, the server stops streaming to it and closes the connection. The tick
  never waits for a tablet. The app reconnects (§7).

## 4. Structure: what appears and what goes

- **Values come from the stream only**, once the snapshot is applied. A node that appears this tick
  is in the tick's bundle with its value.
- **What a node is** - its `TYPE`, `ACCESS`, `RANGE`, `UNIT`, `DESCRIPTION` - comes from HTTP. Go.dot
  sends `{"COMMAND": "PATH_ADDED", "DATA": "<address>"}` to every connection for each node that
  appears, before the tick's bundle, and `PATH_REMOVED` for each that goes. For an added node whose
  description the app does not have, `GET` its object once per burst - `/godot/run/<id>`,
  `/godot/cue/<id>` - and take **only the metadata** from the answer, never the values. The rows of
  one owner share their metadata, so `/godot/run/<a>/position` describes `/godot/run/<b>/position`:
  an app may key metadata by the address with the identifier replaced and skip the GET.
- **`PATH_REMOVED`** drops the node. A finished run stays published for five seconds and then goes.
- `PATH_CHANGED` and `PATH_RENAMED` are never sent.

The snapshot's JSON (`OscQueryJson.cpp`): every node has `FULL_PATH`, `ACCESS` (0 none, 1 read,
2 write, 3 both), and where they apply `TYPE`, `VALUE` (an array), `RANGE` (an array, one entry per
argument: `{"MIN": …, "MAX": …}` or `{"VALS": […]}` for a closed set), `UNIT` (an array),
`DESCRIPTION`, the vendor object `GODOT`, and `CONTENTS` (children by their last segment).

## 5. Who is connected, and finding a tablet

```
{"COMMAND": "HELLO", "DATA": "<id>\t<name>"}     the app says who it is
{"COMMAND": "FIND",  "DATA": "<seconds>"}        Go.dot asks it to show itself
```

- **`HELLO`** names the connection: `<id>` is the app's own, drawn once at install and kept, so a
  tablet is the same tablet across reconnects; `<name>` is what the operator called it. It writes
  nothing to the show and is in no log: who is connected is a fact about tonight.
- **`/godot/network/remotes`** (read-only, runtime) lists the connections that said HELLO, one a line:
  `<origin>\t<id>\t<name>`, the origin as the event log spells it (`ws:<ip>:<port>`; the server
  listens on both address families, so an IPv4 tablet reads `ws:::ffff:192.168.1.20:57069`). The app
  finds its own line by its `<id>`, which is how it learns its origin (§6).
- **`remote.find <id> [seconds]`** - machine-local, as `videoOutput.identify` is - sends `FIND` to
  every connection whose HELLO carried that `<id>`; 30 seconds when none is given, never more than 300.
  `<seconds>` in the FIND frame is a whole number. The app flashes its screen, sounds an alarm and
  vibrates until the time is up or the screen is touched. Show settings >
  Network lists the remotes with a button for it.

## 6. Writing

Binary frames, OSC, exactly as the page sends them (`clients/console/plumbing/link.js`):

- **A command** is a message to `/godot/cmd/<a>/<b>` for the command `a.b`, its arguments OSC
  strings: `/godot/cmd/go`, `/godot/cmd/run/kill ,s <run>`, `/godot/cmd/run/seek ,ss <run> <seconds>`.
- **A value** is a message to the node's address with ONE string argument, whatever the node's type:
  `/godot/cue/<id>/level ,s "-6.5"`. It is `node.set`. A list (`mesh`, `points`) is one string, its
  items separated by spaces. A boolean is `true` or `false`. **Numbers are written in no locale's
  way** - a decimal point, never a comma - whatever the tablet's language.
- **A drag** is `node.touch <address>`, a run of values, `node.release <address>`. The document
  folds a run of writes to one address from one origin into one undo step.
- **A refusal** is only ever `/godot/engine/lastError`: `<tick> <seq> <origin> <reason> [<command>]`.
  The app watches it in the stream, and says in words the ones whose origin is its own.
- **The show lock** (`/godot/document/locked`) refuses every write to what the show file records.
  A cue's EQ and its sends ride live and unsaved under it (§17.14, AM), as do a DCA mark's curve and
  offset and a plugin's parameters; its level does NOT - under the lock a level is ridden with the
  run's `trim` or a DCA's `trim`, which are tonight's and never stored, as are flags. The app shows
  the rest greyed with the word. (Corrected 2026-10-16: the first draft said levels rode live; the
  engine refuses `cue/<id>/level` under the lock, found by the app's A.6 against a live serve.)
- **Stopping a run**: `run.stop <run> [verb]` takes `hard` (the default), `afterMember` and
  `afterIteration` - the last two for a group, which has a member or a round to wait for; `fade` and
  `advance` are refused. Advancing a ranged sound is `run.advance <run>`; `run.solo` is a sampler
  clip's.

The app sends no structure command: no `cue.create`, `object.delete`, `object.move`, nor a write to a
cue's name or number (§68.2, ANO).

## 7. Staleness, and coming back

- **Stale**: no bundle for half a second. The app says so in words and greys what it shows; a value
  being dragged lands on the last value sent (PRD §3.17).
- **Gone**: no bundle and no answer to a WebSocket ping for three seconds. The app closes the socket
  and connects again (§2) at 0.5, 1, 2 and then every 5 seconds.
- A link alive with no ticks is Go.dot's clock standing still (an audio interface lost, §6.2), not
  the network: the app says that instead. While the clock is held for an interface being changed,
  bundles keep coming every 20 ms carrying the same tick: the link is alive and time is not moving.

## 8. Two machines

A tablet may hold a link to a main and to its backup at once (§58). Each link is a connection of its
own, with its own stream and mirror; one is **on screen**, and its words, its cues and its runs are
what the app shows. The other stays linked, so a takeover costs the tablet nothing.

- Commands go to the machine on screen. Sent to a following backup, they are passed on to its main
  and applied there (AGD), so either works.
- `pair.takeOver`, `pair.handOver` and `pair.keep` are machine-local: the app sends each to the
  machine whose button was pressed.
- The pair's words are the engine's (`/godot/pair/role`, `state`, `alarm`; AGM-AGO).

## 9. What each tab reads and sends

Every row is in `docs/parameters/godot-parameters.csv`; this is where the app looks, not a second copy.

| Tab | Reads | Sends |
|---|---|---|
| Header | `list/focus`, `list/<id>/{standby,history,doh,dohWindow}`, `engine/tick`, `document/locked`, `engine/lastError` | `go`, `go.doh`, `run.stopAll`, `run.killAll`, `standby.previous`, `standby.next` |
| Running | `run/order`, `run/<id>/{cue,kind,parent,state,phase,position,remaining,level,trim,meter,timbre,round,pruned,seekable,range,strip,held}` | `run.stop` (§6), `run.kill`, `run.advance`, `run.prune`, `run.unprune`, `run.seek`, `run.solo`; `trim` by touch, set, release |
| Cues | `list/<id>/{name,order}`, `cue/<id>/…` with groups' `order`, `headerOrder`, `footerOrder`, the sound rows, `send/<id>/…`, `bus/<id>/name`, `dca/<id>/…` | `standby.set`, `cue.fire`, `list.focus`; values of the rows the author allowed (§68.2, ANO) |
| Surface | `surface/<id>/…`, `surface/aim`, strips as `slot/<id>` with `kind` = `strip` (`surface`, `index`, `role`, `dca`, `endpoint`, `target`, `word`, `cue`, `holder`) | the strip's `target` by touch, set, release; `strip.press`, `strip.release`; `surface.aim` (a sound only); the aimed cue's EQ and sends |
| Mapping | `videoOutput/<id>/…` (its `grid`, T, tonight's, among them), `zone/<id>/…`, `canvas/<id>/…` | `mesh` by touch, set, release; `node.setMany` for a new grid; `videoOutput.identify`; `videoOutput.grid <output> <on>` (§68.2, ANX) |
| Waveforms | `cue/<id>/hash`, `document/name` | `GET /media/<hash>/timbre?INFO` and `?level=N` |

## 10. What is not here

Discovery (the author chose a typed address, AGAINST mDNS - §68.2, ANN). A password: the WebSocket
takes a GO from anyone on the network (AGB). The canvases' pictures: the projection surface is the
picture (§40.2, XI). Sound peaks: only the timbre pyramid is served (`GET /media/<hash>/timbre`).
Media files: they never leave the machine (§58).
