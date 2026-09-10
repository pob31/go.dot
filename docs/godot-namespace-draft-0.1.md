# Go.dot — Parameter-tree namespace and document schema

**Draft 0.5** — the PRD §9.3 "parameter-tree namespace and node metadata schema". It also
fixes the show-document schema, because the document is the other half of the same
namespace: every node under `/godot/cue` is a projection of an attribute in `show.xml`.
Draft 0.3 added §11, the shape Phase 2 gives the tree, written ahead of its code so that the
Phase 2 pull requests have something to be reviewed against; draft 0.4 added §11.8, what PR 2.1
measured once that code existed. **Draft 0.5 adds §12**, the shape Phase 3 gives the tree —
groups, the run tree, triggers, ranges, MIDI — again written before its code, and records the
four decisions the author took with the Phase 3 plan on 2026-09-06 (L–O in §9).

**This is a living document, and deliberately so.** Go.dot is not a port of something that
already works, the way WFS-DIY was a port of a Max patch — there is no finished parameter
list to transcribe, and pretending otherwise would mean inventing one. So what is fixed
here is the *shape*: how a node is addressed, what metadata it carries, how a mutation
happens, how it is recorded. **What** exists lives in
[`parameters/godot-parameters.csv`](parameters/godot-parameters.csv) and is added to as
each phase lands.

The shape is worth fixing early because it is expensive to change later; the contents are
not, and should not be treated as though they were.

Nothing marked *(proposed)* in the PRD is touched.

---

## 1. Shape of the tree

One root, one owner per subtree (PRD §3.3):

```
/godot/…        the engine, the document, cues, lists, mounts, commands
/wfs/…          a mounted WFS-DIY namespace       (stub in Phase 1)
/xoa/…          a mounted XOA namespace           (stub in Phase 1)
/s21/…          a mounted S21-HiJack namespace    (stub in Phase 1)
/ext/<name>/…   an arbitrary user-added device    (stub in Phase 1)
```

Three rules that shape everything below:

1. **Objects are identity-addressed.** A cue lives at `/godot/cue/<id>` whatever list or
   group contains it and wherever it sits in the order, so a client's `LISTEN` survives a
   reorder and a Choufleur pointer (§3.23) resolves by the same string the operator can
   read off the screen. Order is a separate, read-only node on the container.
2. **Every mutation is a named command** (§4.11). A value write to a node is the
   `node.set` command; everything else is a method node under `/godot/cmd/…`. There is no
   third path.
3. **Go.dot's own metadata rides in one vendor key**, `GODOT`, on every node it owns —
   `KIND`, `RATE_CAP`, `ANTICIPATABLE`, `PANIC` — the four things §3.3 says a node declares.
   The OSCQuery proposal makes custom attributes "intentionally trivial"; clients that do
   not know the key ignore it.
4. **One table generates all four surfaces.**
   [`parameters/godot-parameters.csv`](parameters/godot-parameters.csv) is the single
   source for the document schema, the parameter tree, the RELAX NG schema and the
   OSCQuery reply. WFS-DIY keeps three of those independently and reconciles them with a
   runtime auditor that logs drift after the fact; spatcore's own boundary proposal
   recommends collapsing them, and Go.dot is early enough to simply start that way. A
   parameter absent from that file exists nowhere; one present in it exists everywhere,
   spelled the same.

   The tables in §2 below are therefore a **reading** of that file, kept here because a
   table with prose around it is easier to argue with than a CSV. Where the two disagree,
   the CSV is what the code generates from.

## 2. `/godot`

Types are OSC type tags. `ro` = `ACCESS 1`, `rw` = `ACCESS 3`, containers `ACCESS 0`,
commands `ACCESS 2`. Booleans are `T`/`F` nodes (an `i` 0/1 written to one is accepted).

### 2.1 `/godot/engine` — runtime, read-only, diagnostics

| Node | Type | Meaning |
|---|---|---|
| `/godot/engine/product` | `s` | `Go.dot` |
| `/godot/engine/version` | `s` | `WFG_VERSION` |
| `/godot/engine/tick` | `h` | current tick index (int64) |
| `/godot/engine/sampleRate` | `i` | the clock's sample rate (dummy in Phase 1, observed from Phase 2) |
| `/godot/engine/blockSize` | `i` | |
| `/godot/engine/samplesPerTick` | `i` | `sampleRate / 50` |
| `/godot/engine/lateness` | `i` | last tick's lateness in samples |
| `/godot/engine/latenessMax` | `i` | since start |
| `/godot/engine/clock` | `s` | `dummy` \| `device` |
| `/godot/engine/errorCount` | `i` | rejected commands since start |
| `/godot/engine/lastError` | `s` | `<tick> <seq> <origin> <command> <reason>` — the read-back a client uses to learn that a write was rejected, because OSC has no reply channel |

`GODOT.RATE_CAP` is 5 Hz on this whole container: these are diagnostics, not control.

**What those three clock numbers actually promise** (built in PR 1.4):

- **Tick *n* sits at sample *n* × `samplesPerTick`, exactly.** `samplesPerTick` is
  `sampleRate / 50` with no remainder, and a rate where that division is not exact is
  *refused* rather than rounded — a tick at 882.02 samples drifts a whole sample every
  fifty ticks, so an hour-long show would end 3600 samples from where its log says it was.
  Every rate anybody uses divides exactly, so the refusal costs nothing.
- **`tick` never skips.** Ticks are processed one at a time, in order, with no gaps,
  however far behind the thread falls. The index is the event log's ordering key and a gap
  in it would be a gap in the record of the show. Several ticks coming due at once — one
  long block, one scheduling stall — are processed back to back; the first drains the event
  queue, so the rest usually cost nothing.
- **`lateness` is the part that is *not* exact, and it is reported rather than hidden.**
  The tick thread can only observe the sample counter between blocks, so tick *n* runs
  after the first block whose end reaches its position: late by up to one block, plus
  however long the thread took to wake. The index and the sample position are never wrong;
  this number says how long after the fact the work happened, and `latenessMax` keeps the
  worst rather than an average, because the one tick that ran 40 ms late is the one
  somebody noticed.

A sample-rate change under a running show (§6.2's Dante domain moving) **rebases** the
ratio from a given tick onwards. The tick it lands on stays at the sample it was already
at, so the index sequence stays gapless and increasing across the change and nothing
downstream has to special-case it.

### 2.2 `/godot/document`

| Node | Type | Access | Meaning |
|---|---|---|---|
| `/godot/document/path` | `s` | ro | bundle folder |
| `/godot/document/name` | `s` | ro | bundle name |
| `/godot/document/formatVersion` | `i` | ro | |
| `/godot/document/dirty` | `T`/`F` | ro | unsaved changes exist |

### 2.3 `/godot/list`

| Node | Type | Access | Meaning |
|---|---|---|---|
| `/godot/list/order` | `s` | ro | list IDs in order, space-separated (owner `lists`, derived) |
| `/godot/list/focus` | `s` | rw | ID of the focused list; exactly one; write = `list.focus` (owner `lists`, `persist = state`) |
| `/godot/list/<id>/name` | `s` | rw | |
| `/godot/list/<id>/order` | `s` | ro | IDs of the list's top-level children in order |
| `/godot/list/<id>/standby` | `s` | rw | cue ID, or empty; write = `standby.set` |

One standby per list, engine state, never moved by selection or scrolling (§3.5). Only the
focused list's standby answers `standby.next`/`previous`.

**What PR 1.7 built, and what it deliberately did not.**

- **`/godot/list/<id>/standby` is real, and now carries an invariant**: it names one of *that
  list's own top-level children*, or is empty. Enforced at `ShowDocument::setAttribute`,
  which is the document's single write door — so the standby commands, a client's
  `node.set`, and `state.xml` restoring a saved show are all checked identically. Before
  this the row was a bare string and `node.set … standby banana` was accepted in silence.
- **A direct write is accepted on *any* list**, focused or not. The load path depends on it:
  restoring a show writes every list's standby with no focus involved, and making the node
  writable on only one list would break opening a show with two.
- **`/godot/list/order` and `/godot/list/focus` are built, since PR 3.2.** They were not in
  Phase 1, and the reason is worth keeping: they need a parameter-table row for the
  `/godot/list` **container** rather than for a list, which meant a new owner token, a
  containment entry, container-level address resolution, and a case in both the state writer and
  the RELAX NG generator for an entry that carries no identifier. The author settled focus as
  runtime-only for Phase 1 (2026-09-06) — the smallest thing that makes `standby.next`
  unambiguous — because with one list there was nothing for a focus to be exclusive about.
  Parallel lists are what made it worth the plumbing.
- **What did not change is the resolving.** `focus` is still a request that falls back to the
  first list whenever it names nothing, so creating and deleting lists cannot leave the engine
  pointed at a list that is gone, and "exactly one list is focused whenever a list exists" stays
  true by construction rather than by upkeep. What changed is that the request is now a document
  attribute: a client can read which list GO acts on, a surface can move it, and `state.xml`
  remembers it — the same argument that persisted the standby, applied to the pointer that says
  which standby is being pointed at.
- **The owner token is `lists` and the address segment is `list`**, deliberately.
  `/godot/list/focus` and `/godot/list/<id>/standby` are one container read two ways, and a
  client walking the tree should not have to learn that the collection is spelled differently
  from the things in it. Three address segments rather than four is what tells the resolver
  which was meant. `/godot/run/order` is the same shape and arrived in the same PR.
- **A collection is both a container and a node, and that is why `order` exists.** A client
  cannot assume every child of `/godot/run` is a run — one of them is the roster. The black-box
  driver found this the honest way: it listed the container's children before the first GO and
  reported a run called `order`.
- **The standby moves when the show moves under it.** Deleting the cue it is parked on
  advances it to the next remaining top-level sibling, or empties it if there is none;
  moving that cue out of the list's top level clears it. Both happen *inside the applied
  command*, so a replay reproduces them with no repair record in the log. Reordering within
  the list moves nothing: the pointer stores an identifier.

### 2.4 `/godot/cue` — every cue and every group, flat, by ID

| Node | Type | Access | Meaning |
|---|---|---|---|
| `/godot/cue/<id>/kind` | `s` | ro | `memo` \| `group`. Derived from the element, never stored: a Group *is* a group, so a client cannot turn one into the other by writing here |
| `/godot/cue/<id>/number` | `s` | rw | the decimal cue number, a *string* (`12`, `12.5`, `12.5.1`), mutable, human protocol (§3.5) |
| `/godot/cue/<id>/name` | `s` | rw | |
| `/godot/cue/<id>/notes` | `s` | rw | |
| `/godot/cue/<id>/enabled` | `T`/`F` | rw | |
| `/godot/cue/<id>/colour` | `s` | rw | `#RRGGBB`; decoration, never the sole carrier (§4.8) |
| `/godot/cue/<id>/parent` | `s` | ro | ID of the containing list or group |
| `/godot/cue/<id>/index` | `i` | ro | position among siblings, 0-based |
| `/godot/cue/<id>/order` | `s` | ro | groups only: child IDs in order |
| `/godot/cue/<id>/mode` | `s` | rw | groups only: `timeline` \| `sequence` (§3.6) |
| `/godot/cue/<id>/advance` | `s` | rw | groups only: `auto` \| `manual` |
| `/godot/cue/<id>/preWait` | `d` | rw | seconds. Any cue, and a group has one of its own — see *Waits compose* below |
| `/godot/cue/<id>/postWait` | `d` | rw | seconds. §3.6's "how long after completion this cue reports done to its parent" |

Groups carry no outputs, media or parameters (§4.12); Phase 1 stores their structure and
attributes so fixtures can nest, and nothing more. Question C in §9 fixes what
`standby.next` does with them.

#### Waits compose, they do not replace

A group has a pre-wait and a post-wait **of its own**, and they wrap its members'
rather than standing in for them. Recorded here because Phase 3 implements it and the
alternative reading — a group's wait replacing its members' — is the one someone would
reach for from the schema alone.

- **Pre-wait.** The group's own runs first; only then do its members begin theirs. In a
  timeline group, where §3.6 makes member pre-waits *offsets* from entry, a member
  therefore fires at `group entry + group preWait + member preWait`. The useful
  consequence, and the reason it works this way: raising one number defers a whole scene
  by the same amount, without touching the relative timing anyone spent an afternoon
  getting right.
- **Post-wait.** A group is complete when every member is — each member's own post-wait
  included, since that is what "done" means for a cue (§3.6's completion table). The
  group's post-wait then runs on top of that, before the group reports done to *its*
  parent. So a nested group's waits stack outward, one layer per level, which is what
  makes "hold two seconds after this whole block" expressible at any depth.

Both are inert where §3.6 says they are: a post-wait is meaningful in a sequence and does
nothing in a parallel parent, because nothing is waiting to be told.

### 2.5 `/godot/mount`

| Node | Type | Access | Meaning |
|---|---|---|---|
| `/godot/mount/<id>/prefix` | `s` | ro | e.g. `/wfs` |
| `/godot/mount/<id>/transport` | `s` | ro | `udp` \| `tcp` \| `ws` — declared now, used from Phase 2 (question F) |
| `/godot/mount/<id>/namespace` | `s` | ro | bundle-relative file, e.g. `namespaces/wfs-diy.json` |
| `/godot/mount/<id>/rateCap` | `d` | ro | Hz, default for the mounted nodes |
| `/godot/mount/<id>/anticipatable` | `T`/`F` | ro | default `F` for third parties (§3.3) |
| `/godot/mount/<id>/panic` | `s` | ro | mount-level default (question F) |
| `/godot/mount/<id>/loaded` | `T`/`F` | ro | |
| `/godot/mount/<id>/nodeCount` | `i` | ro | |
| `/godot/mount/<id>/host` | `s` | ro | where the target is; default `127.0.0.1` (PR 2.5) |
| `/godot/mount/<id>/port` | `i` | ro | **required, no default** — where it sends (PR 2.5) |
| `/godot/mount/<id>/sent` | `i` | ro | messages that have left for this target (PR 2.5) |

**Added in PR 2.5, and the reason `port` is required.** Phase 1's table had no destination in
it at all, which was correct while nothing was sent and a gap the moment something was. `port`
follows `audio/@tracks` — required, no default — for a harder reason than "no number is right
for every rig": UDP never reports that nobody was listening, so a mount that guessed would send
into the dark and report success for a whole show. There is no later moment at which the engine
could find out, so it is found out when the file is read. `host` defaults because the ordinary
rig is Go.dot and its processors on one box, and it is a literal address rather than a name
because a socket re-resolves whenever the destination changes and a blocking lookup on the tick
thread is a frame nobody gets back.

The mounted namespace itself appears at the prefix (`/wfs/…`), not under `/godot/mount`.

**Settled while building PR 1.6:**

- **The prefix is where the description's ROOT lands, and a description may be of a
  subtree.** WFS-DIY publishes everything under a `/wfs` container of its own, so a capture
  of `GET /` mounted at `/wfs` would give `/wfs/wfs/input/1/positionX`. Capturing `GET /wfs`
  gives a description whose root is `/wfs`, and mounting that at `/wfs` gives the addresses
  anybody expects. The reader takes either; the mounted address is always the prefix plus the
  nesting, and the root's own `FULL_PATH` is used only to check the file against its own
  shape.
- **A prefix of `/` is refused**, along with a relative one, a trailing slash and an empty
  segment. Mounting at the root would put somebody else's namespace on top of `/godot`.
- **`RANGE` entry zero is the one that is kept** for a multi-argument node. `RANGE` carries
  one entry per argument, so entry zero really is the first argument's — for WFS-DIY's
  `EQgain`, typed `if`, that is the band index and not the gain. Correct rather than a
  simplification, though arguments two onwards lose their bounds.
- **A reload forgets whatever was written to that mount.** The namespace may have changed
  shape underneath, and carrying a value across would assert something nobody checked.
- **`bad-namespace`** joins the reason codes: the mount was named correctly and what failed
  is the file it points at, which is somebody else's and is the thing to go and look at.
  Distinct from `unknown-id` and from `bad-address` for that reason.

### 2.6 `/godot/cmd` — commands as write-only method nodes

`TYPE` is the parameter signature; `DESCRIPTION` is the command's. Sending an OSC message
to the node invokes the command; the same names are what the CLI and the event log use.

| Command | Node | Params | Notes |
|---|---|---|---|
| `noop` | `/godot/cmd/noop` | — | the skeleton's first command; a heartbeat in a log |
| `document.load` | `/godot/cmd/document/load` | `s` bundle path | logs the loaded bundle's SHA-256 |
| `document.save` | `/godot/cmd/document/save` | — | |
| `document.saveAs` | `/godot/cmd/document/saveAs` | `s` bundle path | |
| `list.create` | `/godot/cmd/list/create` | `s` name `[s id]` | the id is optional; the engine generates one and **logs the event with it** |
| `list.delete` | `/godot/cmd/list/delete` | `s` id | |
| `list.focus` | `/godot/cmd/list/focus` | `s` id | |
| `group.create` | `/godot/cmd/group/create` | `s` parent `i` index `s` name `[s id]` | parent is a list or a group |
| `group.delete` | `/godot/cmd/group/delete` | `s` id | deletes the subtree |
| `cue.create` | `/godot/cmd/cue/create` | `s` parent `i` index `s` kind `s` name `[s id]` | |
| `cue.delete` | `/godot/cmd/cue/delete` | `s` id | |
| `cue.move` | `/godot/cmd/cue/move` | `s` id `s` newParent `i` newIndex | also moves groups |
| `node.set` | *the node's own address* | the node's type | a value write **is** this command; it has no `/cmd` node because its signature is the target's |
| `node.touch` | `/godot/cmd/node/touch` | `s` address | per origin (question D) |
| `node.release` | `/godot/cmd/node/release` | `s` address | |
| `standby.set` | `/godot/cmd/standby/set` | `s` cue id | on the focused list; the cue must be one of its top-level children |
| `standby.clear` | `/godot/cmd/standby/clear` | — | |
| `standby.next` | `/godot/cmd/standby/next` | — | |
| `standby.previous` | `/godot/cmd/standby/previous` | — | |
| `mount.load` | `/godot/cmd/mount/load` | `s` mount id | (re)reads the namespace file |

Decision recorded here: the plan listed `list.set`, `group.set`, `cue.set`; they collapse
into `node.set`, since a property edit is a node write and one path is better than two.

**The standby commands as PR 1.7 built them** (author decisions, 2026-09-06):

| Command | Applied when | Refused when |
|---|---|---|
| `standby.set` `s` cue | the cue is a top-level child of the focused list — a Group is a legal target, since a Group is a Cue. Setting the one it already holds is applied | `unknown-id` if nothing has that identifier, or it names a list or a mount rather than a cue; `not-in-list` if the cue exists but is nested or belongs to another list, or if there is no list at all |
| `standby.clear` | always, including when it is already empty — an empty standby is a resting state (§3.5), not a failure | `not-in-list` when the show has no list |
| `standby.next` | always, **including when it does not move**: at the end of a list, and from an empty standby, the pointer stays put and the record is `A`. There is a list and the command did what it does | `not-in-list` when the show has no list |
| `standby.previous` | the mirror of `next` | as `next` |
| `list.focus` `s` list | the identifier names a list. Exclusive by construction: focus is one value, so there is no flag to leave set | `unknown-id` if nothing has that identifier or it is not a list. A refused request leaves the previous focus exactly where it was |

**`next` and `previous` stay put from empty** — only `standby.set` arms a list. There is no
wrap at either end, which is what the end-of-list rule is for.

**A disabled cue is not skipped** in Phase 1. A disabled cue is still a row in the list, and
skipping is a running-behaviour decision that Phase 1 has no runner to justify; Phase 3
revisits it when a GO that does nothing becomes a real failure rather than a hypothetical.

**Two rejection codes gained cases**, recorded here because §2.6 calls these rules fixed:

- **`not-in-list`** now also means *there is no list to act on* (the argument-less standby
  commands on a show with no lists) and *that cue is not at the top level of the list being
  written* (both the command and the `node.set` door). It was previously unused.
- **`unknown-id`** now also covers *a known identifier naming the wrong kind of object* —
  `standby.set` given a mount, `list.focus` given a cue. The alternative was to invent a
  code for it, and "there is no cue with that id" is what the caller needs to hear.

**Rejection rules** (fixed, because the log records outcomes): unknown command, unknown
id, bad address, read-only node, type mismatch other than `i`↔`f`, `standby.set` outside
the focused list, a retired id offered to `create`, a mount whose namespace file will not
read (`bad-namespace`) → the event is rejected, logged as `R` with a reason code, and
surfaced at `/godot/engine/lastError`.

**Three things PR 1.5 settled while building this** (built and tested; say so here if any
should be otherwise):

- **`node.set`'s value argument is declared `*`** — "whatever the target says". Its
  signature is the target node's, which is why it has no `/cmd` node, and the registry
  cannot know the type until the address is resolved. Nothing is loosened: every value
  becomes canonical text and the schema parses it against the row the address resolves to,
  so a client sending the string `"3"` to an integer node and one sending the integer `3`
  produce the identical document, and neither can put a word into a number. The check moved
  one layer in, to where the type is known.
- **Writing to a derived node is `read-only`, not `bad-address`.** `kind`, `parent`, `index`
  and `order` are `persist=none`: the document does not hold them, so an address resolver
  that only knew about stored attributes called them bad addresses. But the tree publishes
  them, so a client that reads the namespace will write to one — and the address is not what
  is wrong with the request. The schema now keeps its derived rows beside its stored ones so
  the refusal can name the real reason.
- **Only a `state` node carries the full `GODOT` key.** `RATE_CAP`, `ANTICIPATABLE` and
  `PANIC` are statements about a *value*: a container has none, and an event has none at any
  given time. `"PANIC": "park"` on `cue.create` would be filling in a form rather than
  saying something, and a client reading it would be entitled to believe it. Containers and
  events declare `KIND` and stop.

**Built in PR 3.2, and the note is kept rather than deleted** because the reason they were
absent is the useful part: `/godot/list/order` and `/godot/list/focus` had no rows in the
parameter table, and a table row is what makes a node exist. The rows they needed belong to the
`/godot/list` **container** rather than to a list, which is a different kind of owner from any
that existed — see §2.3.

## 3. Node metadata — the `GODOT` key

```json
"GODOT": { "KIND": "state", "RATE_CAP": 50, "ANTICIPATABLE": false, "PANIC": "park" }
```

| Key | Values | Meaning (PRD §3.3) |
|---|---|---|
| `KIND` | `container` \| `state` \| `event` | settable state has a value at time *T*; an event is one-shot and has none |
| `RATE_CAP` | Hz, number | cap on outbound dispatch; pushes are coalesced to the tick and then rate-capped per node |
| `ANTICIPATABLE` | boolean | may be pre-sent before GO; third-party default `false` |
| `PANIC` | `"park"` \| `"snap"` \| a JSON array (the declared safe `VALUE`) | the resting state §4.6 requires every parameter to have |

For a mounted node the values come from the mount's declaration unless the namespace file
carries its own `GODOT` key — which it may, so that a hand-written template and a captured
one are indistinguishable to the engine (§3.22). Without a key, `KIND` is inferred:
`ACCESS` write-only and no `VALUE` → `event`, otherwise `state`.

## 4. Change notification

- A value change on a listened node → one binary OSC message per tick, at most, to each
  listener, **except** the origin that caused it (echo suppression, §3.16) and any origin
  currently touching that node (question D).
- `cue.create`/`group.create` → `PATH_ADDED /godot/cue/<id>`; `*.delete` →
  `PATH_REMOVED`; both also push the parent's `order` and the siblings' `index`.
- `cue.move` and reorders → value pushes on `order`/`index`/`parent`; no `PATH_*`, since the
  flat `/godot/cue` container did not change.
- `mount.load` → `PATH_CHANGED <prefix>`.

## 5. Identifiers

- 8 characters, Crockford base32, uppercase: `[0-9A-HJKMNP-TV-Z]{8}` — no `I`, `L`, `O`,
  `U`, so an ID reads unambiguously over comms and survives a handwritten cue sheet.
- 40 bits from `std::random_device`; uniqueness checked within the document at creation;
  every object element carries one (`List`, `Group`, `Cue`, `Mount`).
- Valid verbatim in OSC addresses and XML attributes.
- Cue **numbers** are a different thing entirely (§3.5): mutable strings, renumbered during
  tech, never an identity.

## 6. The bundle and its files

```
MyShow/
  MyShow.wfg           manifest — the file you double-click; carries formatVersion only
  show.xml             what someone decided (§4.10)
  state.xml            ephemeral engine state (question A)
  namespaces/          OSCQuery namespace files the mounts read
    wfs-diy.json
    console.json
```

No timestamps, no writer version, no machine state anywhere: load → save is
byte-identical.

### 6.1 Canonical XML

UTF-8, `\n` line endings, one element per line, two-space indent, attributes sorted by
name, attributes at their default omitted, numbers in the shortest form that reads
back identically (see §9), booleans `true`/`false`.

That last one has a visible consequence worth stating: `1.0` is written `1`, and
`1000000` is written `1e+06`. The value is exact either way — the shortest form is
chosen precisely because it round-trips — and every quantity a show actually carries
(seconds, decibels, metres, hertz) is inside the range where the plain form is
shorter, so the exponent only appears for values no cue would have.

### 6.2 `show.xml`

Attributes come from [`parameters/godot-parameters.csv`](parameters/godot-parameters.csv)
— the rows whose `persist` column is `show`. The table below is that file read back as a
grammar, and it grows with it.

| Element | Attributes (type, default) | Children |
|---|---|---|
| `Show` | `formatVersion` int (1) | `Lists`, `Mounts` |
| `Lists` | — | `List*` |
| `List` | `id`, `name` string | `(Cue \| Group)*` |
| `Cue` | `id`; `number` string; `name` string; `notes` string; `enabled` bool (true); `colour` string; `preWait`, `postWait` double seconds (0) | — |
| `Group` | every `Cue` attribute, plus `mode` enum (`sequence`) and `advance` enum (`manual`) | `(Cue \| Group)*` |
| `Mounts` | — | `Mount*` |
| `Mount` | `id`; `prefix` string; `transport` enum (`udp`); `namespace` string; `rateCap` double Hz (50, cap 3); `anticipatable` bool (false); `panic` enum (`park`) | — |

Example, canonical:

```xml
<Show formatVersion="1">
  <Lists>
    <List id="7K2QM9X4" name="Main">
      <Cue id="B3N8R5TW" name="House to half" number="1"/>
      <Group id="D9FH2JKA" name="Preshow" number="2">
        <Cue id="E4GP6QSC" name="Walk-in" number="2.1"/>
        <Cue id="F7HR8TVD" name="Announce" number="2.2"/>
      </Group>
    </List>
  </Lists>
  <Mounts>
    <Mount id="G1JS4VWE" namespace="namespaces/wfs-diy.json" prefix="/wfs"/>
  </Mounts>
</Show>
```

### 6.3 `state.xml`

What the machine happened to be doing, which PRD §4.10 keeps out of the document. Its
attributes come from the same table as show.xml's — the rows whose `persist` column is
`state` rather than `show` — so adding a piece of ephemeral state is a CSV edit and no
code changes.

Flat, one entry per object that has something to remember, found by identifier:

```xml
<State formatVersion="1">
  <List id="7K2QM9X4" standby="B3N8R5TW"/>
</State>
```

An object with nothing to remember is left out entirely, so a show with four hundred cues
and one standby is two lines.

**This corrects draft 0.1**, which drew a single `<Standby cue= list=>` element and a
`<Focus>` beside it. Standby is **one per list** — the parameter table has said so since the
CSV was written, and its description says it in words — so it is an attribute of a `List`
and not a document-level singleton. Focus is not in the table at all yet; it arrives as a
row in PR 1.7, and lands in this file automatically when it does, because nothing here
enumerates what state.xml may contain.

The rule is enforced in both directions: the canonical writer refuses to put a `persist=state`
attribute in show.xml, and the show reader refuses to read one, saying which file it belongs
in rather than quietly moving it. A `standby` in show.xml was either hand-edited or written
by something that did not know the split, and silently repairing it would hide which.

### 6.4 `MyShow.wfg`

```xml
<Bundle formatVersion="1"/>
```

### 6.5 RELAX NG

[`docs/schema/show.rng`](schema/show.rng) is generated from the engine's `Schema` table by
`wfg schema --out=<file>` and committed; `wfg schema --check=<file>` fails when the two have
drifted apart, and CI runs it under both locales.

**One grammar, three roots.** `start` is a choice of `Show`, `State` and `Bundle`, so a single
file describes every XML file a bundle contains and one validator run covers all of them.

**Why generate it at all**, when the engine already validates a document against its own
schema: because that check and the schema are the same code, so it can prove the engine is
self-consistent and nothing more. `scripts/validate-show.py` runs the published grammar
through lxml, which shares no code with us — an outside opinion, and the only thing that
can catch a mistake our reader and our schema both make. It is also the pre-commit hook
§3.20 asks for, and it is what anybody outside this repository can run without building
anything.

Everything except `id` is optional in the grammar, and that is not laxness: the canonical
writer omits an attribute holding its default, and an absent attribute reads back **as** its
default. The grammar says which values are legal, not which are present.

## 7. The event log

The tick-indexed path of §3.15, as a text file, one record per line:

```
# wfg-log 1
# bundle MyShow sha256:<hex over show.xml, state.xml, namespaces/*>
# clock sampleRate=48000 blockSize=128 samplesPerTick=960
A 0 0 cli document.load s:"D:/shows/MyShow"
A 12 1 ws:192.168.1.20:51234 node.set s:"/godot/cue/B3N8R5TW/name" s:"House to half"
A 12 2 ws:192.168.1.20:51234 cue.create s:"7K2QM9X4" i:3 s:"memo" s:"Blackout" s:"H5KT9WXF"
R 13 3 udp:192.168.1.7:9000 read-only node.set s:"/godot/cue/B3N8R5TW/kind" s:"group"
X 13 4 udp:192.168.1.7:9000 truncated b:LyIvAAAsZgAA
A 40 5 cli standby.next
```

- `A` applied, `R` rejected (reason code before the command), `X` transport-level drop
  (never replayed). `seq` is monotonic across kinds.
- Atoms: `i:` `h:` `f:` `d:` `s:"…"` (escapes `\"` `\\` `\n`) `b:<base64>` `T` `F` `N` `I`
  `t:<uint64>`. Floats and doubles use the same formatter as the document; `f` parses as
  double then narrows.
- Records are written **as applied**: a generated id appears as the last argument of the
  `create` that produced it, so replay never needs randomness.
- Replay re-executes every `A` and `R` at its tick on a manual clock, checks that each `R`
  is rejected again, and must reproduce the saved bundle byte for byte.

**The `X` reason is one of ten atoms, not the single `malformed-packet` this draft first
showed** (built in PR 1.8). A datagram that never became a command is dropped with a
kebab-case token naming *which guard refused it*:

| Atom | What arrived |
|---|---|
| `not-osc` | empty, or not a multiple of four bytes — everything OSC contains is padded to four, so this is a datagram that lost its tail |
| `bad-address` | the address is unterminated, not absolute, has an empty part, or uses one of the nine reserved characters |
| `address-is-pattern` | a well-formed address *pattern* — `/godot/cue/*/name`. Phase 1 resolves an address to exactly one node, and a client that sends a pattern has asked for something Go.dot does not do. Told apart from `bad-address` deliberately: the two need different answers |
| `no-type-tags` | the type-tag string is absent, or does not begin with `,`. OSC 1.0 let it be omitted and 1.1 does not — treating an absent one as "no arguments" turns a corrupted first byte into a plausible empty message |
| `unknown-type-tag` | a tag outside `i h f d s b T F N I t`. Refused, never skipped: the payload size is a property of the tag, so a reader that stepped over one would desync and every later argument would be fiction |
| `truncated` | a message promised an argument the packet does not contain |
| `bad-blob` | a blob's declared length is negative, so no packet length could satisfy it |
| `bad-bundle` | the `#bundle` marker, the time tag, or an element size that does not fit the bundle declaring it |
| `too-deep` | nesting past 32 levels. A bundle costs about twenty bytes a level, so a 64 KB datagram would otherwise buy several thousand stack frames |
| `trailing-bytes` | bytes after the last argument the type tags accounted for |

The **prose** that goes with each — "a float32 argument runs past the end" — is kept beside
the atom and shown to the operator, but it is not what the log records. A log column that
is a sentence is one nobody can group or count, and one that silently stops matching the
day the wording improves. The atom is a closed set and the sentence is free to be reworded.
For the same reason the offending tag is named in the sentence and not in the atom: an atom
that varied with the input would be an unbounded column whose cardinality an attacker
chooses.

The payload rides along as a `b:` atom. It is the only copy — the datagram is gone, and a
post-mortem with no packet in it is a guess.

## 7a. The OSCQuery surface, as PR 1.9 built it

This draft described the tree and never the protocol that carries it. What follows is what
was built; say so here if any of it should be otherwise.

**`GET <path>`** returns that node and everything under it, as the JSON of §2. `GET /` is
the whole tree. **`GET <path>?<ATTR>`** returns one attribute as a JSON object holding just
that key.

**Four answers, not two**, because OSCQuery asks four different questions and a client is
entitled to tell them apart:

| Status | Means |
|---|---|
| **200** | here is the node, or the attribute |
| **404** | nothing lives at that address |
| **400** | that is not an OSCQuery attribute — the node may be perfectly real |
| **204** | the node is real, the attribute is real, and this node does not carry it |

The last is the one worth being careful about. A container has no `VALUE`; a string has no
`RANGE`. Answering 404 would tell a client the node had gone away, and a JSON `null` would
tell it the value *is* null. 204 is the only honest one of the three.

**`ACCESS` is the one attribute that can never answer 204** — every node has one, including
a container.

**`CLIPMODE` is permanently 204, and that is a statement.** Go.dot does not clip: a write
outside a declared range is REJECTED and logged as an `R`, because a cue that silently
became a different cue is worse than one that refused. Answering `"none"` would be a claim
about clipping behaviour a client might then rely on.

**A pattern is refused as a pattern** (400), never as a missing node. Phase 1 resolves an
address to exactly one node, so a client that put a star where a cue identifier belongs has
asked for something Go.dot does not do — and 404 would send it hunting for a typo in an
address that is spelled correctly.

**`?HOST_INFO`** carries `NAME`, `OSC_PORT`, `OSC_TRANSPORT "UDP"`, `WS_PORT` (the same
port as HTTP) and `EXTENSIONS`. The absent extensions are as load-bearing as the present
ones, because a client reads that block to decide what not to try: `CRITICAL` is false
(Phase 1 speaks OSC over UDP only) and `PATH_RENAMED` is false because Go.dot never renames
a path — objects are identity-addressed, so a rename changes a `name` VALUE and the address
is untouched, which is the whole reason a client's `LISTEN` survives an edit.

**The WebSocket** is the same port. Text frames are `{"COMMAND": "LISTEN"|"IGNORE", "DATA":
"<address>"}`; binary frames are OSC, in both directions. A write arriving on it takes the
same road as one arriving over UDP: `/godot/cmd/…` is a command, anything else is
`node.set`, and there is no third case.

**Pushes are coalesced to the tick** and carry the value the node HAS at that tick, not the
succession it passed through during it — a node written forty times in one tick produces
one push. They are withheld from the origin that caused the change, and from any origin
holding the node.

**Suppression needs a single cause, and says so when there is not one.** The engine reports
the origin of a tick's applied events only when they all share one. With two writers in one
tick there is no single cause, and blaming either would withhold a change it did not make —
leaving a surface stale with nothing to correct it. So: suppress when the cause is
unambiguous, send to everybody when it is not. The cost of being wrong that way is one
redundant push. Per-address attribution would remove even that and belongs with Phase 6's
real surfaces.

**A malformed frame is dropped and never forwarded.** It never became a command, so there
is nothing to reject; it becomes an `X` record carrying the sender, the refusal atom and
the bytes.

**mDNS is not implemented.** Clients are pointed at a host and a port.
`juce::NetworkServiceDiscovery` is not mDNS and would advertise to nothing that speaks
OSCQuery.

**Three commands this draft did not list**, added because the things they name are things
that happen:

- **`node.releaseAll`** — everything one origin holds, in one command. §3.16 requires a
  disconnect to release what a surface held, or one that crashed mid-gesture leaves a node
  gated against everybody for the rest of the show. `node.release` takes one address, and a
  disconnect is one event rather than a list of them. Routed through the queue like any
  other command, so the release is in the log and a replay reproduces it.
- **`document.save`** — writing the show back out. §4.11 admits no exceptions and saving is
  a gesture; it is also the one an OSCQuery client has no other way to ask for, since there
  is no node whose value is "saved". A failed write is REJECTED rather than reported: `A`
  means it happened, and a save that did not reach the disk did not happen.
- **`document.load`** is still not built. `serve` takes its bundle on the command line, and
  loading a second show into a running engine is a Phase 5 question about what happens to
  everything pointing at the first.

---

## 8. Ports (decided, overrule early)

| Purpose | Port | Why this number |
|---|---|---|
| OSCQuery HTTP + WebSocket | **5010** | WFS-DIY, which runs on the same machine, is on 5005; XOA and Tight-WFS are not settled. Go.dot takes the "+10" block. |
| OSC over UDP | **8010** | WFS-DIY is on 8000/8001 |
| OSC over TCP | 8011 | reserved, Phase 2 |
| MCP | 7410 | reserved; WFS-DIY is on 7400 |

Tests never use these numbers: they bind port 0 and read the bound port back.

## 9. Decisions, and when each one has to be made

Front-loading every decision would be the wrong shape for a project that is still finding
out what it is. Two below are settled because measurement settled them; the rest are
deliberately left open, each with the subphase that forces it and a recommendation to fall
back on if nobody feels strongly by then.

### Settled

- **Number precision — shortest round-trip.** PRD §3.20 says "fixed precision", which was
  written to mean "not whatever the locale does". Measured over 19 993 random doubles,
  JUCE's writer loses 46% of them to a save-and-load round trip, because it stops at
  fifteen significant digits. `std::to_chars` loses none. So every number Go.dot writes —
  document, log, OSCQuery reply — is the shortest text that reads back as the identical
  value, which satisfies §3.20's intent more strictly than a fixed decimal count would.
  Consequence: an integral double writes as `1`, not `1.0`. The type is never carried by
  the text (the log tags its atoms, the CSV declares its attributes), so nothing is lost.
  This is what put the macOS floor at 13.3.

  The reader is a separate trap and cost a red CI run to find. `std::from_chars` is absent
  for floating point on the macOS toolchains this project builds on, and the obvious
  substitute — an `istringstream` imbued with `std::locale::classic()` — rejects every
  subnormal on libc++, because `num_get` is specified to set `failbit` when the conversion
  sets `errno`, and `strtod` sets `ERANGE` on underflow. Go.dot parses with `strtod_l`
  against a C locale created once, and asks whether the result is finite and the whole
  field was consumed rather than whether a stream flag is set.

- **Identifiers — 8-character Crockford base32.** `[0-9A-HJKMNP-TV-Z]{8}`, 40 bits from
  `std::random_device`, unique within the document at creation.

- **C — A group is an opaque sibling in Phase 1** (settled 2026-09-06, in PR 1.7): the
  standby pointer steps over a group rather than descending into it. PRD §3.6 says the
  pointer descends into a manual sequence group, and Phase 3 implements that — a Phase 1
  group has no runtime behaviour to descend into, and §2.6's `standby.set` constraint already
  requires a top-level child. The test that asserts it is named for the choice rather than
  for a rule, so that when Phase 3 makes it fail, the failure is the point.

- **B — Deleted identifiers are not retired** (settled 2026-09-05): "reusing is not such a
  problem, we can skip tombstones". So there is no `Retired` element, deletion forgets, and
  the document carries nothing to record what is no longer in it.

  What the guarantee actually is, stated plainly rather than overclaimed: an identifier is
  unique among the objects that exist, and a fresh one is drawn from 40 bits, so *reissuing*
  a number that some deleted object once held is possible and vanishingly unlikely. PRD
  §3.5's "never reused" is therefore honoured in practice and not enforced in the file. The
  cost, if it is ever felt, is that a Choufleur note (§3.23) pointing at a deleted cue shows
  as unknown rather than as deleted — the two are indistinguishable without a tombstone.
  Adding one later changes nothing already written, which is why this was safe to decide
  quickly.

- **G — The fixed track count lives in the document** (settled 2026-09-05, for Phase 2):
  `Show/Audio/@tracks`, required, with no default anywhere in the tree. It is the polyphony
  ceiling of PRD §3.25 and it is something someone decided (§4.10), so the show says it and a
  new show has to say it. Every fixture states its own.

- **H — A GO on a media cue that is already running is ignored** (settled 2026-09-05):
  the `go` is applied and logged, standby advances, no second run is created and the running
  instance continues. PRD §3.8's per-cue-type policy may revisit this later; restart and
  second-instance were the alternatives offered.

- **D — Touch gating as described** (settled 2026-09-05, in PR 1.9): `node.touch` and
  `node.release` per origin, pushes withheld from the touching origin, `node.releaseAll` on
  disconnect. Built as drawn, with one addition the draft had not anticipated: the same
  question decides echo suppression, so both live behind one call rather than in two files
  that could come to disagree.

- **F — The mount attribute set as drawn** (settled 2026-09-05, in PR 1.6): `transport`
  declared now and used from Phase 2, a mount-level `panic` default with per-node
  overrides. Nothing in building it argued for a different set.

- **A — Standby survives a save and a load** (settled 2026-09-05, in PR 1.3, the way the
  plan recommended): a rehearsal reopened where it was left is the kinder default. It is one
  column of one CSV row — `list/standby` carries `persist=state` — so reversing it is an edit
  to the table and no change to any code.

  The other half matters as much: LOSING `state.xml` COSTS ONLY THE STANDBY. A bundle
  without one opens silently with every ephemeral value at its default, and an entry naming
  an object the show no longer contains is reported and skipped. A show must never become
  unopenable because of a file describing where somebody had got to in it.

- **I — Audio backends as WFS-DIY builds them** (settled 2026-09-05): `JUCE_ASIO=1` behind a
  `WFG_ASIO_SDK` path variable (the SDK is not redistributable, so without a path the build
  is WASAPI/DirectSound only) and `JUCE_JACK=1` on Linux with `libjack-jackd2-dev` in the
  package list. CoreAudio needs nothing.

- **L — The §3.24 proposals touching Phase 3** (settled 2026-09-06, with the Phase 3 plan):
  ranges **may be discontiguous and in any file order** — a media cue is then a playlist over one
  file — and **an edit to a running cue's ranges takes effect at the next iteration**. Crossfaded
  joins and tag targeting for stop cues (§3.8) are **not built**; both stay *(proposed)*.

- **M — GO on a manual sequence group fires its first member** (settled 2026-09-06): the pointer
  lands on the second member — QLab's "start first child and enter" — and leaves the group to its
  next sibling the moment the last member of the last round fires, so no GO is ever spent on
  leaving. The literal reading of §3.6's "GO past the final iteration completes the group", in
  which the pointer wraps and one more GO exits, was offered and declined; that sentence goes to
  the author as an amendment. An infinite manual loop is left by `afterIteration`, `advance` or
  `run.stop`.

- **N — A refire is decided per kind** (settled 2026-09-06, PRD §6.6): **ignored** for media (H)
  and for groups; a **restart** for a fade or a stop, which takes over from the level its target
  is at; a **second instance** for an osc, midi or memo cue. "Ignore for every kind" was the
  recommendation and was declined. A stop or fade aimed at a cue with several live runs acts on
  the newest.

- **O — A fade aimed at a group is a trim** (settled 2026-09-06): a run's level becomes
  `base + Σ trims` and a group run's `level` is a trim over its members, which is what §3.6
  already says ("trim, not write… nested trims compose"). Built in PR 3.12. Relative fade *cues*
  — a delta rather than a destination — remain a PRD amendment for the author; the structure that
  will carry them is this one.

- **P — The slot pool is declared in the document** (settled 2026-09-07, with the Phase 4 plan):
  `Mount/Slot` names a processor's input, its width, the bus that feeds it and where in that bus it
  begins; `Show/Audio/Rack` declares the rack's channels the same way. PRD §3.9b's *(proposed)*
  "the processor declares its own slots" — Go.dot discovering inputs over OSCQuery — becomes a
  later authoring gesture that **writes** these rows rather than a mechanism that replaces them,
  and `wfg validate` warns when a slot names an address the mounted namespace does not contain.
  How many inputs of a processor a show is using is something somebody decided (§4.10), and a pool
  that changed when somebody reconfigured a processor would change a show nobody had edited.

- **Q — A preset is a mark on the member** (settled 2026-09-07): `cue/@preset` names the **ancestor
  group whose header prepares this cue**, and the header line is derived rather than written. The
  author's picture is dragging a cue onto a header at one level or another of the groups it sits
  inside, with a mark on its own row and a tendril to the header it went to; written header cues
  stay, since "we may have to add cues manually too". This answers all four questions
  `godot-open-questions-0.1.md` §5 left open — where the preset lives, what editing a derived line
  does, their order among the written ones, and what happens when the member goes — because a
  derived line has no existence of its own to answer them about.

- **R — Waypoints are invisible** (settled 2026-09-07): there is no authored waypoint object.
  Structural waypoints stay inside the solver, and what an operator gets is the list's own history
  of steps, kept without being asked for and offered when they go back. The author's words: *"If
  this could be invisible to the user this would be great, something they don't have to manage but
  can suggest several states to pick from when needed… something that keeps track of a history in
  the various steps of the cuelist."* A mark on a group, and a waypoint cue kind holding captured
  values, were both offered and set aside.

- **S — The persistent section is the list's, and a kill suspends** (settled 2026-09-07):
  `List/Persistent` holds media, osc and midi cues, asserted after every applied trigger through
  the solver (§13.11). PRD §3.29's *(proposed)* "a kill from the running pane suspends a persistent
  assertion" is taken as **yes** — run-local, for the session — because without it the operator
  fights the machine, and every other run-local gesture here already evaporates the same way. A
  group carrying its own persistent section, and Esc as a pause on persistent media, stay
  *(proposed)*.

- **T — Phase 5 has two halves, and the engine half comes first** (settled 2026-09-09): undo, a
  save that cannot be half-written, an autosave, the edit lock and the spectral cache are built
  before the client that draws them, because none of them exists and every client needs all of
  them. The client half runs beside it in `clients/console` — one view per pull request, each
  earning a round of the author's feedback — rather than in a new compiled program, because PRD
  §3.17 says the desktop layout is *deliberately undesigned* and the author's to design, and he
  designs by looking. A page the engine serves from disk can be edited and refreshed while a show
  is running; a compiled client cannot, and the layout would be settled by whoever typed it. The
  JUCE desktop client starts when the layout stops moving; until then it is an outline (§14.16).

- **U — Phase 5's done-when is judged in the room** (settled 2026-09-09): the devplan's criterion
  stands unchanged — *the author runs a simple show from the desktop build in a rehearsal room* —
  and whether a browser on the booth machine satisfies it is answered when the page is in front of
  him rather than now. The alternative, deciding in advance that only a native window counts, would
  commit the phase to a build whose layout nobody has yet agreed on.

- **V — The console becomes the web client of §3.17** (settled 2026-09-09): it is not a prototype
  to be thrown away when a desktop client arrives. PRD §3.17 makes a web client the tablet's
  primary surface and says it *must be complete on its own*, because it is the redundancy path and
  a redundancy path that requires an installed app is not one. So `clients/console` grows into that
  client — split into ES modules served from the same directory, still with no build step, still
  editable while a show runs — and the engineering readouts move behind a *tech* layout preset
  rather than being deleted. §14.3 draws what it becomes; the README's `clients/tablet` paragraph
  is superseded by this.

- **W — The edit lock is an engine node, not a client's own restraint** (settled 2026-09-09): show
  mode locks the layout and disables editing, and the disabling half is a promise the operator
  relies on in the dark, so it is kept by the thing every client talks to rather than by each
  client separately. A lock only the desktop honours is not a lock: the tablet in the house, an MCP
  client and somebody's script are clients of the same surface (§3.2). What it refuses is document
  mutation. What it must not touch is GO, the standby, every run gesture, the aim and load-to-time,
  the save itself, and `node.set` on a **mounted** address — because §3.17 has the operator walking
  the house adjusting parameters while the show runs, and locking that would mean locking the
  mixing. §14.11 draws the predicate and the doors.

### Open, with the subphase that forces each

| # | Question | Forced by | Fallback if undecided |
|---|---|---|---|
| ~~E~~ | *(still open, and §14.16 says why that is not an accident — see below)* **Does the Phase 5 desktop UI run in-process or as a separate client?** | Phase 5, but it shapes Phase 2's plugin-parameter handover | assume separate, because that is the stricter assumption and the one PRD §3.2 reads most naturally |
| J | **Should PRD §4.2 record what Tracktion does inside the callback?** Its device callback takes one uncontended `std::shared_lock` per block and its node-player pool uses semaphores; the lipogram can be *enforced* on Go.dot's code and only *measured* on Tracktion's (§11.5). A PRD amendment is the author's to make. | the lipogram test (PR 2.2) | enforce on Go.dot's scopes, report Tracktion's count separately, never hide it |
| ~~K~~ | *(settled 2026-09-06, in PR 2.6, the way this table recommended — see below)* **How does a mount declare what it can do?** `transport` says how to *send* and nothing says whether the target can be *asked*, so `wait: verified` against a write-only device is a cue that cannot succeed and nothing notices until the show. Chataigne carries two booleans per module, `hasInput` and `hasOutput`, for exactly this. Also: whether the answer names the *mechanism* (`oscquery` \| `poll` \| `subscribe` \| `none`) or only the capability. | `verified` (PR 2.6) | a mount-level `readback` enum defaulting to `none`, and a `verified` cue against `none` refused at load — the strictest reading, and the one that cannot fail silently |

**E — not answered, and deliberately so** (2026-09-09). Decision T builds the engine half of
Phase 5 first and leaves the desktop client an outline, which means E does not have to be answered
this phase and would only be answered *by accident* if a compiled client were started now. What has
arrived instead is evidence: the web client of decision V is a separate client by construction, it
holds nothing the engine owns (§14.1), and it has driven every gesture Phase 4 built over a socket
without once needing to be inside the process. That is the fallback this table recorded, earning
itself rather than being assumed. §14.16 writes the outline against it and says so.

**K — settled 2026-09-06, in PR 2.6, exactly as the fallback drew it.** A mount declares
`readback` (`none | oscquery`, default `none`) and `queryPort`, and a `verified` cue aimed at a
mount that declares neither is refused when the show is read. `none` is the right default
because it is true of most devices: OSCQuery was never standardised, and a mounted namespace is
usually hand-written for a box that will never answer. The check is on the document alone — the
cue names an address, the address falls under a mount's prefix, the mount says whether it can
be asked — so it runs in `wfg validate` on a laptop with nothing plugged in, which is the
machine somebody is sitting at when they have time to fix it. The answer names the MECHANISM
rather than only the capability, so that Phase 4's three other ways of getting a value back
(a polled get-convention, a subscription, a bespoke sync command) each become another word here
rather than another boolean.

**And the word "refused" above is not what PR 2.6 built** — found on 2026-09-07 while auditing the
claims §13 rests on, and recorded here rather than quietly rewritten, because the decision and the
code disagree and the decision is the one that was approved. The check exists and finds the cue, but
it only *reports*: `wfg serve`, `wfg tree` and `wfg replay` print it to stderr and open the show
anyway, so the cue fires and fails on a timeout during the act. Only `wfg validate` turns it into a
non-zero exit. The fallback this decision took was chosen as "the strictest reading, and the one
that cannot fail silently", so the load refusal is a debt rather than a change of mind, and PR 4.1
pays it beside the others in §13.13.

None of these blocks the next subphase. B (tombstones) and A (standby persistence) were the
two due soonest and both are now settled. J changes no code either way; it changes what §4.2
claims. K does change code, but not before PR 2.6, and its fallback is the safe direction.

One more that is the author's rather than a question for this document: **PRD §3.11 opens
"Targets speak OSCQuery"**, which states as a premise something true of Go.dot's own
processors and untrue of most third-party devices. The section's own parenthesis already
scopes the mechanism correctly, so this is a sentence to amend rather than a design to change.

## 10. Not in Phase 1, by design

Media, fades, triggers, bindings, run pointers, prepare/commit, headers and footers, the
solver, timecode, surfaces, video, plugins. The tree above is the skeleton those hang on:
a cue's outputs and parameters (Phase 2+) become further nodes under `/godot/cue/<id>`,
run pointers become `/godot/run/<id>` (a minimal form in Phase 2, plural per group in
Phase 3), and mounts stop being stubs in Phase 2. §11 draws the Phase 2 part and §12 the
Phase 3 one — each written before its code, so its pull requests have a text to be reviewed
against rather than a memory.

## 11. Phase 2 — first sound: what the tree, the commands and the log gain

Written on 2026-09-05, while Phase 1's PRs 1.2–1.11 are still landing. **Nothing in this
section exists yet.** It fixes the shape so that PRs 2.1–2.9 are reviewed against a text
rather than against memory; the rows reach `parameters/godot-parameters.csv` with the PR
that implements each of them, never before. The engine-side design (threads, the generated
Tracktion Edit, the measurements each PR must take) is in the approved Phase 2 plan and will
be reconciled into this file at close-out, as §2 was for Phase 1.

### 11.1 `/godot/audio` — the fixed graph, and two nodes it adds to `/godot/engine`

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/audio/tracks` | `i` | ro | show | the fixed track count — the polyphony ceiling (§3.25). Required, no default (G) |
| `/godot/audio/bus/<id>/name` | `s` | rw | show | user-authored, what a dropdown shows |
| `/godot/audio/bus/<id>/firstChannel` | `i` | ro | show | hardware output index, 0-based |
| `/godot/audio/bus/<id>/width` | `i` | ro | show | explicit, never inferred (§3.9b) |
| `/godot/audio/device` | `s` | ro | none | the open device's name |
| `/godot/audio/outputs` | `i` | ro | none | hardware outputs the device presents; a cue wider than this is refused at load |
| `/godot/audio/status` | `s` | ro | none | `stopped` \| `running` \| `noClock` — "no clock" and "no interface" are different failures (§6.2) |
| `/godot/engine/launchLatencyTicks` | `i` | ro | none | `1 + ceil (blockSize / samplesPerTick)`, see §11.5 |
| `/godot/engine/rtViolations` | `i` | ro | none | allocations counted inside Go.dot's audio scopes since start |

A **bus** is a summing point — a named, contiguous range of hardware outputs with a declared
width. Processor *slots* (exclusive, allocated) are Phase 4 and are not drawn here.

### 11.2 Cue kinds

`kind` grows to `memo | group | media | fade | stop | osc`. Each kind's attributes are nodes
under `/godot/cue/<id>/`, `rw`, `persist = show`:

| Kind | Attributes (type, default) |
|---|---|
| `media` | `file` string, bundle-relative under `media/`; `level` double dB (0, −120..12); `startOffset` double s (0); `Route*` children: `bus` id, `gains` = `C_in × width` doubles, row-major (`/godot/cue/<id>/route/<busId>/gains`, the first list-typed node) |
| `fade` | `target` cue id; `level` double dB; `duration` double s; `curve` enum `linear \| sCurve` |
| `stop` | `target` cue id; `verb` enum `hard \| fade`; `duration`; `curve` |
| `osc` | `address` string, a mounted node; `value` string, one typed atom as the log writes it (`f:0.5`, `s:"…"`, `T`); `wait` enum `none \| sent \| verified`; `timeout` double s |

**Built in two steps: `none \| sent` in PR 2.5, `verified` and its `timeout` in PR 2.6.** The
enum grew only when the engine could honour the new word, because a grammar that accepted one
it ignored would be a show that looked like it was checking and was not.

A media cue's `level` is what was decided. The level a running instance is actually at is
`/godot/run/<id>/level` (§11.3), which is what a fade writes. The two never merge (§4.10).
A missing media file is reported at load and fails the arm, never the load.

### 11.3 `/godot/run` — what is happening

A run is the live instance of a launched cue. Phase 2 has one per launched cue; Phase 3
makes them plural per group and adds kill, advance and prune. Run IDs are generated exactly
like cue IDs and are logged as applied in the `go` record.

| Node | Type | Meaning |
|---|---|---|
| `/godot/run/<id>/cue`, `kind` | `s` | the cue it instantiates, and its kind |
| `/godot/run/<id>/state` | `s` | `armed` \| `playing` \| `stopping` \| `done` \| `failed` |
| `/godot/run/<id>/track` | `i` | the fixed track it plays on (media only) |
| `/godot/run/<id>/position` | `d` | seconds into the file — a readout, never a model input |
| `/godot/run/<id>/level` | `d` | live level in dB, `rw`; what fades write |
| `/godot/run/<id>/late` | `i` | blocks between the intended launch and the earliest one possible |
| `/godot/run/<id>/error` | `s` | reason when `failed`: `no-track`, `media-missing`, `timeout`, … |

All `persist = none`. `GODOT.RATE_CAP` on `position` is the tick rate; nothing here is
anticipatable.

### 11.4 Commands, and the events the engine reports to itself

Operator commands, write-only method nodes as in §2.6:

| Command | Node | Params | Notes |
|---|---|---|---|
| `go` | `/godot/cmd/go` | — | acts on the focused list's standby and **advances it** (§3.5); the run ID it created is the record's last argument |
| `cue.fire` | `/godot/cmd/cue/fire` | `s` cue id | fires a named cue and **does not touch standby** — only GO moves it |
| `run.kill` | `/godot/cmd/run/kill` | `s` run id | hard stop; the primitive Phase 10's stop levels will use |
| `audio.arm` | `/godot/cmd/audio/arm` | `s` cue id | explicit arm; standby arms implicitly |

**Engine-origin commands.** Everything the tick thread learns from Tracktion or from a mounted
target *that a decision depends on* enters the model as a command with origin `engine` or
`mount:<id>`, applied on the tick it was observed and logged like any other. They are
registered commands (§4.11 holds for what the machine reports too) whose handlers are
replay-idempotent, and they are rejected from any other origin (`R … bad-origin`):

| Command | Args | When |
|---|---|---|
| `audio.editBuilt` | `h` seed, `i` tracks, `i` outputs | the Edit was generated and its node graph verified collision-free; the seed makes replay build the same Edit |
| `audio.armed` | `s` run, `i` track | the media is in the slot |
| `audio.deviceStarted` | `i` sampleRate, `i` blockSize, `s` device, `h` switchSample | the device (re)started; the tick clock rebases at the boundary after `switchSample` |
| `audio.sessionReleased` | `i` generation | the tick thread has let go of a retired playback context |
| `run.started`, `run.ended` | `s` run | the launch handle reported playing / stopped |
| `run.late` | `s` run, `i` blocks | a GO arrived before its arm completed |
| `run.failed` | `s` run, `s` reason | |
| `mount.readback` | `s` mount, `s` address, one atom | a value read back from a target's OSCQuery server |

### 11.5 Two rules and one protocol

- **State transitions are events; continuous readouts are not.** A run's `position`, the
  engine's `tick`, a meter: snapshot readouts for clients, never inputs to a decision. A run
  ending, a device starting, a read-back arriving: logged commands. Replay with no audio
  engine at all re-injects every transition from the log and reproduces the saved bundle, the
  tree dump and the log itself — the same guarantee §7 already makes.
- **The launch tick.** A `go` applied at tick *n* launches at tick
  `n + 1 + ceil (blockSize / samplesPerTick)`: far enough ahead that Tracktion never starts a
  clip back-dated (a launch beat already in the past skips the file forward by the lateness,
  it does not delay it), and a pure function of the log header, so replay computes the same
  tick. One or two ticks of latency, exposed at `/godot/engine/launchLatencyTicks`. Every
  message belonging to one GO leaves in the same frame (§3.4).
- **The session protocol.** Tracktion recreates its playback context on every device change,
  on the message thread. The tick thread never holds a raw pointer into it: it reads an
  immutable session `{context, launch handles, generation}` published by the audio host, and
  the host retires a session only after `audio.sessionReleased <generation>` has been applied.

**The audio thread's contact with the control plane** stays one relaxed atomic add on the
sample counter, plus the atomics of Go.dot's own output plugin (level and routing matrix,
slewed per block). The lipogram (§4.2) is *enforced* by a test on Go.dot's scopes of the
callback — prologue, sub-block loop, the plugin's process, epilogue — and *measured* on
Tracktion's, whose own device callback takes one shared lock per block by design (question J).

### 11.6 The bundle and the log

```
MyShow/
  media/               audio files, referenced bundle-relative from Cue/@file
```

The log header gains one line per media file the show references — `# media <path> <bytes>`
— so a replay knows what was read without hashing a show's media on every open.

### 11.7 Ports, unchanged

Mounts send over UDP (their declared `transport`); 8011 stays reserved for OSC over TCP, and
`verified` reads back over the target's OSCQuery HTTP port. Nothing new is opened.

**That last clause is true of a minority of targets, and deliberately so in Phase 2.** OSCQuery
was never standardised, so most devices do not implement it: a mounted namespace will usually
be hand-written, and getting a value back from such a target needs one of the other three
mechanisms the ecosystem uses — a protocol get-convention that is polled, a subscription, or a
sync command somebody wrote for that one device. Those are Phase 4 or later. Phase 2
implements the OSCQuery path, which is what PRD §3.11 already scopes `verified` to when it
calls it the *default for own processors*. Question K is how a mount says which it is, so that
a `verified` cue against a target that can never answer is refused when the show loads rather
than discovered during it. The survey behind this is in `docs/godot-reuse-map-0.1.md`.

### 11.9 What Phase 2 built, against what §11 drew

Written at close-out, 2026-09-06. §11 was drawn before any of it existed, which was the point:
the pull requests had a text to be reviewed against rather than a memory. It came out close,
and the differences are worth naming because each is a thing the drawing could not have known.

- **`mount` gained four attributes nobody had drawn**: `host` and `port` (PR 2.5) because
  nothing in §2.5's table said where a mount SENDS — correct while it sent nothing, a gap the
  moment it did — and `readback` and `queryPort` (PR 2.6) as question K's answer. `port` is
  required with no default, for a harder reason than `audio/@tracks`: UDP never reports that
  nobody was listening, so a mount that guessed would send into the dark and report success.
- **`osc` has no `timeout` until it has `verified`.** The enum grew in two steps, one per PR,
  because a grammar that accepted a word the engine ignored would be a show that looked like it
  was checking and was not.
- **`run` gained `stopIssued` and a fade job gained `stopsAtTick`**, neither published. The
  first is because two paths can now stop a voice; the second because the author settled that a
  stop happens when it should even if a later fade takes over the level (2026-09-06).
- **`mount/<id>/sent`** was added as a readout. UDP cannot report delivery; how many times
  Go.dot sent is the honest thing it CAN say, and it is the first question at a tech rehearsal
  when a device is not moving.
- **The engine-origin commands are not origin-checked.** §11.4 says they are rejected from any
  other origin. No engine-origin command has ever checked, so `mount.readback` does not either
  — adding it to one would be a rule with a single member. It is on the deferred list.

Everything else in §11 was built as drawn, including the launch-tick rule, the two-value
`kind`-derivation, the run table, and the rule that state transitions are events and continuous
readouts are not — which turned out to be the load-bearing sentence of the whole phase: it is
why a fade replays with no audio, why a verified cue replays with no network, and the one time
something reported from a command handler instead of a tick hook, a replay fixture caught it in
the same afternoon.

### 11.8 What PR 2.1 measured, and the two things it changed its mind about

Everything below is measured on the graph that plays, on the Windows box, at the pin. The
numbers are here rather than in a commit message because the next three PRs are built on
them.

**M1 — routing exactness.** A file whose every sample is a known constant, a different
constant per channel, through a rig assembled from the show document: a destination either
carries its coefficient exactly or carries nothing. One channel into eight outputs, unity into
two, a stereo cue splitting and summing, and eight channels into sixty-four with destinations
near the top of the range. All exact; every output nobody named is digital silence, not
"small". The eight-into-sixty-four case is the one the architecture rests on, and it is why
`CueOutputPlugin` overrides `getNumOutputChannelsGivenInputs`: sized from `getBusses()`
instead, the buffer would be stereo and that case would be silent everywhere.

**M3 — what a block costs**, at 96 kHz and 64 frames where the budget is 667 µs, every track
audible while it was timed, Release:

| configuration | µs/block | of real time |
|---|---|---|
| 32 tracks × 64 outputs | 221 | 33 % |
| 32 tracks × 8 outputs | 78 | 12 % |
| 1 track × 64 outputs | 19 | 3 % |

**So the wide device and the per-track matrix fit, with room, at the polyphony ceiling and the
widest rig the design admits.** The per-destination fallback spike 01 built and the plan wrote
down is not needed, and stays written down. The Debug figure is 242 %, which is why none of
this is asserted as a threshold: a wall-clock gate on a shared runner is a flaky test, and the
Debug number is not one any show runs at.

**M2 — node identities.** 1 to 64 tracks, every slot holding a resident clip, on the graph
`EditPlaybackContext` itself builds: 15 nodes at one track, 456 at sixty-four, zero
unidentified and zero duplicated. The upstream collision does not appear in Go.dot's generated
Edit, so the check ships and the jittered-identifier lattice the plan reserved does not.

#### The two things this changed its mind about

**The Edit runs at 60 bpm, and that is now load-bearing rather than tidy.** A launcher clip is
played through auto-tempo: Tracktion stretches it so its length in *beats*, taken from the loop
info the file was scanned with, fits the tempo map. The resident clip is created against a
one-second placeholder, so it says one beat; pointing it at a two-second cue changes the source
and not the beat count, and the file is squeezed into one second. At one second the cue goes
quiet **while the launch handle still reports that it is playing** — which is the worst shape a
failure can have, because nothing looks wrong. Turning auto-tempo off is not the fix: with no
beat length there is nothing for the launcher to schedule and the clip plays nothing at all.
Measured in both directions. At 60 bpm one beat is one second, so setting a clip's beat count
from its length in seconds makes the stretch exactly 1:1 — the tempo map is the identity, and a
cue plays at the rate it was recorded at.

**A cue is not audible the moment it is launched.** A wave clip is silent until the audio file
cache holds a mapped Reader for its file, and the cache only maps a file while something holds
one; measured at about 0.4 s for a local file. Firing a cue before that plays silence for as
long as the disk takes, with the run reporting itself as playing throughout. This is why
`AudioHost::waitForTrackSourceReady` is on the host rather than in a test: **PR 2.3's arm calls
it from standby**, so the disk is waited on while the operator reads the next line rather than
after they press GO. It also means arming is not free and its cost is a disk, which the
prepare/commit design in Phase 4 should assume rather than discover.

#### Deferred out of 2.1, on purpose

`TeSession` and `ItemIds` were listed in the plan for this PR and are not in it. `TeSession`
exists to keep the tick thread from holding a raw pointer into a playback context that a
**device change** destroys and recreates; there is no device until PR 2.7 and no way to
exercise a generation swap before it, so it lands there, with the code that makes it necessary.
`ItemIds` was the jittered identifier lattice, and M2 is the reason it is not here: it was a
workaround for a collision that does not occur.

## 12. Phase 3 — groups, triggers, ranges: what the tree, the commands and the log gain

Written on 2026-09-06, before any of it exists, as §11 was for Phase 2: the approved Phase 3 plan
drawn as a text the pull requests 3.1–3.13 can be reviewed against rather than against memory.
Rows reach `parameters/godot-parameters.csv` with the PR that implements each of them, never
before. Where this section and the code come to disagree, §12.15 at close-out says which won.

Four decisions the author took with the plan shape it — **L** (the §3.24 proposals), **M** (GO on
a manual group), **N** (refire per kind) and **O** (a group fade is a trim), all in §9 — and so
does one rule Phase 2 found load-bearing and Phase 3 leans on harder than anything else did:

### 12.1 The hook decides, the handler applies — and the arithmetic that forces

Phase 2's rule was that *state transitions are events and continuous readouts are not*, and that
*only the tick hook reports*. Phase 3 has a scheduler, and a scheduler is nothing but decisions,
so the rule becomes: **every decision the scheduler takes is a logged engine-origin command** —
spawn a member, launch it, fire it when its wait elapses, materialise a round, enter a range,
report a group done. The hook submits; the handler applies; **a handler never submits**, because
`wfg replay` re-injects every record *and* re-runs every handler, and a handler that reported would
report twice. Two consequences the code forced:

- A due tick computed inside a handler comes from the command's own tick
  (`CommandContext::tick`), never from `Runner::currentTick`, which no hook sets during a replay.
- The new hooks sit **above** `beforeTick`'s "no Player, return" line, because `wfg serve` without
  `--hosted` has no Player and must still sequence a group of memo, osc and fade cues.

**The gap at a sequence boundary is 2 + `launchLatencyTicks` ticks.** The hook at tick *n* sees a
member's voice stop and submits `run.ended`; *n*'s drain applies it; the hook at *n+1* sees `done`
and submits `run.launch`; *n+1*'s drain applies it; the hook at *n+2* places the launch
`launchLatencyTicks` ahead. Same-tick reaction would need a hook to mutate the model directly,
which replay forbids. The number is published — `/godot/engine/sequenceGapTicks` — for the reason
`launchLatencyTicks` is: a designer timing a chain against light needs it. §3.6's sequence group
is discrete children relaunched; the sample-accurate join is §3.24's range (§12.9), a different
owner by design.

### 12.2 `/godot/run` — a tree, and every kind gets one

Every cue kind now gets a run, memo included (done on the tick *after* it fires, as an osc cue
with `wait = none` already is), so "done" has one home: **a run is complete when its `state` is
`done`**, every kind's loop ends its run with `run.ended`, and the group scheduler reads states from
the table and nothing else. A run copies `preWait` and `postWait` (in ticks) from its cue at
creation, as it copies `kind`, so an edit under a running group changes the next run and never the
current one; a group's `mode`, `advance` and `selection` are **not** copied — §3.6 says a mid-run
toggle takes effect at the next member boundary, so the job reads them from the document there.

| Node | Type | Meaning |
|---|---|---|
| `/godot/run/order` | `s` | live run IDs in creation order — the first container-level node (§12.12) |
| `/godot/run/<id>/state` | `s` | grows `waiting` (its pre-wait is running) and `postWait` (its own activity ended; the post-wait before it reports done to its parent) beside `armed \| playing \| stopping \| done \| failed` |
| `/godot/run/<id>/parent` | `s` | the group run that spawned it, or empty at the top level |
| `/godot/run/<id>/children` | `s` | its child run IDs in order (groups) |
| `/godot/run/<id>/phase` | `s` | groups: `header \| members \| footer` |
| `/godot/run/<id>/member` | `s` | groups: the cue ID of the member in progress |
| `/godot/run/<id>/iteration`, `iterations` | `i` | groups: the round in progress and the count (0 = infinite) — the strip's `3/8` |
| `/godot/run/<id>/round` | `s` | groups: the materialised round, cue IDs in the order they will play |
| `/godot/run/<id>/pruned` | `s` | groups: cue IDs pruned from this run — run-local, evaporates with the run (§3.6) |
| `/godot/run/<id>/seed` | `h` | groups: the seed the round was drawn with |
| `/godot/run/<id>/range`, `rangeIteration` | `s`, `i` | ranged media: the range in progress and its pass — the pass is a readout computed from the sample counter, like `position` |
| `/godot/run/<id>/rate` | `d` | media: the rate it was armed at (§12.10); read-only |
| `/godot/run/<id>/error` | `s` | grows `no-port` (a MIDI cue's port is unbound on this machine), `bad-target` (a target the document no longer has), `no-slot` (a range beyond the slots the show was loaded with) |

All `persist = none`, as every run row is.

**Refire, per kind — decision N.** A GO or fire on a cue that already has a live run is *ignored*
for `media` and `group` (applied, logged, nothing created); *restarts* a `fade` or `stop` (the
existing takeover path, from the level the target is at now — the superseded fade's own run ends
and a fresh one starts); creates a *second instance* of an `osc`, `midi` or `memo` cue. A stop or
fade aimed at a cue with several live runs acts on the **newest** — `liveRunOf`'s existing answer.

**Retention.** A finished run keeps its address for 250 ticks after it ended (a constant of the
Runner, derived from the `run.ended` tick, so it is the same on replay) and is then removed with
`PATH_REMOVED`. Gogo is present tense (§7 of the PRD); a client polling at 20 ms still sees the
`done` it was waiting for; a four-hour show does not publish four hours of runs every tick.

### 12.3 What the engine reports to itself — the scheduler's records

Registered commands, replay-idempotent handlers, origin `engine`, as §11.4's are:

| Command | Args | When |
|---|---|---|
| `run.spawn` | `s` parentRun, `s` cue, `[s run]` | the scheduler created a child run — armed if media, idle otherwise. The generated ID is the record's last argument, as every generated ID is |
| `run.launch` | `s` run | the scheduler started a run: its pre-wait begins; the due tick is the record's tick plus the wait |
| `run.fire` | `s` run | a pre-wait elapsed: the kind's fire path runs — media requests its launch, a fade, osc or memo fires at once. Its own record, because a replay runs no hook and skips no handler |
| `run.done` | `s` run | a post-wait elapsed; the run reports done to its parent. Written only when there *was* a post-wait — `run.ended` sets `done` directly when the run copied none |
| `run.round` | `s` run, `h` seed, `s` ids… | a shuffle group materialised a round. The round is the data; a replay never consults the RNG |
| `run.range` | `s` run, `i` index | a ranged media run entered a range — at launch, at a placed boundary, or on an advance — reported when the boundary is *placed*, the `run.started` rule |
| `run.late` | `s` run, `i` blocks | §11.4 declared it and nothing ever produced it. From Phase 3 the intended launch tick is kept on the run and the hook reports the shortfall — a GO before its arm, a range boundary its re-arm missed |

### 12.4 Operator commands

| Command | Node | Params | Notes |
|---|---|---|---|
| `run.stop` | `/godot/cmd/run/stop` | `s` run, `s` verb, `[d duration, s curve]` | the targeting object of §3.8 aimed at a *run* rather than a cue — "may target a specific run pointer". Verbs as `stop/@verb`: `hard \| fade \| afterIteration \| afterMember \| advance` |
| `run.advance` | `/godot/cmd/run/advance` | `s` run | leave the current range at the end of its current pass (§12.9) |
| `run.prune`, `run.unprune` | `/godot/cmd/run/prune`, `…/unprune` | `s` run, `s` cue, `s` scope | scope `round` (this round only) or `group` (every round of this run). Run-local; clicking again reinstates if not already passed (§3.6) |
| `run.kill` | unchanged | `s` run | **now kills a run with no track** — a fade, an osc wait, a group and every descendant — immediately, and runs no footer |
| `trigger.fire` | `/godot/cmd/trigger/fire` | `s` trigger, `[s run]` | what a matched trigger submits (§12.8); fires the trigger's cue as `cue.fire` does and never moves standby or focus |
| `go`, `cue.fire`, `audio.arm` | unchanged | | `audio.arm` stays the explicit form and still accepts only media |

**Graceful and immediate, drawn now for §4.4 later.** A **stop cue** (any verb) aimed at a group
stops its live members per the verb, **then runs the footer**, then the group reports done — the
same path as normal completion, entered early. **`run.kill`** on a group run kills every
descendant and runs no footer. Esc and double-Esc in Phase 10 are these two paths bound to keys.

### 12.5 Groups — `/godot/cue/<id>` grows, and two children appear

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/cue/<id>/selection` | `s` | rw | show | groups: `sequential \| shuffle` (§3.6) |
| `/godot/cue/<id>/loops` | `i` | rw | show | groups: rounds to play; 1; **0 = infinite** |
| `/godot/cue/<id>/play` | `i` | rw | show | groups: "play N of M"; 0 = all |
| `/godot/cue/<id>/seed` | `i` | rw | show | groups: 0 = a fresh seed per run, drawn by the run and logged in `run.round`; anything else is the fixed-seed option for "random that is the same every night" |
| `/godot/cue/<id>/headerOrder`, `footerOrder` | `s` | ro | none | groups: the cue IDs of the header and footer, in order |
| `/godot/cue/<id>/role` | `s` | ro | none | any cue: `member \| header \| footer` — where in its group it sits |

**Header and footer** are two optional child elements of `Group`, `Header` and `Footer`, each
holding ordinary cues. They are **identified** (the `Route` precedent, and because `cue.create`
and `object.move` address a parent by id) and carry no attributes of their own; at most one of
each per group is a `validate()` rule. Header cues run as an auto sequence at entry and members
wait for them; footer cues run as an auto sequence at exit and **block** completion (§3.6: "the
group is not done until its footer's cues report done"). `order` lists members only; the cursor
and `childrenOf` never enter a header or footer; `standby.set` refuses their cues. Phase 4 turns
the header into the prepare horizon; nothing here forecloses that.

**How a group run proceeds.** After its own pre-wait, then its header:

| mode / advance | at entry | on a member's `done` | complete when |
|---|---|---|---|
| **timeline** | every member spawned and launched at once; each member's pre-wait is its offset from entry (§3.6). Tracks are claimed at entry, so a member that finds none fails *at entry*, visibly, not at its offset | nothing — post-wait is inert in a parallel parent | every member `done` |
| **sequence · auto** | the first member spawned and launched; the next spawned (armed) | the armed next member launched; the one after spawned | the last member of the last round `done` |
| **sequence · manual** | created by GO (§12.6); the header runs; members fire on GO | nothing — GO is the parent | as auto, and the pointer has already left |

Then the footer, then the group's post-wait, then `run.done` to its parent. **Waits compose, they
do not replace** — exactly as §2.4 recorded before any of this existed.

**Rounds.** A round is materialised as a list when it begins (`run.round`); a shuffle re-draws
until the first of the new round differs from the last of the previous (two members: fully
determined, and correct). **Iterations count rounds, not playbacks.** A pruned member is removed
from the current round or from every round of this run; an emptied round completes the group
rather than spinning; `afterIteration` and `afterMember` are honoured at the next boundary.

**Seven things PR 3.5 settled while building that paragraph**, recorded here because each of
them is a choice somebody could reasonably have made differently.

1. **A manual group ignores `selection` and `play`.** Both are the *machine* choosing — which
   member comes next, and how many — and in a manual sequence the operator is the one choosing
   (§3.6). The pointer walks the list in document order, so a shuffled round would have the group
   finishing at whichever member the draw happened to put last, at a moment the operator has no
   way to see coming; "play two of five" would leave three rows the pointer walks through and
   nothing happens on. Ignored rather than refused at load, because the pair means something the
   moment somebody makes the group automatic, which §3.6 expects during tech.

2. **The round lives on the run, and the scheduler re-reads it every tick.** It is what
   `/godot/run/<id>/round` publishes, it is written by `run.round`'s handler, and the scheduler
   works from it rather than from a copy — which is what lets a prune reach the round *in
   progress*, and that is the whole of what an operator asking at 22:40 wants.

3. **`run.round`'s handler drops pruned members too**, and not only the draw. The two commands
   can arrive in either order inside one tick: the scheduler draws in the hook, and an operator's
   prune submitted a moment earlier is already in the queue ahead of it, taking a member out of a
   round that does not exist yet. Filtering in both places makes the result the same whichever
   way round they land — which a replay needs as much as the operator does.

4. **Every round is a pure function of the seed and the round number** (`seed ^ k·φ`), rather
   than of a running generator state carried between rounds. So `/godot/run/<id>/seed` means the
   seed this run drew — stable, and the number to write down after a night somebody liked —
   rather than "where the stream has got to".

5. **A group that does not shuffle draws no seed and publishes nought.** An unused random number
   in every group's log is a number somebody will one day try to interpret, and the one thing
   that differs between two otherwise identical sessions.

6. **The shuffle is written out — SplitMix64 and Fisher-Yates, eight lines — rather than reached
   for in `<random>`.** That header's *engines* are specified down to the bit and its
   *distributions* are not, and neither is `std::shuffle`: the same seed gives different orders on
   different standard libraries. A show rehearsed on one machine has to play the same order in
   the theatre, and a fixture drawn on one platform has to replay on three. A golden case pins
   the order for a fixed seed.

7. **`run.stop` has three verbs, not the plan's four.** `hard`, `afterMember` and
   `afterIteration`; `fade` is deliberately absent. A fade needs a run of its own to report
   through — it takes time, and something has to say when it arrived — and a command has no cue
   and therefore no run. A fade-and-stop is a stop *cue*, where the duration and the curve are
   authored values somebody decided rather than arguments typed at the moment of panic. Aiming
   one at a specific run waits for PR 3.12, which is where a run's level composition is built.

**And one cost, measured rather than assumed.** `afterIteration` asked for in the same tick as a
round boundary costs one more round. The scheduler decides in a tick hook, which runs *before*
that tick's commands are applied, so it has already decided to start another round when the stop
arrives; a press one tick earlier — twenty milliseconds — is seen. It is the same one-tick
decision latency `/godot/engine/sequenceGapTicks` publishes, seen from the other side, and it is
the price of every decision being a logged command. Fixture #11 says so in its own text.

**Disabled cues are skipped** — by the scheduler (not spawned, not run in a header or footer) and
by the cursor. A GO whose standby is disabled, which only a cue disabled while the pointer sat on
it can produce, is applied, fires nothing and advances: the one GO that does nothing, logged as
such. Phase 1's "a disabled cue is not skipped" (§2.6) is therefore superseded here.

**Arming, which nothing does implicitly today.** §11.4 said "standby arms implicitly" and no code
ever did — `audio.arm` is an operator command with no submitter in the engine, and a GO on an
unarmed cue arms and launches in one, paying the disk. Phase 3 builds it: when a list's standby
lands on a media cue (by any of the ways it moves) the Runner arms it; when it lands on a group,
the Runner arms what that group would launch first. Inside a running sequence group the next
member is armed; a timeline group arms every member at entry. Phase 4's allocator replaces this
lookahead with claims.

### 12.6 The standby cursor — decision M, and four places that move

`nextOf`/`previousOf` were one level of one list. They become a cursor that **descends into an
enabled manual sequence group** to its first enabled member, steps over timeline and auto groups
as opaque siblings — positionally past the whole chain, the instant GO is pressed (§3.5) — skips
disabled cues and header/footer elements, and **climbs out** to the group's next sibling when the
members are exhausted. **Decision M**: GO at a manual group's row fires its first member (after
the header) and lands the pointer on the second; the pointer leaves the group the moment the last
member of the last round is fired, not a GO later. In a manual loop the cursor wraps to the next
round's first member while rounds remain — the one fact it reads from the run table.

The invariant on `list/@standby` widens from *a top-level child* to *a cue of this list whose every
ancestor group is a manual sequence group*, in **all four places it lives**: the legality check,
the document's write door, the repair when the standby cue is deleted (advance to the next
remaining sibling *inside the group*, or climb out), and the clear when it is moved away.
`standby.set` on a member of an auto or timeline group is refused with a new atom,
**`not-manual-path`** — the pointer cannot be the parent of something the machine parents.
`not-in-list` keeps its meanings. `standby.set` also stops refusing media, fade, stop and osc cues,
which it has done since Phase 2 by accepting only elements named `Cue` or `Group` — a bug no
fixture exercised, because every one parks on a memo or restores standby from `state.xml`.

GO on a member whose manual group has **no live run** — the pointer was placed inside by
`standby.set` or restored from `state.xml` — creates the group run, runs the header, and the member
follows the header: one GO, nothing skipped. **A manual sequence group is reachable only through
GO from standby**: `cue.fire` or a trigger aimed at one is rejected, **`needs-go`**, because
there is nobody to be its parent, and `wfg validate` warns about a manual group nested under a
timeline or auto group for the same reason.

What descent changes that already exists, so PR 3.4 replaces rather than discovers it: decision
C's two tests, the recorded session in `CueListTests` that asserts `standby.set` on a nested cue
is *refused*, and every fixture that carries a `<Group>` — all manual sequence groups by default.

### 12.7 `/godot/list` — the container §2.3 said Phase 3 publishes *(built in PR 3.2)*

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/list/order` | `s` | ro | none | list IDs in order |
| `/godot/list/focus` | `s` | rw | **state** | the focused list; write = `list.focus`. Persisted like `standby`, so a show reopens on the list the operator was on |

`go` acts on the focused list's standby; `cue.fire`, `trigger.fire` and the scheduler act on any
list and never move focus or standby. A list with no standby and only triggers is then §3.5's
background process, with nothing built for it.

### 12.8 `/godot/trigger` — GO is one trigger among several

A `Trigger` element, identified, is a child of any cue kind including `Group`; published flat by
ID with a derived `cue`.

| Node | Type, default | Meaning |
|---|---|---|
| `/godot/trigger/<id>/kind` | `s` — `osc \| midi \| clock` | |
| `/godot/trigger/<id>/enabled` | `T`, true | |
| `/godot/trigger/<id>/address` | `s` | **osc**: an address arriving on Go.dot's own OSC port **over UDP**. Refused at load under `/godot` or under any mount prefix — those are nodes, and a node write is `node.set` |
| `/godot/trigger/<id>/value` | `s`, empty | **osc**: an atom the first argument must match (`f:1`, `T`); empty = any, including no argument |
| `/godot/trigger/<id>/port` | `s`, empty | **midi**: a declared `Port` name (§12.11); empty = any input |
| `/godot/trigger/<id>/channel` | `i`, 0 (0..16) | **midi**: 0 = any |
| `/godot/trigger/<id>/type` | `s` — `noteOn \| programChange \| controlChange` | **midi** |
| `/godot/trigger/<id>/number` | `i`, 0..127 | **midi**: note or controller |
| `/godot/trigger/<id>/data` | `i`, −1 (−1..127) | **midi**: velocity or value to match; −1 = any |
| `/godot/trigger/<id>/at` | `s` | **clock**: `HH:MM:SS`, local time of day; fires once each day it is crossed while the show is open |
| `/godot/trigger/<id>/cue` | `s`, ro | derived: the cue it fires |

**`trigger.fire <trigger>` is the command** (§4.11). *What* fired is the command's argument,
never the origin — nothing enforces an origin and §11.9 says so. Origins still say where it came
from: `udp:<ip>:<port>`, `midi:<device>`, `clock`. The handler fires the trigger's cue exactly as
`cue.fire` does and never moves standby or focus (§3.5, §3.7); it rejects a manual sequence group
(`needs-go`).

**The matchers are pure functions over an immutable index** the tick thread republishes with the
tree snapshot whenever the document changes, so they are tested with no socket, port or clock, and
they run where the mount probe runs — in the `serve` wiring, outside `Engine` and `Runner`, which
still read no clock and own no socket:

- **OSC**: the namespace's write path runs the match *before* it treats an address as `node.set`
  and *before* its argument-less early return, and only for a `udp:` origin — a WebSocket client
  has the command set and `cue.fire`; a trigger is a device-facing input. A malformed datagram is
  still an `X`.
- **MIDI**: `wfg serve --midi-in=<device>` (repeatable) opens inputs; the callback matches on its
  own thread against the index and submits. `wfg midi` lists inputs and outputs and exits 0 with
  none. WFS-DIY's `MidiSnapshotTrigger` is the precedent for the *mechanism* and the reuse map says
  what is taken from it: own the port rather than route it through the device manager, match on the
  MIDI thread with nothing that allocates or locks, reopen by identifier then by name on hot-plug.
  **It is not a statement that MIDI is the way in** (author, 2026-09-06): that trigger exists in
  WFS-DIY because one user drives his show from a Behringer console, and **OSC is the preferred
  carrier** — certainly between Go.dot and the author's own processors, which have a namespace and
  a description of themselves. MIDI is here because §3.7 lists it and consoles exist.
- **Clock**: once per tick, in the serve loop's before-tick step, the wall clock is read and
  compared with the previous reading; a crossing submits on that tick. "Last day fired" is
  serve-side machine state, never the document. A replay re-injects the record and consults
  nothing — which is the whole point of the firing being a record.

Debounce is a user preference (§3.7) and arrives with Phase 10's other preferences.

**Six things PR 3.7 settled while building that**, recorded here for the same reason §12.5's
seven are: each is a choice somebody could reasonably have made differently.

1. **One element for three kinds, and the matchers keep them apart.** The grammar cannot refuse
   `channel` on an OSC trigger without an element per kind, and three elements for one concept
   with three sources would be worse to read and worse to extend. So `Trigger` carries every
   kind's rows, `kind` is fixed at creation the way a cue's is, and a MIDI trigger answering an
   OSC address is a thing the matchers prevent rather than the grammar. `wfg validate` is where a
   MIDI field on a clock trigger will get mentioned, when there is a warning channel for it.

2. **A trigger with no address fires on nothing**, rather than on everything. A bare
   `address != wanted` would have made a half-authored trigger — created and not yet filled in —
   answer every message the show received, which is the worst possible failure of the feature.

3. **An empty `value` matches any arguments including none**, and a value asked for is matched
   against *any* argument rather than the first. A foot switch sends a bare address; a surface
   sends `/go f:1` on press and `f:0` on release and may put the meaningful argument second.

4. **`data` is -1 for "any velocity" and channel is 0 for "any channel"**, and the asymmetry is
   deliberate. MIDI channels are one-based everywhere a musician looks at them, so nought cannot
   be one; velocity nought *can* be a velocity, and matching it is how somebody catches the
   release from the very many surfaces that spell it that way. Which is also why the conversion
   from `juce::MidiMessage` classifies by the **status byte**: JUCE reports a note-on of velocity
   nought as a note-off, and that is right for a synthesiser and wrong here.

5. **A clock trigger is asked about an INTERVAL, half-open, `(previous, now]`.** A tick is 20 ms
   and a second is fifty of them: "does the clock read 19:30:00" would fire fifty times, and
   asking on a tick that happened to be late would miss it. Midnight falls out of the interval
   wrapping rather than a special case. The first tick crosses nothing, so a show opened at
   19:30:00 does not fire the 19:30:00 cue because it happened to be started then.

6. **A `--midi-in` device that is not there is fatal at startup**, like a mistyped `--ui`, and
   the message lists the ports the machine does have. A cue that can be fired from a foot switch
   and silently cannot is the failure the whole feature exists to avoid, and the answer is almost
   always one of those names spelled differently.

**And the load refusal, which is the one thing here that stops a show from opening.** An OSC
trigger may not listen under `/godot` or under a mount prefix. The engine already answers there
on that same port, so such a trigger would be a message that both wrote a value and fired a cue —
and nobody reading the log afterwards could say which had been meant, nor which the sender
intended, because the sender wrote one message. Refused when the show is read, because there is
no reading of the file under which it does what it says.

### 12.9 `/godot/range` — ranges, in-cue loops, and what the pin actually allows (decision L)

**What Tracktion at the pin does, read rather than assumed**, because it changed the design:

- Every knob a follow action uses is on the graph's restart list, and the slot node captures its
  stop duration and follow function at graph build. Nothing about a range's shape can change while
  the graph runs, and there is no `advance` (§3.25 already says so).
- **A clip whose `isLooping()` is false gets a finite stop duration — its length — and the slot
  node queues that stop every block *before* it advances the launch handle.** So
  `LaunchHandle::setLooping`, the one rebuild-free loop lever, cannot loop a clip Go.dot armed with
  `disableLooping()`: the queued stop pre-empts the wrap at the end of the first pass. A clip armed
  **looping** gets *no* stop duration and its wave node loops the section for ever, with no click
  suppressor at the wrap, and nothing but a queued stop ends it.

**So the mechanism is this, and it uses no follow action:**

- **Every range is a clip in its own slot, armed looping** — its source, then its loop range
  `[in, out)` — inside the one `ReallocationInhibitor` that arming already uses. One rebuild per
  arm, exactly as today; none while it plays.
- **Go.dot places every boundary.** The boundary of pass *k* of a range launched at sample *s* with
  length *L* is `s + k × L`, known the moment the launch is placed. The natural end of a range
  after *N* passes and an `advance` at the end of the current pass are the same operation: a
  queued `stop (boundary)` on the live slot and `play (boundary)` on the next range's slot — or
  the stop alone, after the last range — placed `launchLatencyTicks` ahead, which M5 and M6
  measured landing on their sample. An infinite range is never stopped on its own. If the audio
  thread's try-lock misses the block a wrap and a stop share, the stop lands at the next block
  start: one block of restarted audio, the class of artefact spike 03 measured.
- **Passes are readouts, transitions are events.** `rangeIteration` is computed from the sample
  counter like `position`; entering a range is `run.range`.

| Node | Type, default | Meaning |
|---|---|---|
| `/godot/range/<id>/name` | `s` | what the strip shows (§3.24) |
| `/godot/range/<id>/in`, `out` | `d`, seconds | the region of the file; `out` ≤ file length is checked at arm, when the file is read, not at load |
| `/godot/range/<id>/loops` | `i`, 1 (0 = infinite) | passes before playback continues into the next range |
| `/godot/range/<id>/cue`, `index` | `s`, `i`, ro | derived |

A media cue with no ranges plays as today. With ranges, the list is what plays and `startOffset`
is refused beside it. **Ranges may be discontiguous and in any file order** (decision L) — a media
cue is then a playlist over one file. **Edits take effect at the next iteration** (decision L): a
running ranged cue does *not* copy its ranges at launch; at every boundary the job re-reads them —
a changed `loops` is honoured then; a changed `in`/`out` re-arms that range's slot on the message
thread (a rebuild on playing audio, which M4 measured bit-identical) and the *next* pass uses the
new length; a range removed while playing finishes its pass and is not entered again. A boundary
whose re-arm has not confirmed is placed late and `run.late` says so.

**Slots.** Every track gets **S slots**, S = the largest range count of any media cue in the show
(at least 1) — a property of the show, like `tracks`, fixed at load so the graph's shape never
changes after it (§3.25). A range added beyond S during a show has no slot: refused, `no-slot`,
until the show is reloaded, and `lastError` says so. Fallback, recorded not chosen: two slots per
track, A/B alternated, re-arming the idle one while the other plays — if M10 or M11 find S slots
too costly.

`stop/@verb` grows **`afterIteration | afterMember | advance`**; `advance` means *the end of the
current pass*. §3.24 also lists *advance at range end* without saying what it adds; it is not
built until the author says.

### 12.10 Rate — at arm, not live, and the PRD has to be told

`media/rate` (`d`, 1, 0.25..4) and `media/rateMode` (`varispeed | stretch`) are applied **at arm**:
the clip's beat count and its length are scaled together (scaling one alone leaves a clip that
ends early or runs into silence); varispeed is the resampler Tracktion already uses when no
stretcher is compiled in, stretch is Signalsmith (vendored, MIT, behind
`TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH`, off today). `run/<id>/rate` is a readout.

**§3.24's "rate is a node, so it is automatable, fader-bindable and can carry a lane" cannot be
honoured at this pin.** `setSpeedRatio` is a no-op on an auto-tempo clip, which every slot clip
is; the launcher path never receives a ratio; the 1:1 rate Go.dot gets today comes entirely from
the clip's beat count, which is on the restart list. Live rate needs a per-clip speed threaded
into Tracktion's wave node — upstream or forked — and that is a PRD amendment for the author, put
at close-out beside Phase 2's three.

### 12.11 MIDI — cues, ports, and the thread that sends

**Ports are a declaration, devices are machine config.** `Show` gains a `Midi` section holding
`Port` elements (identified; `name`), published at `/godot/port/<id>/name`. The document says
*"Lights"* (§4.10); `wfg serve --midi-out=<port name>=<device>` binds it to a physical output — a
fact about this machine, like `--device=`. An unbound port is reported at start and its cues fail,
`no-port`, never the load.

| `midi` cue node | Type, default | Meaning |
|---|---|---|
| `/godot/cue/<id>/port` | `s` | a declared `Port` |
| `/godot/cue/<id>/type` | `s` — `noteOn \| noteOff \| programChange \| controlChange \| pitchBend \| aftertouch \| channelPressure \| sysex` | every event type §3.10 names |
| `/godot/cue/<id>/channel` | `i`, 1 (1..16) | |
| `/godot/cue/<id>/number` | `i`, 0 (0..127) | note, controller or program |
| `/godot/cue/<id>/data` | `i`, 0 (0..16383) | velocity, value, or the 14-bit bend |
| `/godot/cue/<id>/sysex` | `s` | hex bytes, `F0 … F7` |
| `/godot/cue/<id>/wait` | `s` — `none \| sent` | as osc, minus `verified`: MIDI has no read-back, and `verified` on a MIDI cue is refused at load |

`kind` and `run/kind` grow `midi`.

**The tick flush hands a batch to a sender thread.** At the end of the launch tick the tick thread
enqueues the tick's MIDI beside its OSC datagrams — every message belonging to one GO leaves in
the same frame (§3.4) — and a sender thread of its own puts them on the ports. Not the tick thread
itself: on Windows a SysEx send busy-waits for the port, about 32 ms for a hundred bytes, and the
flush also publishes the tree. Resolution is the tick plus `lateness`, as for a network cue. The
alternative is recorded for the sequenced-MIDI-clip cue §3.10 puts in v1: a Tracktion `MidiClip`
in a launcher slot takes the identical launch path as audio and is sample-accurate, and the host's
block already receives the graph's MIDI output and throws it away — but it needs a lock-free
queue off the audio thread, one hosted MIDI output demultiplexed by port, and more slots in the
fixed graph. Right for a MIDI *clip*; more than one event needs. A replay installs no sender.

### 12.12 The document layer — three pieces of plumbing, done once each

- **Child elements by role.** The tree walk skipped every id-less child and special-cased `Route`;
  it becomes a lookup — `Route`, `Range`, `Trigger`, `Header`, `Footer` — so a non-cue child is
  published where it belongs and never as a cue that mis-indexes its siblings.
- **Container-level nodes.** `/godot/list/order`, `/godot/list/focus`, `/godot/run/order` need
  what §2.3 said they need: an owner token for the container itself, a container case in address
  resolution, and a root-attribute case in the state writer and the RELAX NG generator (and
  `<optional>` for a single non-repeating child).
- **A `refers` column** in the parameter table names what an id-valued attribute must point at —
  `cue`, `list`, `bus`, `port` — and replaces the hand-written standby check, covering
  `fade/@target`, `stop/@target`, `route/@bus` and `midi/@port` in one place: the generalisation
  the code said "Phase 3's second case" would pay for. **A dangling reference is a `wfg validate`
  warning and a run-time `failed bad-target`, never a load refusal**: deleting a cue repairs no
  reference today and §3.8 makes a target that is not there a silent no-op during tech, so
  yesterday's saved show must open. Load refusals stay the explicit cases: a trigger address under
  `/godot` or a mount prefix, `verified` on a MIDI cue, `startOffset` beside ranges.

**The tree rebuild is split before triggers land.** The document half re-materialises every
mounted node on any applied mutation — 2 480 of them with the WFS-DIY capture. The mounted subtree
becomes its own half, rebuilt only on `mount.load`, so a trigger firing forty times a minute does
not re-sort somebody else's namespace forty times a minute. Measured before and after (M9).

### 12.13 What Phase 3 has to measure, and in which order

| | what | why it gates |
|---|---|---|
| **M9** | wall-clock of one `node.set` with the 2 480-node capture, before and after the tree split | triggers add mutation rate |
| **M10** | node IDs unique at 1..64 tracks × 1..8 slots, on the graph that plays | the ID check fails hard and the slot count multiplies its surface |
| **M11** | callback cost at 32 tracks × 8 slots × 64 outputs at 96 kHz, against M3's 221 µs | S slots is the chosen shape; A/B is the fallback |
| **M12** (spike 03b) | three joins on spike 03's chirp rig at five block sizes: the looping clip's own wrap, a `setLooping` re-trigger on a lengthened clip, the cross-slot placed boundary | the wrap is the primary; if it measures worse than a placed boundary, loops become placed same-slot `play`s |
| **M13** | a natural and an advanced boundary land on their sample; damaged span ≤ block + 40 samples | what §3.24 promises, from the render |
| **M14** | duration exactness at rate 0.5 and 2.0 in both modes; pitch preserved under stretch, shifted under varispeed | rate at arm is what is claimed |
| **M15** | an auto chain's member-to-member gap is exactly `2 + launchLatencyTicks` ticks, from the render | §12.1's arithmetic |

#### What M10, M11 and M12 answered *(PR 3.8, 2026-09-07)*

The three that gate the shape of ranges are all answered, and all three answered in the
design's favour. Numbers on the Windows box, MSVC Release for M11, Debug for M10 and M12;
`docs/spikes/spike03b-loop-joins.md` carries M12 in full.

| | verdict | the numbers |
|---|---|---|
| **M10** | **no collisions anywhere** | 1..64 tracks × 1..8 slots, all 128 combinations: 0 duplicate ids, 0 nodes without one. At 64 tracks the collection an id lookup can reach grows by exactly 128 per slot added — two nodes per (track, slot) — to 1480 at eight slots, all distinct. |
| **M11** | **S slots is affordable** | 32 tracks × 64 outputs @ 96 kHz, 64-frame blocks: ~216 µs a block at one slot, ~223 µs at eight — 32% and 33% of a 667 µs budget, against M3's 221 µs. Eight slots less one, averaged over four interleaved pairs, on three runs: +6.7, +15.1, +10.2 µs. |
| **M12** | **the clip's own wrap wins, at every configuration** | 0 join error at every block size and both rates. Damage energy - summed squared deviation around the join - is smaller for the wrap than for a placed boundary in all ten cells, by 5.5× to 23 000×; at 96 kHz up to 256 frames the wrap has no damaged sample at all. A placed boundary costs a fixed 25–33 samples at 0.49 of an amplitude of 0.5, block-size independent, which is `SlotControlNode`'s own 40-sample stop decay. `setLooping` on a clip armed not-looping never comes back. |

**M10 had to be asked of a different collection before it meant anything.** The node-id check
had looked at `orderedNodes` since Phase 2, which is the outer graph — and a launcher slot is
not in it: `SlotControlNode` is an *internal* child of the switching node above it, so a graph
with eight slots on every track has exactly as many ordered nodes as one with a single slot,
456 at 64 tracks, measured. What decides state adoption across a rebuild is `sortedNodes`,
which `createNodeMap` builds by recursing through `getInternalNodes` and which
`findNodeWithID` searches. That is the collection the check now walks.

**M11 had to be interleaved before it meant anything either.** Each rig is a Tracktion engine
built and torn down inside one process, and the cost of a block drifts upward with how many
have been built before it — the same configuration measures near 205 µs early in a run and
near 330 µs eight rigs later. Measured as a descending sweep, eight slots came out 47 µs
*cheaper* than one, which is not a fact about slots. The two configurations are therefore
interleaved, 1 8 1 8 1 8 1 8.

**M12 also priced the boundary PR 3.9 has to place.** A placed cross-slot boundary is
sample-accurate in position and costs a fixed 25–33 samples of one-sided decay on the outgoing
range — `SlotControlNode`'s `lastSampleFadeLength = std::min (numFrames, 40u)`, which spike 03
identified. It does not grow with the block size, because it is not a scheduling error. So
M13's bound, *damaged span ≤ block + 40 samples*, is met before PR 3.9 has written a line, and
the boundary between two ranges needs no cleverness to meet it.

### 12.14 The direction this phase does not build — PRD §3.26

Every trigger above is a processor or a console telling Go.dot to **fire** something. The author
added PRD §3.26 on 2026-09-06 for the other thing a processor will eventually want: to **write**
to the show — a capture verb, so that the position, send and LFO rate a designer has just found at
WFS-DIY become a cue, or update the cue being rehearsed, the way QLab's OSC API can author and not
only fire.

Nothing in Phase 3 builds it, and it needs no new transport when it comes: §4.11 already makes
every gesture a named command and §3.2 makes every client equal, so `cue.create` and `node.set`
are reachable by any process that can address the engine. What §3.26 says is missing is a capture
verb, an explicit statement of where the result lands (§3.10's *update cue*, never a silent
write-back), and a cue-list view for the processor no wider than §3.23's. It waits for the state
solver, because a capture is a solved state written down.

Recorded here so that Phase 3's trigger table is not later mistaken for the whole of the
relationship between Go.dot and the processors it commands.

### 12.15 What Phase 3 built, against what §12 drew

Written at close-out, 2026-09-07. §12 was drawn before any of it existed, which was the point:
the pull requests had a text to be reviewed against rather than a memory. It came out close,
and the differences are worth naming because each is a thing the drawing could not have known.

**The four things §12 got wrong, and what they cost to find.**

- **`midi/@port` was drawn as a name and had to become an identifier.** §12.11 said a MIDI cue
  names "a declared `Port`", and the first build read that as the port's name — which made it
  the one reference in the whole document outside the mechanism the `refers` column exists to
  be, and would have silenced every cue that used a port somebody renamed. It names the
  identifier now, the way a route names its bus; `--midi-out=<port name>=<device>` still takes
  the name, because that is what a person types and what the show file says out loud.

- **`Midi` could not be both a cue kind and a section.** Two elements of one name are one
  element as far as the schema is concerned, and the one that loses is the one nobody can find.
  The section is `MidiPorts`.

- **Adding a section to `<Show>` invalidated every show file in the tree**, because the
  generator emitted a container child as a required `<ref>`. Thirteen fixtures reported
  "Expecting an element, got nothing" at once. Containers are `<optional>` now — an empty
  `<Mounts/>` and no `<Mounts>` at all say the same thing, and yesterday's saved show has to
  open tomorrow. §12.12's plumbing list had named this and it had not been paid for.

- **`validate()` had to become two lists.** §12.12 said a dangling reference is "a `wfg
  validate` warning and a run-time `failed bad-target`, never a load refusal" — and `validate()`
  WAS the load-refusal list, so returning one from it made a saved show refuse to open. Refusals
  and warnings are separate functions now: a trigger listening inside `/godot`, a start offset
  beside a range and a MIDI cue asking to be verified are refusals, because there is no reading
  of the file under which they do what they say; a pointer at something that is not there is a
  warning, because §3.8 makes it a silent no-op during tech.

**Three things §12 drew and the engine turned out to need differently.**

- **Standby arming a group was owed by PR 3.3 and built by PR 3.13**, which is late by five
  pull requests and was found by the black-box driver's first check. `armStandby` armed a media
  cue and returned for anything else. A pointer on a group is a pointer on a whole scene, and
  not arming it meant GO on a group paid the disk with the operator's hand already down.

- **A group has to ADOPT what standby armed**, which §12 did not draw at all. The arm creates a
  run with no parent; a group that spawned its own would leave the first holding a voice nobody
  was going to launch and pay the disk twice. `run.spawn` adopts by identifier, so the record
  carries it either way.

- **`observeEdges` had to learn that a boundary is not an ending.** At a range boundary the
  outgoing slot stops in the same block the incoming one starts, and the poll that watches for
  the edge is 20 ms wide — so it can fall between them and see neither playing. A ranged cue
  would have reported itself done at its first boundary with two ranges still to play. `run`
  gained `rangesFinished`, unpublished, set when the LAST range's end is placed.

**What the measurements changed.**

- **M9 moved the mounted namespace out of the document half.** It was drawn as part of it, on
  the argument that it changes only when a mount does — true, and beside the point, because the
  document half is also rebuilt by everything else. 3.13 ms of every applied mutation with
  WFS-DIY's capture, twenty-nine times the rest of the tree put together. It is its own cache
  now, invalidated by a revision counter on the mount table rather than by a flag somebody has
  to remember to set.

- **M12 removed work rather than adding it.** §12.9 said "range clips are armed looping; every
  boundary is placed by Go.dot" and left the wrap's quality open. The wrap won every one of ten
  configurations by between 5.5× and 23 000× in damage energy, so Go.dot places nothing INSIDE a
  range: a bed looping for four hours costs no command, no placed instant and no run record.

- **M10 had to be asked of a different collection before it meant anything.** The node-id check
  had looked at `orderedNodes` since Phase 2, and a launcher slot is not in it — a graph with
  eight slots on every track has exactly as many ordered nodes as one with a single slot. What
  decides state adoption across a rebuild is `sortedNodes`, which recurses through the internal
  nodes.

**Two rows §12 drew that were not built, and why.**

- **`group/@play`** — "play N of M" — is in the table and honoured by the scheduler, but no
  fixture or driver exercises it beyond the unit suite. It is not a gap so much as a thing
  nobody has yet needed on a stage.

- **`media/@rate` and `media/@rateMode`** are PR 3.10, which the plan marked droppable and which
  is dropped. §12.10's own finding is why: rate cannot change on a playing launcher clip at this
  pin, so what could be built is rate at ARM, and §3.24's "rate is a node, so it is automatable,
  fader-bindable and can carry a lane" cannot be honoured without a TE-side change. Building the
  half would have put a row in the document that does not do what the PRD says it does. It goes
  to the author as an amendment instead.

**Everything else in §12 was built as drawn**, including the boundary arithmetic — the
sequence gap is `2 + launchLatencyTicks` and is published — the hook-decides-handler-applies
rule, the run tree, the refire policy per kind, the manual cursor's descent and climb, rounds
with a logged seed, the three trigger matchers as pure functions, ranges as looping clips with
placed boundaries, MIDI cues on a sender thread, and group fades as trims.

## 13. Phase 4 — prepare, solve, allocate: what the tree, the commands and the log gain

Written on 2026-09-07, before any of it exists, as §11 and §12 were: the approved Phase 4 plan
drawn as a text the pull requests 4.1–4.11 can be reviewed against rather than against memory.
Rows reach `parameters/godot-parameters.csv` with the PR that implements each of them, never
before. Where this section and the code come to disagree, §13.16 at close-out says which won.

Four decisions the author took with the plan shape it — **P** (the slot pool is declared in the
document), **Q** (a preset is a mark on the member), **R** (waypoints are invisible) and **S**
(the persistent section, and a kill suspends) — all in §9. Phase 3's rule holds and does more
work here than it did there: *the hook decides, the handler applies, and a handler never
submits.* Phase 4 adds one of its own.

### 13.1 Anticipation is only as good as its revocation — and one walk answers three questions

**Nothing is sent ahead of GO that cannot be put back, and putting it back means knowing what was
there.** PRD §3.12 makes anticipation a property of the *parameter*: imperceptible **and
revocable**. The second half is the one with a mechanism in it. A value pre-sent to a processor is
revocable only if Go.dot asked the processor what it held first, so the prepare path **reads
before it writes**, and the answer it got is what a revocation replays.

Which settles a question the parameter table could not: a node marked `anticipatable` on a mount
that declares no read-back is **not pre-sent**, whatever the node says, because there would be
nothing to put back. `wfg validate` says so on a laptop with nothing plugged in, the way a
`verified` cue against an unaskable mount already is (§9, decision K).

That is also why the horizon claims early and sends late. A **claim** is revocable by construction
— releasing a slot nobody heard costs nothing — so claims and arms run as far ahead as the pointer
can see. A **value** is revocable only as far as the read-back reaches. The two halves of §3.12
have different budgets, and the row says which it got: `armed` when there was nothing to verify,
`verified` when every pre-sent value read back equal, `partial` when something in the block
declined to be sent early (§3.6's own word for it).

**And the solver is one walk asked three questions.** *What should be true at this moment* is
load-to-time (§3.13). *What should be true of this designated set, now* is the persistent
assertion — PRD §3.29 says it in those words: "*asserted* is §3.13's reconstruct-and-diff applied
to a designated set at trigger time — the check reuses the solver rather than a second mechanism".
*What was true before this prepare* is a revocation. Three walks would be three sets of bugs in
one piece of arithmetic. There is one `solve`, and what differs between the three is which cues it
is pointed at and what is done with the plan it returns.

**And the arithmetic comes out one tick better than a sequence boundary, which is worth knowing
before somebody assumes otherwise.** A claim that cannot be met is *pending*, and it lands when the
holder's `run.ended` is applied in tick *n*'s drain — the same handler releases the slot and hands
it to the head of the queue — so the launch it was blocking goes in on the hook at *n+1*:
**`1 + launchLatencyTicks`** between release and sound. A sequence boundary costs
`2 + launchLatencyTicks` (§12.1) because the scheduler has to *decide* which member is next, and a
decision costs a hook, a submit and a drain. Granting a slot decides nothing. §13.4 says why that
means the claim needs no records of its own; the shortfall against the intended launch is reported
on the run by the existing `run.late`.

### 13.2 `/godot/slot` — one table, three kinds in Phase 4 and a fourth reserved

PRD §3.9e defines a slot once, having used the word in §3.9b and §3.25 and defined it in neither:
**one position in a pool of fixed size declared at load; typed; exclusive; held for a live range;
released by a policy; with a failure policy of its own kind.** Four instances. Phase 4 builds three
and reserves the fourth.

| kind | pool declared by | typed by | released | a claim that finds none |
|---|---|---|---|---|
| `voice` | `Show/Audio/@tracks` (Phase 2) | width | at run end, as today | **fails at entry**, `no-track`, visibly — Phase 3's behaviour, unchanged |
| `processorInput` | `Mount/Slot` (§13.3) | width | **at run end**, exactly as a voice is — see below | **waits**: the claim is `pending` and lands when the holder releases |
| `rackChannel` | `Show/Audio/Rack` (§13.3) | width class | at run end | **degrades**: the cue plays dry and the run says so, `no-channel` |
| `strip` | the layout — **Phase 6** | role | when the clip's run ends | waits, or evicts |

**Released at run end, and this paragraph is a correction** *(PR 4.3, 2026-09-08)*. The table
above said *footer-timed* when it was written, meaning a claim held until the group containing the
holder had run its footer. What was built releases at **run end**, which is the same moment
`holdsTrack()` stops being true. Two reasons, and the second is the one that decided it. A group is
not done until its members are (§3.6), so by the time a footer runs the members' runs have ended
anyway — footer-timing and run-end timing differ only for the footer's own duration. And a slot
released by one rule and a voice by another is two rules that will one day disagree, in a place
where disagreeing means a cue holding a processor input nothing can take back. One rule, written in
`releaseSlotsOf` and reached from the three handlers that end a run.

**A declared slot is an object and is addressed like one.** §1's first rule — objects are
identity-addressed, and order is a separate read-only node on the container — applies to a `Slot`
and a rack `Channel` exactly as it does to a cue: `/godot/slot/<id>` wherever the element sits in
the document. So one address carries both halves, the stored rows the show decided and the derived
rows the engine computed, which is what `/godot/cue/<id>` already does with `name` beside `kind`:

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/slot/order` | `s` | ro | none | every declared slot, in document order — the container-level node §12.12 built the machinery for |
| `/godot/slot/<id>/kind` | `s` | ro | none | `processorInput \| rackChannel` — derived from which element it is |
| `/godot/slot/<id>/name` | `s` | rw | show | what the designer called it. §3.9b: the names appear in the dropdown and on the OLED, the channel numbers do not |
| `/godot/slot/<id>/address`, `bus` | `s` | rw | show | a processor input's own rows (§13.3) |
| `/godot/slot/<id>/width`, `firstChannel` | `i` | rw | show | as above |
| `/godot/slot/<id>/class`, `access` | `s` | rw | show | a rack channel's own rows (§13.3) |
| `/godot/slot/<id>/holder` | `s` | ro | none | the run holding it, or empty |
| `/godot/slot/<id>/pending` | `s` | ro | none | the runs waiting for it, in the order they claimed. A queue rather than a set, so "who gets it next" is read rather than inferred |
| `/godot/slot/<id>/usage` | `s` | ro | none | the live ranges §13.5 computes, as `<first> <last>` cue pairs — the data behind §3.9c's usage-over-show-time plot, which Phase 5 draws |
| `/godot/slot/<id>/overlaps` | `s` | ro | none | the cue pairs whose live ranges intersect on this slot |

**A slot is published out of both halves of the tree, and it has to be.** Its stored rows and its
derived `kind` come from the document half, which is a cache rebuilt when the show changes. `holder`
and `pending` cannot: they change several times a second while nothing about the show does, so
published from the cached half they would freeze at whatever they were when somebody last edited a
cue — which for a show that is running and not being edited means for ever. It is the same reason
`/godot/audio/status` is not published beside `/godot/audio/tracks`. The document half leaves the
roster of declared slots behind and the runtime half reads it, which costs one vector and no second
walk.

**Voices appear in neither table, and both absences are deliberate.**

They are not in the *namespace* because a track is not an object anybody declared: nobody wrote it
down, it has no identifier, and `@tracks` is a count rather than a list. Inventing
`/godot/slot/voice3` would put a made-up key beside real ones, and it would force one address to be
`persist = show` for a declared slot and `persist = none` for a track, which no row can be. Where a
voice is held has been readable since Phase 2 at `/godot/run/<id>/track`, and the ceiling at
`/godot/audio/tracks`.

They are not in the *implementation* either: `RunTable::isTrackBusy` and `lowestFreeTrack` stay
exactly what they are — the lowest track no unfinished run holds, lowest rather than round-robin
*so that a show replayed puts the same cue on the same track* — and the slot table asks the run
table for the voice kind rather than keeping a second copy that could come to disagree with it.
Every replay fixture in the tree depends on that answer, and a second implementation that happened
to allocate identically today would still be a second thing to keep identical for ever.

What *is* one mechanism is the rule set: a voice is claimed, held for a live range, released by a
policy and refused by a policy, and the table at the head of this section says which. Being one
mechanism does not oblige it to be one data structure, and here it is cheaper not to be. If §3.9c's
usage plot wants voices drawn beside the others, that is a derived view over the run table, and it
belongs in Phase 5 where the plot is.

**A run says what it holds and what it is waiting for**, which is the half an operator needs:

| Node | Type | Meaning |
|---|---|---|
| `/godot/run/<id>/claims` | `s` | the slots this run holds, by identifier; a voice is not among them, it is the run's own `track` |
| `/godot/run/<id>/pending` | `s` | the slots it has claimed and not yet been given |
| `/godot/run/<id>/warning` | `s` | `no-channel \| revoked` |

**`warning` is beside `error` and is not a weaker version of it.** An `error` is a run that did not
do what it said; a `warning` is a run that did something less than it meant to and is still going.
That distinction is what keeps §3.9e's *degrade* honest: a cue that lost its rack channel and
played dry is not a failure — the show continued, which is the entire point of the policy — and
reporting it as one would either stop the scene or teach an operator to ignore the state that means
stopped.

**A claim that landed late is neither**, which is why there is no atom for it here. The run did what
it said, one tick after the holder released it, so the shortfall is a measurement rather than a
warning: `/godot/run/<id>/late` already carries it in blocks — the intended launch tick is the tick
GO was applied on, so a run held by a pending claim reports its wait with no new arithmetic (§13.1)
— and `pending` above says which slot it was waiting for. In words, never colour alone (§4.8).

### 13.3 The document grows six elements, and one of them is a section

**`Mount/Slot` — a processor's input, declared where the processor is** (decision P). §3.9b marks
*"the processor declares its own slots"* as *(proposed)*: Go.dot would read a mounted namespace and
infer how many inputs exist and at what width. The author's decision is that the **show** declares
them and the mounted namespace is what a validate pass **checks against** — a slot naming an
address the capture does not contain is a warning. Discovery becomes a later authoring gesture that
writes these rows rather than a mechanism that replaces them, which is §4.10 applied to a pool: how
many inputs a processor is being used for is something somebody decided.

| `slot` row | type, default | meaning |
|---|---|---|
| `name` | `s` | *"Voix solo"*, *"Ambiance G"* — what the dropdown and the OLED show |
| `address` | `s` | the processor's input prefix, e.g. `/wfs/input/3`. Under the mount's own prefix, checked at load; the parameters underneath it are ordinary mounted nodes an osc cue writes |
| `width` | `i`, 1 | how many channels the input takes |
| `bus` | `s`, `refers=bus` | the bus that carries audio into it |
| `firstChannel` | `i`, 0 | where in that bus this slot's channels begin |

**`firstChannel` is the attribute that makes one wide send ordinary.** A rig feeding a
twelve-input processor has one twelve-channel bus, not twelve buses, and the third input is
channel two of it. It is exactly `Bus/@firstChannel`'s idea one level down — an offset into the
thing above — and declaring twelve buses to avoid it would put a hardware layout in the document
twice. `firstChannel + width ≤ bus.width` is checked when the show loads.

**`Show/Audio/Rack` and `Rack/Channel` — the pool, not the plugins** (§3.18, added 2026-09-07).
The rack is *"a pool of channels declared at load — so many of each width class — each with its
plugin chain, exactly as `@tracks` declares polyphony"*. Phase 4 declares the pool and allocates
from it; Phase 9 puts the tracks, the sends and the plugins underneath. A cue claims a channel and
Phase 4 records the claim; the audio does not yet go anywhere, and the section says so rather than
implying a rack that works.

| `rackChannel` row | type, default | meaning |
|---|---|---|
| `name` | `s` | what the designer called it |
| `class` | `s` | `mono \| monoToStereo \| stereo`. Stereo-to-mono is deliberately absent (§3.18); wider comes later |
| `access` | `s`, `exclusive` | `exclusive \| shared`. A **shared** channel is a reverb many cues send into: a bus with a chain, and §3.9e says a bus is not a slot, so it is declared, published and **never claimed** |

**`Media/Feed` and `Media/Insert` — a cue's destinations are a list** (§3.9b: *"a cue may hold a
slot and a bus routing simultaneously — a source into WFS plus a stereo feed to foldback is
ordinary"*). A `Route` sends a cue to a bus; a `Feed` sends it to a **slot**, which means to the
slot's own channels of the slot's bus, and claims the slot; an `Insert` claims a rack channel.

| `feed` row | type, default | meaning |
|---|---|---|
| `slot` | `s`, `refers=slot` | which processor input |
| `gains` | `d*` | the coefficients from the cue's channels to the slot's, row-major: length is the cue's channel count times `slot/width`, refused when the show loads if it is not |
| `shared` | `T`, false | this cue is meant to share the slot with another: §3.9c's *"allow marking deliberate sharing"*, and what silences the overlap warning of §13.5 |

| `insert` row | type, default | meaning |
|---|---|---|
| `channel` | `s`, `refers=rackChannel` | which rack channel |
| `shared` | `T`, false | as `feed/shared`, for a channel two cues are meant to share |

**A Feed is a routing and a claim in one object, and that is the point.** The coefficient reaches
hardware output `bus/firstChannel + slot/firstChannel + j` with gain `gains[i × slot/width + j]` —
the `Route` arithmetic with one more offset term, through the same code — so audio arrives at the
processor's input on the day the slot is declared, and the claim that keeps two cues from fighting
over the position, the trajectory and the LFO state behind that input (§3.9b) is the same object
that carries it there. Two separate objects would let a show route a cue somewhere it had not
claimed.

**Gains are required, exactly as a Route's are.** An identity matrix when the widths match would be
the one inference that is defensible, and it is still an inference; §3.9b's rule is that width is
explicit and never automatic. What makes it not tedious is the authoring gesture §3.9b already
names — copy-assignment and multi-select edit — rather than a default that is right until the day
it is not.

**`List/Persistent` — a section of its own** (§3.29, and §13.11). An optional identified child of
`List` holding ordinary cues, the `Header`/`Footer` precedent exactly: identified because
`cue.create` addresses a parent by identifier, carrying no attributes of its own, at most one per
list as a `validate()` rule. It is emphatically **not** a header — *"a header fires once, ahead, and
what it set is never reset, while a persistent cue re-asserts. They are opposites on exactly the
point that matters"*.

### 13.4 What the engine reports to itself, and the one thing it does not

Three engine-origin commands, registered like §11.4's and §12.3's, handlers replay-idempotent,
origin `engine`:

| Command | Args | When |
|---|---|---|
| `run.prepare` | `s` cue, `[s run]` | the horizon reached a cue or a group and made it ready: a group run is created in `preparing` and its header's preparable cues run under it. The generated ID is the record's last argument, as every generated ID is |
| `run.revoke` | `s` run | the horizon left before a GO: the pre-sent values are put back, the claims released, the run and its children ended |
| `run.assert` | `s` cue, `[s run]` | a persistent cue was found not running, or found disagreeing with the world, and was re-asserted (§13.11) |

Two operator commands:

| Command | Node | Params | Notes |
|---|---|---|---|
| `list.aim` | `/godot/cmd/list/aim` | `s` list, `s` cue, `d` offset | moves the state-position pointer. Solves and publishes; changes nothing in the world |
| `list.loadToTime` | `/godot/cmd/list/loadToTime` | `s` list, `s` cue, `d` offset, `[s run…]` | applies the solve. As many identifiers as the jump created, in the order they were made — the `go` pattern (§12.6), because a replay never draws one of its own |

`audio.arm`, `go`, `cue.fire` and `trigger.fire` are unchanged in signature.

**And the claim lifecycle has no records at all, which is a conclusion rather than an omission.**
The approved plan carried a fourth engine-origin command, `claim.land`, and writing this section is
what removed it. Every other decision in this engine is a logged command because a hook took it and
a replay runs no hooks. A claim takes no decisions:

- it is **issued** inside `armMedia`, which is already inside a handler — `audio.arm`'s, or
  `run.spawn`'s, or `run.prepare`'s — and **above that function's `audio == nullptr` return**, the
  way §12.1's new hooks sit above `beforeTick`'s. A claim is derived from the document alone: the
  cue's `Feed` and `Insert` children against the declared pool. It needs no Player, so `wfg replay`
  and `wfg serve` without `--hosted` take it exactly as a hosted session does. What stays *below*
  that return is the half that does need one — the file, the voice, the routing, the `ArmRequest`,
  and the `run.failed` reports the return exists to keep from arriving twice. So a `Feed`'s two
  halves land on opposite sides of one line, the claim above it and the coefficients below, and it
  is worth saying here rather than rediscovering it at the first replay fixture;
- it is **released wherever a run reaches `done` or `failed`** — `run.ended`'s done branch,
  `run.done` when a post-wait was holding it, and **`run.failed`, which is the exit that sends no
  `run.ended` at all**. That last one matters: a media run has taken its claims in `armMedia` by the
  time the message thread refuses a range that is not inside the file, and a release written only
  against `run.ended` would leave that slot held for the session. It is the same moment the voice is
  freed today — `holdsTrack()` is `track >= 0 && ! isFinished()`, and `isFinished()` is `done` or
  `failed` — so the claim's release and the voice's are one rule written in two places rather than
  two rules. A run *waiting* for a slot leaves the queue by the same rule, so a killed, failed or
  revoked waiter is never found at a head;
- and it is **granted** in whichever of those handlers released it, to the head of the slot's
  pending queue, which is a fact about the queue rather than a choice about the show.

**Built as drawn in PR 4.3, and one thing fell out of building it that is worth keeping.** The
claims live on the RUN rather than in a table of their own — a vector of slot identifiers held and
another of ones waited for — so who holds a slot is a scan of the run table, exactly as
`isTrackBusy` already answers who holds a track. That was not the plan's shape and it is better
than the plan's shape: a second record of who holds what is a second thing to keep in step, and the
one that is wrong is always the copy. It also means the whole of §13.2's slot table needed no new
object at all, and that a replay reconstructs every claim from the records it already had.

One guard the implementer will not see coming: `run.ended`'s handler returns early on a run that is
already `failed`, so a release written below that guard is skipped for exactly the runs whose claims
would otherwise leak.

So a replay reproduces every claim from records it already has. Adding a `claim.land` record would
be adding a second way for the model to reach a state it can already reach, and the two would
eventually disagree — which is the shape of §12.5's third finding, where a round had to be filtered
in both the hook and the handler because either one alone was wrong.

**It also makes a waiting claim one tick cheaper than a sequence boundary, and the reason is worth
keeping.** At a sequence boundary the scheduler must *decide* which member is next — the mode, the
round, the prune — so the decision costs a hook, a submit and a drain: `2 + launchLatencyTicks`
(§12.1). A grant decides nothing, so the release and the grant land in one drain and the held
launch goes in on the next tick: **`1 + launchLatencyTicks`**. Both numbers are the same
arithmetic; the difference is whether anybody had to choose.

**A pending claim holds the whole cue, not the part that wants the slot.** A media cue with a
`Feed` to a busy slot and a `Route` to foldback launches neither until the claim lands. Half a cue
is not a cue, sending it into a slot somebody else holds is the fighting §3.9b exists to prevent,
and the foldback arriving alone would be an operator hearing the cue and finding it is not the one
they fired. GO has already returned (§4.1); the row says *pending* (§3.9e); and a holder that never
ends on its own — an infinite loop — is the operator's to end, which the PRD states and this does
not try to improve on.

### 13.5 Liveness at edit time — the warning nobody has to ask for

§3.9c: each binding has a live range over show time, overlapping ranges cannot share a resource,
and the reorder warning is liveness re-analysis on edit. Over the document alone, per slot, Phase 4
computes the **live range of every claim in list order** — from the claiming cue's row to its
release, which is its group's end, or a stop cue aimed at it, or the end of the list when nothing
ends it — with exact times inside timeline and automatic chains, where offsets and durations are
known, and list positions across manual boundaries, where they are not.

**Conservative, in the direction the section names**: some ranges are indefinite — loops, manual
groups, operator-paced material — so the analysis can prove possible overlap and never impossible.
Two ranges that intersect on one slot are a **warning, never a refusal**, and marking either cue's
`Feed` or `Insert` `shared` says the sharing is meant and silences the pair. The analysis runs
**across lists**, since two lists can be live at once; a cross-list pair is the same warning.
§3.9c's *(proposed)* cross-list refusal is not built — *warn, don't refuse* is the section's own
rule, and the override it asks for is the `shared` mark, which one attribute already gives.

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/document/warnings` | `s` | ro | none | every warning the document has, one per line, each naming its own kind: the dangling references `ShowDocument::warnings()` already finds, and the overlaps this adds. Newline-separated rather than space-separated, because a warning is a sentence and sentences contain spaces |

**And an overlap must not change `wfg validate`'s exit code, which is a distinction that verb does
not draw today.** It currently returns 1 for *any* problem, a dangling reference included — fine for
a reference, which is a mistake somebody made and can fix by fixing it. An overlap is not that. The
analysis is **conservative on purpose**: it reports possible overlap and cannot report impossible,
so a show with a bed looping indefinitely and two cues on one processor input produces an overlap
that is correct, unavoidable and completely fine. A check that fails a build on findings it is
designed to over-report is a check somebody turns off within a week, and then the reference
warnings go with it.

So `wfg validate` grows a third outcome: refusals and reference warnings keep exit 1, **overlaps are
printed and leave the exit code alone**, and `shared` is how a designer says of one specific pair
that they meant it — permanently, in the document, where the next person reading the show can see
that somebody considered it. §3.9c asks for exactly that: "**Warn, don't refuse**; allow marking
deliberate sharing."

**It is a cache asked rather than told**, which is M9's shape and the handoff's instruction:
`ShowDocument` gains a revision counter, and the analysis rebuilds when the revision it was built at
differs. Not a `markStale` call somebody has to remember at every new write path — the mounted
namespace was moved off exactly that pattern in PR 3.2 for exactly that reason.
`/godot/engine/analysisRebuilds` publishes the count so **M18** asserts the guarantee by counting
rather than by timing, which is what makes such a test survive a shared CI runner.

**And the counter is a listener on the tree rather than a line in each write door** *(PR 4.4)*,
which is the same argument one level further down. `setAttribute`, `createCue`, `remove` and `move`
are today's doors; the tests write through `ValueTree::setProperty` directly; and the phase that
adds a fifth door would have to remember this one. A `juce::ValueTree::Listener` on the root hears
every property, child and order change anywhere beneath it, so there is nothing to remember and
nothing that can be forgotten. It counts CHANGES rather than edits — one `object.move` is a
remove and an add — so nothing may read the difference between two revisions, only whether they
differ, which is all a cache ever asks. The one thing it costs is that a `ShowDocument` may no
longer be copied: a copied one would share the tree, since `ValueTree` is a reference type, and hear
none of its changes.

### 13.6 Prepare and commit — the horizon, and the states a row can be in

**`run/state` gains `preparing`, and `GroupJob` gains two phases.** Today a header runs at entry.
Phase 4 runs its *preparable* part when the horizon reaches the group, and holds:

| phase | what runs | leaves when |
|---|---|---|
| `preparing` | the **prepare** of every header cue that has one, as a sequence — derived preset lines first (§13.7), then the written ones | every prepare has finished: an osc pre-send verified, a media arm armed |
| `prepared` | nothing at all | a GO adopts the run |
| `header` | the header's **remainder** — every header cue except those whose prepare *was* their execution. A prepared media cue is still in this list and is launched here | as today |
| `members`, `footer` | as today | as today |

**Prepared is not run, except where the prepare was the whole of it** — and the phase split is
therefore of *work* rather than of cues. An anticipatable osc pre-send **is** that cue's execution:
the value is at the target and read back equal, and there is nothing left for GO to do, so the cue
leaves the header's list. A media cue's prepare is an **arm** — the voice reserved, the file made
ready, the slots claimed, and the sound still to come — so it stays in the list and is launched in
the `header` phase, adopting the run its prepare created rather than spawning a second beside it.
That is §13.7's rule for a preset member, *the member still runs where it sits, at its own moment*,
applied to a cue whose own moment is the header. An osc cue whose block came out `partial` stays
too, and sends the nodes that declined to be sent early.

**The hold phase is one line and it is not decoration.** `Runner::finishPhase` moves a group on to
the next phase that has anything in it and ends the run when none has: from `header` it tries the
members and then the footer, from anything else it tries the footer, and from the footer it ends
outright. There is no branch that waits. So a job whose `preparing` cues were exhausted would arrive
there through `endOfRound`, **run the group's footer, and end the scene before anybody pressed GO**.
`prepared` is the phase `finishPhase` returns from without doing anything, and that return is the
whole of its job.

**`preparing` is a state a *group* run is in, and nothing else.** A media cue at standby is prepared
exactly as it is armed today, so its run is `armed` as it has been since PR 2.3; the members armed
underneath a prepared group are `armed` too, with the prepared run as their parent. That keeps the
new state out of every path that reasons about media.

**Which matters, because `liveRunOf` has nine callers and not the five a reading suggests.** It
means *the newest unfinished run of this cue*, and `preparing` is unfinished. Three of the nine are
gated on `kind == "media"` or on a media cue and never see a group run. The ones that do:

- **the standby cursor's wrap test** (`nextStandby`, a free function rather than a Runner method):
  a prepared group run has `iteration 0, iterations 1`, so `iteration < iterations` holds and the
  pointer would wrap to a second round of a group **nobody had entered yet** rather than leaving it;
- **`fireStandby`'s ancestor walk**, whose "already running is the ordinary case" branch would take
  the prepared run as a parent without firing the group or recording where the pointer entered — the
  scene would start from the inside out;
- **`fireStop` twice and `beginFade` once**, which look a target cue's live run up: a stop or a fade
  aimed at a scene nobody has entered should be §3.8's silent no-op, and a prepared run would make
  it act on a group that is not playing.

The ninth is **`spawnChild`'s adoption**, and it fails in the opposite direction: its test accepts
only a run whose state is `armed`, so a prepared one is rejected and a *second* run for the same
group is created beside it. Skipping `preparing` in `liveRunOf` does not fix that one — which is
why adoption is written where the next paragraph puts it.

So **`liveRunOf` skips `preparing`**, which gives the five above the answer they already expect, and
the one place that wants a prepared run asks `preparedRunOf` for it by name.

**GO adopts, and it is `fireStandby`'s own branch rather than `spawnChild`'s.** `spawnChild` does
adopt a parentless `armed` run rather than making a second one — PR 3.13's finding, that an arm
which buys nothing is worse than no arm — but that test is reachable only when the caller passes no
run identifier, which is the scheduler's own `run.spawn`; `fireStandby` always generates one and
takes the other branch, which re-parents whatever that identifier names. So the adoption is written
where the ancestor walk already is: for each group between the pointer and the list, find its
prepared run — already parented, since the horizon built the chain that way — write where the
pointer entered onto **both the run and the job** (the job holds its own copy, taken when it was
created, and `beginPhase` reads that one), and move its job into the header's remainder.

**And parenting the arms costs that test its other half.** `spawnChild` adopts a run that is `armed`
**and parentless**, and the horizon has just given every prepared member a parent — so the test
PR 3.13 wrote to stop an arm buying nothing would stop adopting the very runs this phase prepares.
It loses the parentless half. An adoptable run is one that is unfinished, still `armed`, not yet
asked to launch, claimed by no job, and either parentless **or** held under a run in the spawning
run's own ancestry. That is exactly what a prepared member is, whether the horizon armed it under
its own group — the offset-nought members of a timeline, which `beginPhase` spawns all at once with
no unclaimed test of its own — or an ancestor's header prepared it there (§13.7). Adoption
re-parents it to the spawning group's run and the prepared parent drops it from `children`; a
revocation before GO still reaches it, because until GO it is still under the run that prepared it.
Without this, a timeline's members are spawned a second time, both runs land in the phase's own
list, and the phase launches both: one cue, two voices, a tick apart — which is the failure the
parentless test was written to prevent, returning because the parent is no longer empty.

**And nothing further is needed for the nested case, because PR 3.4's descent already built it.**
`beginPhase` spawns a member only when no *unclaimed* child run for that cue exists, and adopts one
when it does — the mechanism written for a GO descending into a manual group, where the press
created the member's run before the group's job could ask for it. The adoption came with the
nested-descent fix on top of PR 3.4, and the *unclaimed* test with PR 3.5's rounds, which is what
makes it survive a loop. A prepared chain parented by
`fireStandby` presents exactly that situation one level up: when the outer group's job reaches the
inner group as a member, its run is already there and unclaimed, so the job takes it rather than
starting a second scene beside it.

**With one extension, because that test only ever had to cover one cue.** `beginPhase` asks it of
the phase's **first** cue — which was enough for a descent, where the pointer's own member is the
one the press created — and the branch that advances a sequence to its next member spawns
`phaseCues[nextMember]` with no such test at all. A horizon prepares whatever it can reach, which is
not always the first thing in a list, so the adopt-an-unclaimed-child test moves onto the advance
path beside it. Without that, a prepared cue standing second in a header gets a second run when its
turn comes, and the one that was prepared keeps its voice until the show is reloaded.

**What is preparable, decided by the parameter and never by the cue** (§3.12):

| kind | prepared how | why not more |
|---|---|---|
| `media` | the slots claimed, and — where there is an audio side — the voice reserved and the file made ready | already revocable by construction; this is Phase 2's arm with claims beside it |
| `osc` | **only** where the node is `anticipatable` and its mount `canBeAsked()`: read the target, keep what it held, write, verify | a value on a mount that cannot answer has nothing to put back (§13.1) |
| `midi` | never | MIDI has no read-back; `verified` on a MIDI cue is already refused at load (§12.11) |
| `fade`, `stop`, `memo` | nothing to prepare | a fade cannot take over a level before it is time to |

**Read-before-write is a state of the network job**, not a new mechanism. `OscJob` gains `reading`:
ask the target, wait for the `mount.readback` record, keep the answer as the restore value, then
write and verify exactly as a `verified` cue does today. The existing path clears the remembered
read-back at the moment it writes and asks afterwards, which is right for verification and is the
opposite of what a restore needs — so the order is the difference, and the state is what carries it.

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/cue/<id>/prepare` | `s` | ro | none | `idle \| preparing \| pending \| partial \| armed \| verified` |

`idle` before the horizon arrives. `preparing` while it works. Then one of four: **`pending`** when
a claim is waiting (§3.9e's word, on the row where the operator is looking), **`partial`** when
something in the block declined to be sent early — §3.6's own term for a member that is not
anticipatable — **`armed`** when there was nothing to verify, and **`verified`** when every
pre-sent value read back equal. A value that read back *different* is not a fifth word: that run
fails with `disagreed`, which `oscError` already has, and the block shows `partial` because part of
it did not take.

**The horizon is one block, not the show.** It reaches the standby's own cue; and when the pointer
is on a group, or inside one, it reaches **every group between the pointer and the list, outermost
first** — each one's header prepared in the order those headers would run — and then what the
innermost would launch first: the first member of a sequence, the offset-nought members of a
timeline, recursively, which is `armablesFor` asked further ahead. Outermost first because that is
the order a GO would run them in, and because a chain prepared inside-out would position a source
and then have an outer header move it again. **The chain is parented as it is prepared**, each
level's run to the level above, exactly as `fireStandby` parents the chain a GO creates — so
adoption has only to say where the pointer entered and move the jobs on, and a revocation of the
outermost reaches all of them by following `children`.

It does **not** reach the next sibling group. §3.12 says a header extends the horizon "from one row
to a block", and preparing two scenes at once would hold two scenes' worth of slots — in a mechanism
whose entire subject is that slots are scarce.

**Rollback is a record and the handler does the work itself.** The hook that notices needs no new
state to do it: `armedStandby` already holds the cue the pointer was on until the moment it is
reassigned, so the hook asks `preparedRunOf` for that cue and submits `run.revoke` against what it
gets, outermost run only, since the chain is parented.

The handler then queues the restore value for every node the horizon pre-sent, releases every claim,
and **ends the prepared run and its children itself** — state, track, `endedAtTick`,
`warning = revoked`. It does not ask the audio side to stop something and then wait to observe it
stopping, because a run that was armed and never launched is never observed to stop: that is the
second leak §13.13 closes, and revocation is the path that would meet it on its first tick.

**And `mount/@rateCap` is finally read.** Declared in Phase 1, unread ever since, and named in
`MountSender`'s own header as Phase 4's to honour. A prepare and a load-to-time are the first two
things in this engine that send a burst at one tick, so the sender gains a per-tick budget per
mount with the remainder spilling to the next tick — which is a rate cap in the sense §3.3 means:
a cap on outbound dispatch, not a cap on what may be asked for.

#### What PR 4.5 built, and the two places it does not match the paragraphs above

**A preparation is issued in header order and is not waited on one cue at a time**, which the table
at the head of this section calls "as a sequence" and which cannot be quite that. A header phase
runs its cues one after another because each reports done and the next begins. **A prepared media
cue never reports done** — being armed and not launched is the whole of what preparing one means —
so a chain that waited for the first would wait for ever. What the phase waits for instead is that
everything it issued has *arrived*: an arm armed, a network cue finished. The order is still the
header's, and a header whose cues write one address still leaves the last one standing, because the
sender coalesces by address inside a tick.

**A network cue is pre-sent only where there is something to put back**, which is §13.1 as a
condition rather than a preference. Both halves are required: the node marked `anticipatable`, which
is what its owner says about whether an early write is safe, and the mount able to answer, which is
what makes the restore exist at all.

**And the restore goes back as an ordinary `node.set`.** That is the whole of the mechanism and it
is deliberately not a private path: `node.set` is how any client writes a mounted node, so a
revocation is one of those with the value the target held before the horizon touched it. A replay
then reproduces the restore exactly as it reproduces every other write — the value is in the record,
the mounted tree comes out the same, and in a replay nothing is sent, because a replay's foreign
writer has no sender. Somebody running `wfg replay` at three in the morning on the show network does
not move the rig.

**The read asks ONCE, and the verify asks every tick, and the difference is not tidiness.** A verify
wants the CURRENT value, so a later answer is a better answer and the probe drops the duplicates. A
read wants the value from ONE moment — the one before anything was written — and asking again after
the answer has been submitted but before it has been applied leaves a second question in flight. That
one lands after the write and satisfies the verify that follows it, and the cue reports `disagreed`
about a desk that agreed perfectly. It showed up as a test that failed one run in three, which is
the only reason it was found at all. A read that is never answered times out, nothing is pre-sent,
and the block says `partial` — the safe direction, because a value that could not be read is a value
that must not be written early.

**`verified` is now reachable and means the desk agreed**, which is the one word of the six that is a
statement about the other box rather than about what Go.dot meant to do.

**`mount/@rateCap` is still not honoured.** It is about what leaves the machine and how fast, which
is a different concern from what may be written early, and it gets its own PR.

**The revocation finishes its runs itself, and ends them `done` rather than emptying `track`.**
`run.ended` has nowhere to put a reason — `error` is documented as failed-only — and a revocation is
not a failure: a scene was got ready and then not wanted, which is what anticipation is allowed to
cost. So `run.revoke`'s handler writes `done`, `endedAtTick` and `warning = revoked`, and releases
the slots. It does **not** write `track = -1`, which the plan asked for and which would throw away
the record of which voice was held: `holdsTrack()` is a track **and an unfinished run**, so ending
the run is the whole of letting the voice go.

**Adoption has three doors and not two.** `fireStandby`'s descent takes every group *between* the
pointer and the list; `armInternal` takes the pointer's own cue when it is a group; and `fireKind`
takes a nested one when its parent's job launches it. The third exists because **only the outermost
group a press touches may start**: an inner one that started at adoption would spawn its member on
the next tick while its parent was still running the header that comes first — the scene beginning
from the inside out, which is the failure PR 3.4 was written about, returning by another road.

**And parenting the arms had a second cost the section did not see: a promise looks exactly like a
member.** A phase takes charge of the children of its own cues and launches them, and a horizon
arming what the scene would launch next puts a child of exactly that shape under a group the pointer
is merely passing through. A manual sequence took it and started the member the operator was reading
about — with nobody having pressed anything, and §3.6's *the operator is the parent* gone. The mark
that tells them apart is `prepare` itself, which already means *ready for a GO that has not
happened*: a phase skips a child that carries one, and the four places where somebody genuinely asks
for a cue — a phase beginning its first, a timeline adopting its round, a sequence advancing, and a
GO reaching a member that was armed ahead — clear it. Clearing it is the ask, which is why it is one
function called `askedFor` rather than four assignments.

**`/godot/cue/<id>/prepare` is published from the runtime half of the tree**, against a roster of
every cue the document half leaves behind — the same shape as a slot's `holder`. It cannot come from
the cached half, which freezes; and it cannot be published for *some* cues only, or a client polling
a cue would watch its node list change shape.

**And that closed a hole that was already open.** The runtime half has published a slot's `holder`
and `pending` since PR 4.3, so `/godot/slot` and `/godot/slot/<id>` were being carried by **both**
halves of the snapshot — and `find` searches one and then the other, so the answer depended on which
it reached first. No test caught it: the case needs a show with a declared slot *and* the whole-tree
walk that counts addresses, and no fixture had both. Putting every cue in the same position made it
visible immediately.

**A `Run*` taken before a run is created is a `Run*` into moved memory**, and this cost an hour. The
run table is a `std::vector<Run>`; `prepareStandby` took a pointer to the group run it had just made,
then called `beginPreparation`, which creates a child run for every preparable header cue. The media
case never showed it — a scene whose header holds nothing preparable creates no children at all, so
the pointer stayed valid and every test passed. It took a header with a network cue in it, which is
the first thing that function ever creates a run for.

#### What M19 answered, so far *(PR 4.5, 2026-09-09)*

§13.14 asks M19 for a prepared header of twenty anticipatable osc cues, counted from the pointer
landing to `verified`. That half waits for the pre-send. The arm half, measured on the Windows box
in a Debug build:

| | the number |
|---|---|
| a scene of twenty media members, from the pointer landing to the block being prepared | **1 tick** |
| what the horizon reserved a voice for | the members the scene would launch first — one, for a manual sequence |
| the word it ended on | `partial`, because the scene's header is a memo and nothing can anticipate a memo |

**One tick, and the shape is why.** The pointer is a document write; the hook that notices it runs
at the head of the next tick and submits `run.prepare`; the handler applies inside that same tick.
The rest of the scene is armed by the scheduler as it runs, which is what a member's position in a
list is for — a horizon that armed twenty voices for a scene of twenty would hold the whole
polyphony ceiling for a scene nobody had entered.

### 13.7 The header as a preset sheet — a mark on the member (decision Q)

The author, looking at the first web client on 2026-09-06: *"since this is something that preloads
and prepares OSC parameters ahead of time, the parameters of the groups could have a preload/preset
tickbox to add them in the header… The preloaded or preset lines would appear in italics showing
they are set from a cue from the group. Double clicking on them would lead to the edit panel."*
And with the Phase 4 plan, on where the gesture points: *"we could drag a cue to be preloaded to a
header from one level or another of the nested groups it's in… a visual aide to show it's a header
item would be great and maybe some form of tendril showing the header it's in. We may have to add
cues manually too."*

| `cue` row | type, default | meaning |
|---|---|---|
| `preset` | `s`, empty, `refers=cue` | the **ancestor group whose header prepares this cue**. Empty means it is prepared with its own group, as everything is |

| `group` row | type, access | meaning |
|---|---|---|
| `headerDerived` | `s`, ro | the cues whose `preset` names this group, in list order — the lines somebody did not write |

**The mark is the decision and the line is a reading of it**, which is §4.10 and answers the first
of open questions §5's four. Nothing is copied into the `Header` element; `headerDerived` is derived
the way `order` and `headerOrder` are. So editing a derived line is editing the member — the
double-click the author describes — deleting the member removes the line with no repair rule to
write, and the two can never come to disagree because there is only one of them.

**It names an ancestor rather than a boolean**, which is what the drag gesture means: a cue three
groups deep can be prepared by its own group, by the act, or by the show's opening scene, and which
one is a decision about how early. A `preset` naming a group that is not an ancestor is a `wfg
validate` warning and is ignored — not a refusal, because the repair is somebody dragging it
somewhere sensible and yesterday's show must still open (§12.12).

**Derived lines run before written ones**, which answers §5's third: a written header cue may
reasonably depend on what the presets set — position the source, then move it — and the reverse
dependency has no natural example. A header is a sequence whatever the group's mode says
(`GroupJob.h`, and §3.12 puts prepare/commit there because "preparation has an order").

**What the horizon does with a preset member is its preparable part and nothing else** (§13.6): a
media member is armed and its slots claimed under the group's prepared run; an osc member is read,
pre-sent and verified. The member still runs where it sits, at its own moment, in its own group —
and its own group **adopts** the arm the ancestor made rather than making a second one (§13.6), so
the voice reserved early is the voice that sounds. That is the whole difference between this and
moving the cue: the header line says *this is got ready here*, and the cue list still says *this
happens there*.

#### What PR 4.6 built

**`headerDerived` is a walk of the group's own subtree, not of the show**, and that is the mark's
own rule paying for itself: a `preset` names an ANCESTOR, so every cue that could name this group is
somewhere underneath it. A value naming a group the cue is not inside is a `wfg validate` warning
and is ignored, which is what makes the local walk complete rather than merely cheap.

**Adoption had to learn that spawning a run is asking for it.** PR 4.5 marks a run the horizon armed
ahead with `prepare`, so that a phase does not launch a member nobody called for; a preset member is
armed under an ancestor's run and then adopted by its own group, and without clearing the mark at
that moment it would sit armed for ever — adopted by a phase that then refused to start it. So
`spawnChild`'s adoption branch calls `askedFor`, beside the four callers §13.6 already had.

**A cue that is both a written header cue and marked for that same header is one cue.** It is what
somebody dragging a header cue onto its own group's header produces, which is a reasonable accident,
and without the check it would be spawned twice.

**The console draws the line where the author asked for it** — in italics inside the header band,
carrying the member's own identifier so a click selects the member and the inspector opens the thing
that can be edited. The *tendril* is the member's row naming the header that gets it ready, in words:
a line drawn between two rows of a scrolling list is a thing to maintain, and the name is the same
fact said in the form §4.8 asks for.

### 13.8 The solver — a pure function, and the coordinate it works in

**The coordinate is a cue and an offset, never a wall time.** A manual list has no time between two
GOs — it has however long the actor took — so "the state at 04:12" is a question the document cannot
answer. What it can answer is *cue C has been running for `offset` seconds*, with **`offset = -1`
meaning before C has fired at all**: standby on C, nothing of C done. That is the position an
operator actually asks for when they say "take it back to cue 12", and it is what a step in §13.10's
history is.

`Solver::solve (snapshot, {list, cue, offset}) → Plan` — tick thread, no side effects, no audio, no
network. It is a function of the document and the aim, so it is unit-tested with neither, and the
same call answers load-to-time, the persistent assertion and a revocation (§13.1).

**One forward pass, and §3.13 says backward.** The section's step 1 is *"walk back through the list
accumulating the last writer of each parameter"*, which describes the answer rather than the
direction, and the direction turns out to matter: a backward walk cannot know when it is finished,
because the set of parameters the show writes is not known until the whole list has been read, so it
walks to the top in the general case and pays for the machinery of stopping early without ever
stopping early. A forward pass from the top accumulates last-writers into a map, tracks each run's
lifetime as it goes, and answers both halves in one sweep at O(cues). **This is a PRD amendment to
propose** (§13.15), not a disagreement about behaviour — the state it computes is identical.

**What a structural waypoint actually bounds, which is half of what it looks like.** §3.13 makes
group boundaries structural waypoints so "the walk can stop at one rather than going to the top of
the show". That is true of *what is running* and false of *what values are set*: when a group
completes, its footer has run and nothing it started is still going — that is what blocking footers
buy — but the values its cues wrote are still there. So the **run reconstruction** starts at the
last completed group boundary before the target and the **value reconstruction** is the full
last-writer pass, which is cheap because it is one map and one sweep. Saying so is what keeps
somebody from later "optimising" the value half to stop at a boundary and quietly losing a level
somebody set in act one.

**What the plan holds:**

| | |
|---|---|
| **standby** | where the pointer lands: positionally after the target (§3.5) |
| **runs** | what should be live: cue, its chain of ancestor groups, offset into the file, range index and pass, and for each group the round, iteration and member |
| **values** | address, value, and the cue that last wrote it — **event-kind nodes excluded**, §3.13 step 4, so a jump does not re-fire the pyro |
| **trims** | group fades partway or complete, as `base + Σ trims` (PR 3.12) |
| **confused** | every place the walk could not know, each with the default it took |

**The confused list is the feature, not the apology.** §3.24 settles the infinite-loop case as
*solve-in-practice* and says plainly that "a confused solver that says so is better than one that
guesses". Four things the walk cannot know, and what it does:

- **an infinite range** fired at an earlier manual step: which pass, and how far into it, is
  operator-timed. Lands at that range's start, pass one, and says so.
- **a shuffled group** that ran earlier with `seed = 0`: the round was drawn on the night and is not
  in the document, so which members played is unknown. Takes document order and says so.
- **a media cue with no duration** — the file is missing, or unreadable: whether it is still playing
  cannot be computed. Treats it as finished and says so.
- **a manual loop** the operator was inside: standby says which member, and nothing says which
  round. Takes the first and says so.

| Node | Type | Access | Persist | Rate cap | Meaning |
|---|---|---|---|---|---|
| `/godot/list/<id>/aim` | `s` | rw | none | 50 | `"<cue> <offset>"` — the state-position pointer of §3.13, which a client drags. Writing it is `list.aim` |
| `/godot/list/<id>/solve` | `s` | ro | none | 5 | the plan for the current aim, as JSON. A structure rather than a value, so it is one node holding a document rather than a subtree of nodes appearing and vanishing under a finger |
| `/godot/list/<id>/statePosition` | `s` | ro | none | 50 | where the last `list.loadToTime` landed — §3.13's second pointer. After a jump the two agree; after a manual tweak they do not, and that divergence is what the running view shows |

**Durations, which nothing has ever known.** `/godot/cue/<id>/duration` (`d`, seconds, ro,
`persist = none`) is read once when the show is opened, on the thread that opens it, beside the walk
that already counts tracks, buses and ranges there. Not at arm, which would know only the cues
somebody armed; not stored, because it is a fact about a file rather than something anybody decided
(§4.10). A file that will not read is `0` and a confused entry, never a refusal to load — the same
rule as a missing media file, which has failed the arm and not the load since Phase 2. A range's
length is `out - in`, and the pass count follows from it.

**It is read through JUCE's own format manager, and this paragraph used to say Tracktion's
`AudioFile`** — corrected while building PR 4.1, which is the first thing in this section the code
argued with. `te::AudioFile` needs a `te::Engine&`, and at the moment a show is read there is not
one: `wfg tree` and `wfg validate` build no audio at all, and `wfg serve` brings the engine up
three hundred lines after the document is loaded and the first snapshot is published. Standing an
engine up to ask a file's length would also set flush-to-zero on the calling thread for the rest of
the process, which this project has been bitten by once already and scopes deliberately everywhere
else.

So it is `juce::AudioFormatManager` with the basic formats registered, in `audio/MediaInfo.h`,
behind a signature that names no JUCE type — because the tree and the document both read it and
neither of them names an audio library. Tracktion reads through the same JUCE readers, so the two
agree about every format the engine can actually play; where they could differ is a format JUCE's
basic set excludes, and there the duration is nought, which is the same answer a missing file gives
and which the confused list already covers.

**The test that decides whether the solver is right**, and it is worth naming here because it is the
reason to trust any of this: for every fixture whose timing is deterministic, **the plan at tick *T*
must equal what that session's own log says was live at *T*** — the runs, the ranges, the
iterations, read back out of `run.started`, `run.ended`, `run.range` and `run.round`. The scheduler
and the solver are two answers to one question, and a solver that disagrees with the scheduler is
wrong by definition. §3.15 made every transition an event precisely so that it could be checked
afterwards; this is the check.

#### What PR 4.7 built, and the one thing it did to PR 4.4

**The walk is now shared, and that was the first thing the code insisted on.** PR 4.4's slot analysis
already reasons about when a cue is live by reading the document — rows across manual boundaries,
seconds inside a timed chain — and that is the same question this section asks. A second
implementation would have been a second thing to keep correct, with the failure arriving as a slot
warning that disagreed with a load-to-time about one show. So it moved into `cue/ShowWalk.h` and both
ask it. The solver's whole treatment of *what else is sounding beside the target* is a comparison
against numbers that walk already computed.

**Everything before the target is at its end state, and there are exactly two exceptions.** A cue
that never ends on its own — a bed, a group looping for ever — did not end at an earlier GO, so it is
still going; and inside a timed chain there was no person between the members, so the document knows
precisely what is playing alongside the target. Both fall out of the shared walk: the first from
`endsOnItsOwn`, which PR 4.4 already needed to know where a claim is given back, and the second from
the seconds it computes for a chain's members.

**The value half stops at nothing**, which is the paragraph above made executable, and there is a
test whose only job is to fail if somebody later optimises it to stop at a group boundary.

**`aim`, `solve` and `statePosition` live on the Runner, not in the document.** §4.10: an aim is
where somebody's finger is, and a show reopened tomorrow correctly has none. They are published from
the runtime half of the tree for the same reason a slot's `holder` is — the cached half would freeze
them at whatever they were when a cue was last edited — and the solve behind them is computed when
the QUESTION changes, keyed on the aim and the document's revision together, because the same aim
over an edited show is a different answer.

**What is not here yet: the equivalence test.** §13.8 names it as the reason to trust any of this —
the plan at tick *T* against what the session's own log says was live at *T* — and it belongs with
`list.loadToTime`, where there is a jump to check it against.

#### What M20 answered *(PR 4.7, 2026-09-09)*

Five hundred cues, a third of them network cues over forty distinct addresses, on the Windows box in
a **Debug** build with iterator debugging on — where every number in this suite is taken, and
roughly an order of magnitude above the shipped one.

| | |
|---|---|
| one solve | **~16 ms** |

**What that has to fit inside is a gesture rather than a tick.** `/godot/list/<id>/solve` is capped
at five hertz and a dragged aim re-solves at the drag's own rate; nothing on the GO path calls it at
all. If a drag ever feels slow the answer is in the walk, which builds a `Placed` per cue with a
string map lookup behind every attribute read — but that is Phase 5's problem, when there is a finger
to measure rather than a loop.

### 13.9 `list.loadToTime` — one record, and a run tree adopted mid-way

The handler applies the current plan as a single logged command whose applied arguments carry every
run identifier it drew, in the order it drew them — the `go` pattern (§12.6), because a replay never
draws one of its own. It:

- **ends what the jump abandons, before it builds anything.** Every run of *this list* the plan does
  not name — group runs and their jobs, the members under them, the armed run at the old standby —
  is ended the way `run.kill` ends one (§12.4): every descendant, **and no footer**. A footer is
  arbitrary and need not be an inverse, which is exactly why §3.13 recomputes forward rather than
  unwinding; running one here would be arbitrary work fighting the values the jump is about to
  send, and a footer that blocks on a fade would make the jump wait for it. The handler does this
  itself, as §13.6's revocation ends a prepared chain itself, so the jump is still one record and a
  replay reaches the same state from the identifiers already in it. **Their claims are released
  before the plan's are issued, in the same drain**, so the new claims land rather than queue behind
  runs the jump has just ended. Two things it does not touch: the runs of **other lists**, because a
  jump is scoped to its own list and two lists can be live at once (§13.5) — a slot held by another
  list is precisely the "something else" the claim bullet below means — and the **persistent
  section**, whose cues §13.11 re-asserts rather than restarts, which is what decision S's "until a
  load-to-time re-solves" already promises;
- **sets the standby** positionally after the target;
- **builds the run tree mid-way** and hands it to the existing scheduler: each group run gets its
  `round`, `iteration`, `iterations` and `seed`, its finished members get runs that are already
  `done`, its live member gets one that is armed with its launch asked for, and a timeline group
  gets a run for *every* member — the finished, the sounding and the ones still to come at their
  remaining offsets. The scheduler then continues from there on the next tick, because a group job
  re-reads the round from the run every tick rather than keeping a copy (§12.5's second finding,
  built so a prune could reach the round in progress, and paying again here).

  **Two of those fields have to be written by hand and it is not obvious which.** A run made by
  `RunTable::create` leaves `iterations` at 1 and `seed` at nought; `iterations` is otherwise set
  when a group is *fired* and `seed` and `round` when a round is *drawn*, and a jump does neither.
  A group adopted without them ends after one round, or draws its next shuffle from a seed the show
  never used. The job's own list of runs it has taken charge of is filled at the same moment, because
  the loop that claims a child on sight does not run on a job's first tick;
- **launches at an offset**: the run carries a start offset that the arm applies as the clip's own
  offset. Engine state on the run and never the document — the show says where a cue starts, and
  this is where an operator jumped to (§4.10). For a ranged cue it is the range's clip that takes
  the offset, never the range's `in`, whose distance from `out` is the length of every later pass;
  the run's range-start sample is back-dated so that the pass count and the next boundary come out
  right;
- **claims** what the plan says should be held, `pending` where something else holds it;
- and **diffs and sends**. For a mount that can be asked, the plan's addresses are read back and
  only what differs is sent — §3.13's *"a rehearsal jump is a minimal correction, not a shotgun
  blast"* — and because each answer arrives as a `mount.readback` record, the diff replays exactly.
  For a mount that cannot be asked, everything the plan names is sent, and the plan says which mount
  got which treatment rather than leaving an operator to wonder.

**A cue already playing at the wrong offset is stopped and relaunched** — settled by **M17**, taken
in PR 4.1 before anything depended on it. The arm-side offset is exact, on the sample at every
offset tried, so a jump can place a cue anywhere in a file and trust it. `LaunchHandle::nudge` is
not the alternative it looked like: it is reachable from Tracktion and from nothing Go.dot exposes,
so there was no entry point to measure and none to call. Spike 02's shape still holds — an offset
is a graph rebuild and belongs in prepare — and a nudge path is a thing to add, and measure, when
something wants it.

#### What PR 4.8 built, and the sentence it had to correct

**"Positionally after the target" cannot mean the next row.** §3.5 lets the pointer sit at the top of
a list or inside a **manual sequence** group and nowhere else, because those are the only places a GO
means anything — so a jump into the middle of a timeline scene must leave the operator **after the
whole scene**, not on its third member, where nothing they pressed would do anything. The walk now
answers `onManualPath` for every cue and the solver takes the first row after the target that says
yes. It is also what the operator wants: the scene is running, and the next press is what comes
after it.

**The plan describes the whole chain, not only the noisy part.** A `PlannedRun` says whether it is
`sounding`, `finished` or `due`, because a jump has to build the tree the scheduler is about to take
over: a member missing from the *due* end is one the group spawns a second time, and one missing from
the *finished* end is a group that thinks it has not started.

**`Run` gains `startOffset` and `startRange`**, both engine state and never the document (§4.10): the
show says where a cue starts and `media/startOffset` is that decision; these are where an operator
jumped to. `launchIfDue` launches slot `startRange` rather than slot nought, which is how a jump
lands in the right range of a playlist — every run the scheduler makes still has nought, so nothing
about a GO changed.

**What the values do here is a mount-table diff, not a read-back.** The plan's values are compared
with what Go.dot last wrote and only the differences are sent. That is a real minimal correction for
the common case and it is **not** §3.13's full sentence: a value somebody moved by hand on the desk
is not in that comparison and is not corrected. The bulk read-back — asking a mount for the plan's
addresses and diffing against what the desk actually holds — is the next PR, and this one does not
pretend to be it.

**Not here yet: the in-range offset.** A jump into a ranged cue enters the right range at that
range's start rather than partway into it. The arm-side offset M17 measured applies to the whole-file
arm, which `validate()` guarantees is the only place it can — a `startOffset` beside a `Range` is
refused, because a cue with ranges plays its ranges. Landing partway into a range needs `armRangeInto`
to shorten and shift one slot's clip, which is an audio-side change with a measurement of its own.

### 13.10 The step history — the waypoints nobody has to keep (decision R)

§3.13 keeps manual waypoints available "as a way to force a divergent world back into agreement",
and the author's decision is that **the operator should not have to keep any**: *"If this could be
invisible to the user this would be great, something they don't have to manage but can suggest
several states to pick from when needed… It would be something that keeps track of a history in the
various steps of the cuelist."*

So there is no waypoint object. Structural waypoints stay inside the solver (§13.8), and what an
operator gets is the **list's own history of steps**, kept by the engine and offered when they go
back.

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/list/<id>/history` | `s` | ro | none | the last 64 steps, newest first: `<tick>:<cue>:<g\|f\|t>` — when, what, and whether it was a GO, a fire or a trigger |

**Every applied `go`, `cue.fire` and `trigger.fire` appends a step in the handler**, which is what
makes it model state a replay reproduces rather than something a hook noticed. `persist = none`, so
`state.xml` never carries it: a history is what the machine happened to be doing (§4.10), and a show
reopened tomorrow starts a new one. It publishes from the runtime half of the tree beside the runs,
not the document half, because the document half rebuilds when something tells it to and a replay
tells it nothing.

**A step is a load-to-time target**: *before cue 12* is `list.loadToTime <list> <cue 12> -1`, which
is why the coordinate has a `-1` in it. Going back is picking a row rather than typing a time.

**And every applied trigger reads the world back**, rate-capped to once a second per mount and only
for mounts that can be asked, over the addresses the show writes to that mount. That is what makes a
step's world *observed* rather than only computed: a level somebody moved on the desk between two
GOs is in the log at the step that saw it, so a jump back to that step restores what was actually
there rather than what the cue list believes. It is the same mechanism the persistent assertion and
load-to-time's diff already need — one bulk read, three users — and **M21** prices it against
WFS-DIY's own 2 487-node capture before it is relied on.

This history is also the inventory §4.4 defers **Go Doh!** until: *"Specification deferred until the
full inventory of in-flight objects exists."* Phase 10 does not have to invent one.

#### What PR 4.9 built, and what it found out about the shape

**Three handlers, one letter.** Every applied `go`, `cue.fire` and `trigger.fire` appends a step in
its handler — `<tick>:<cue>:<g|f|t>`, newest first at `/godot/list/<id>/history`, sixty-four kept —
and the letter is the whole reason a hook could not have done it: a hook sees runs appear and cannot
tell a press from a trigger, and a replay runs no hooks at all. A `cue.fire` climbs to the list that
holds the cue, three groups down or not, so a step lands on the list whose pointer it did not move.
A literal replay of a session's log against its own show reproduces the history, tick for tick.

**The sweep is per address, not per subtree, and M21 says by how much.** A subtree GET of WFS-DIY's
capture is 1.09 MB and 2 480 nodes, and digesting it costs **135 ms** in a debug build — nearly seven
ticks, every second. A 500-cue show writes about forty addresses (M20 counted them), and forty
replies of 74 bytes cost **0.6 ms** to digest: two orders of magnitude, and 3 % of one tick. The
break-even is past eight thousand written addresses — more nodes than the capture holds — so there
is no number at which the subtree shape wins for a show, and the sweep asks about what the show
*writes*, one question each, once a second per mount. `MountProbe::Question` carries `observation`, the outstanding
set is keyed by kind as well as address so a sweep can never swallow the one question a verified cue
is waiting on, and the answer is the same `mount.readback` record with a trailing `T` — one handler,
one replay path, and every log written before this replays as what it was.

**Three stores for one address, because they are three facts.** What Go.dot *wrote* is the
decision; what the target *said to a waiting cue* is that cue's evidence and is consumed by it; what
the target was *seen to hold* is the freshest thing known about the room. `MountTable::observedOf`
is the third, and a write to the address ends it — so an observation that survives is one taken since
the last write, and "no observation" means the written value is the best account there is. That
ordering is what the jump now diffs against: a fader moved by hand between two GOs is corrected,
and one that agrees is left alone.

**What is deliberately not here.** No sweep on a `run.assert` yet — PR 4.10's assertion reads the
same store, which is the point of having one. No observation of nodes the show never writes: the
sweep is over the written set, and a desk's other five hundred faders are its own business. And a
step inside the cap is not queued for later — the next step gets a fresh sweep, because an
observation of a moment that has passed is worth less than the cost of asking for it.

### 13.11 The persistent section — checked, not fired (decision S)

§3.29: a persistent cue is the thing that should be running at all times and is relaunched if it is
not, in **a section of its own** — not the header, because a header fires once and never resets
while a persistent cue re-asserts.

| Node | Type | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/list/<id>/persistentOrder` | `s` | ro | none | the cues in this list's persistent section, in order |
| `/godot/cue/<id>/role` | `s` | ro | none | `member \| header \| footer \| persistent` — where a cue sits. Drawn in §12.5 and never built; §13.13 |

**Kinds honoured in Phase 4: media, osc and midi.** A fade, a stop or a group in the section is a
`wfg validate` warning and is ignored — a fade asserts nothing, a stop is the thing that suspends an
assertion, and a group is a lifetime rather than a state. The cursor skips the section as it skips a
footer, `standby.set` refuses its cues with `not-manual-path`, and they are not GO targets.

**The assertion is a mode of the solver, not a second mechanism** — §3.29 says so in those words.
After any applied `go`, `cue.fire` or `trigger.fire`, the next tick's hook solves for the persistent
set alone and submits `run.assert` for each cue that is not as declared: a media cue with no live
run is relaunched, an osc cue is re-sent where the read-back differs (and always where the mount
cannot be asked), a midi cue is re-sent. Checking at triggers rather than every tick is deliberate
and the PRD gives the reason: *"a tick-rate check makes a stop impossible, a trigger-rate check is
human-paced"*.

**What suspends it, and what does not:**

- a **stop cue** placed before standby whose target is the persistent cue does — it is the last
  writer and the document holds the decision (§4.10), which is exactly what the solver's own
  last-writer walk sees, so this needs no special case at all;
- a **kill from the running pane** does — §3.29's *(proposed)* item, **taken as yes** (decision S):
  run-local like a prune, for the session, until a load-to-time re-solves. Without it the operator
  fights the machine, and every other run-local gesture in this engine already evaporates the same
  way;
- a **double Esc** does not, by §3.29. Phase 10's.

A relaunch is a machine action: logged with its origin, shown on the run, and it never moves standby
(§3.5). A stateful data process loses its state on relaunch and restarts at its resting state
(§4.6); the solver cannot rebuild it, because the stream that fed it is not in the list.

#### What PR 4.10 built, and the one thing it had to add

**The section is a `List`'s child, and the cursor never had to be told.** `Persistent` is an
identified container beside the members, with the same children a header takes — so `stops()`, which
walks a container for cue elements, skips it by construction, and `standby.set` refuses its cues with
`not-manual-path` because `findOnPath` never descends into it. Neither needed a line of new code,
which is the shape saying it is right: the section is not a place the pointer can be, and nothing had
to learn that it was not.

**`solvePersistent` is the same walk read for a different question.** It takes the section's cues as
what should be true, and drops any the rows *before the pointer* stop — the same last-writer reading
`solve` makes for a jump. Nothing in the assertion knows what a stop cue is, which is what makes this
a mode of the solver and not a second mechanism; the test that proves it puts a stop before standby
and watches the bed stay silent without any special case being reached.

**The hook waits for the sweep, and gives up.** §13.10's observation asks each askable desk what it
holds on the step's own tick, and an assertion that ran before the answers arrived would compare the
plan against last second's world. So `assertPersistent` holds off until every value it is about to
assert has an observation from this step — or half a second has passed, because a desk that has gone
quiet must not stop the section asserting at all. A mount that cannot be asked is re-sent every step,
which is §3.29's own answer for a target nobody can read.

**A kill needed a field, and the run table's memory needed a date.** `run.kill` writes `killed` on
the run — `skipFooter` alone cannot say it once Phase 10's double Esc sets that too — and the
assertion reads it off the run rather than remembering the gesture, so what a replay would have
written is what it sees. But the run table keeps every run for ever, so the first version re-suspended
the cue at every step from a kill the operator had already undone: the lift records the tick, and only
a kill since then counts.

**Not here: the fixture.** `persistent.wfglog` was planned for this PR and is not written; the
assertion is a hook, so a replay re-injects its `run.assert` records rather than deciding them, and
what a fixture would add is the record shape rather than the decision. It joins 4.11's list with
#16 and #17.

### 13.12 The document layer — plumbing, and the rows in one place

Six new elements mean six entries in each of the places the schema is hand-written, and naming them
together is what stops the sixth from being discovered by a fixture failing:

- **`KNOWN_OWNERS`** in `generate-schema.py` gains `slots`, `slot`, `processorInput`,
  `rackChannel`, `feed` and `insert` — six words, and **not the six elements**, which is worth
  saying because the two sixes are different sixes. There is no `rack`: `Audio/Rack` is a container
  element like `Mounts` and carries no rows, so it needs no owner word. `persistent` is likewise
  absent because `Persistent` carries none either. And `slot` is not one element's owner but the
  one **both** declared kinds share. It gates the `refers` column as well as the `owner` column, so
  a reference to a new owner word fails generation until the word exists.
- **The containment table** gains `Slot` under `Mount` (which has no children today), `Rack` under
  `Audio` and `Channel` under it, `Feed` and `Insert` under `Media`, and `Persistent` under `List`.
- **`ownerForElement`** — a hand-written mapping, and the by-kind half of the `refers` check — gains
  one line per element. That is the whole cost of making `refers` work for a nested element, which
  is what PR 3.2 bought when it generalised the standby check rather than writing a fifth by hand.
- **The tree walk trusts an identified child's element name in two hand-written places, and the new
  elements break both.** `collectCue` gains a branch per new child of `Media`, because it recurses
  into any identified child it does not recognise **as though it were a nested cue** — so a `Feed`
  would otherwise be published at `/godot/cue/<id>` carrying a cue's rows. The list's own child loop
  makes the same mistake one level up: it calls `collectCue` on every identified child of a `List`,
  so `Persistent` needs its branch **there** rather than inside `collectCue`, which never sees the
  section as a child. It recurses into it with the *list's* identifier as parent and without taking
  a member index, exactly as the `Header`/`Footer` branch does inside a group — a section that took
  index 0 would shift every real member by one. Without it, the section is published at
  `/godot/cue/<id>` as a memo cue.
- **`orderOf`'s exclusion list is hand-written** — `Header` and `Footer` today — so `Persistent`
  joins it. `order` is the list's *members*, and a client reading `/godot/list/<id>/order` is
  reading the cue list: the console renders a row per entry, and `persistentOrder` (§13.11) is where
  the section's own cues are read. The one walk that needs nothing is the cursor, which asks
  `ownerForElement` and gets the same empty answer for `Persistent` as it does for a header — which
  is what §13.11 means by "the cursor skips the section as it skips a footer".
- **`Rack` needs none of this**, because §13.3 makes it a container element like `Mounts`, carrying
  no identifier: the loop that publishes every identified child of `Audio` as a bus passes over it
  on the empty-identifier guard it already has. Had `Rack` been identified, `/godot/bus` would have
  gained a bus with a default width, and the show's buses would have stopped being what that
  container holds.
- **An object's ADDRESS and its KIND turned out to be two questions**, and PR 4.2 found it by
  failing: a rack channel is published at `/godot/slot/<id>` beside a processor input, and the
  document's address resolver checked the segment against `ownerForElement` — which answers
  `rackChannel`, so every write to a channel was unresolvable while every read worked. They are
  separated now: `addressOwnerFor` says which container an object is addressed under and
  `ownerForElement` says what kind it is, and only one element makes them differ. Worth the note
  because sharing an address space between two kinds is what this section chose, and this is the
  cost of that choice rather than an accident.
- **`Mount` is a childless leaf today**, so `Slot` is the first child it has ever had: the
  containment entry and the branch that publishes it are both new, and there is no existing
  behaviour to extend or to break. The mounted namespace itself is published by the other half of
  the tree, under the mount's own prefix, and is untouched by any of this.
- **`slot.create`, `channel.create`, `feed.create` and `insert.create`** follow `route.create` and
  `range.create` exactly: a parent identifier, an index, and the generated id logged as applied.

**And the routing arithmetic is factored rather than copied.** `resolveRouting` walks a cue's
`Route` children and turns each into coefficients against its bus's first channel and width; the
per-bus half becomes a function of `(firstChannel, width, gains)` that a `Feed` calls after
resolving slot to bus, adding the slot's own offset within it. One arithmetic, two callers, and the
`bad-route` failure keeps its meaning for both.

**The gains length check finally happens where the table says it does.** `route/gains`'s own
description has said *"a length that does not match is refused when the show loads rather than
discovered mid-cue"* since Phase 2, and nothing has ever checked it: the only check is at arm, which
fails the run. It moves to `validate()` for `Route` and `Feed` alike, which is where the row already
claims it is (§13.13).

**Everything the parameter table gains:**

| owner | rows |
|---|---|
| `slot` (new; shared by `Mount/Slot` **and** `Rack/Channel`) | `name`; derived `kind`. What both declared kinds have, so it is written once and neither can drift from the other — the `Media` shape, where a media cue carries `{ cue, media }` because it is a cue first |
| `processorInput` (new; `Mount/Slot`) | `address`, `width`, `bus` (`refers=bus`), `firstChannel` |
| `rackChannel` (new; `Rack/Channel`) | `class`, `access` |
| `rack` | **no owner word at all** — `Audio/Rack` is a container element like `Mounts`, holding channels and carrying nothing itself. PR 4.2 corrects an earlier line here that said otherwise |
| `feed` (new; `Media/Feed`) | `slot` (`refers=slot`), `gains` (`d*`), `shared` |
| `insert` (new; `Media/Insert`) | `channel` (`refers=rackChannel`), `shared` |
| `slots` (new container, `/godot/slot`) | `order` |
| `cue` | `role` (ro — built in 4.1, and its enum likewise declares `persistent` from the start although nothing can produce it until §13.11's section exists), `prepare` (ro), `preset` (`refers=cue`) |
| `group` | `headerDerived` (ro) |
| `run` | `phase` (ro — built in 4.1, and its enum declares the two prepare phases now rather than growing under a client later: `entering \| preparing \| prepared \| header \| members \| footer`, which is wider than the three §12.2 drew), `claims`, `pending`, `warning`, `offset` (ro); `state` grows `preparing` |
| `list` | `aim`, `solve`, `statePosition`, `history`, `persistentOrder` |
| `document` | `warnings` |
| `engine` | `analysisRebuilds` |
| `media` | `duration` (ro) — on owner **`media`** and not `cue`, or every memo, group and fade would grow one: owner `cue` rows publish for every kind, while the media branch publishes `rowsForOwner ("media")`. It still addresses as `/godot/cue/<id>/duration`, which is what §13.8 writes. And `startOffset` honoured at last (§13.13) |
| `mount` | `anticipatable` and `rateCap` honoured at last (§13.1, §13.6) |
| commands | engine-origin `run.prepare`, `run.revoke`, `run.assert`; operator `list.aim`, `list.loadToTime` |

`d*` joins `route/gains` as the second list-typed row, which needs nothing special: list handling is
generic on the type suffix everywhere it occurs.

### 13.13 Debts, paid before anything is built on them

Each of these is a row, a sentence or a settled decision that already claims to be true. PR 4.1
makes them true, and they come first because the rest of the phase leans on them. They were found
by auditing, one at a time and adversarially, every claim about the current code that this section
rests on — which is worth saying because §12.15 records four such claims that survived review and
were paid for later instead.

**`media/startOffset` does nothing.** The row exists, `validate()` refuses it beside a `Range`, and
no code reads it at arm: the arm request is built from the file, the level, the routing and the
ranges. A show that declares a start offset gets none. Load-to-time needs exactly this mechanism, so
it is made true first rather than added as part of a jump.

**Two voice leaks, and revocation and load-to-time both create exactly the runs that leak.** Nothing
ends an armed run when standby moves away, so a pointer scrolled down a list holds a voice per media
cue it passed — and once every voice is held, the *next* cue the pointer reaches fails its arm with
`no-track`, which is the first symptom anybody would see and points at the wrong thing entirely. And
a kill cannot free one either: killing a run that was armed and never launched writes `stopping`,
the audio side is told, and the hook that ends a run waits to observe a playing-to-stopped edge —
which a run that never started never gives it — while the sweep that collects abandoned `stopping`
runs skips anything still holding a track. It stays `stopping`, holding its voice, for the session.
Revocation would meet the second on its first tick.

**Four things are drawn and absent, and all four are drawn in this document.** `cue/role` was drawn
in §12.5 and never built — no row, no emitter. `run/phase` was drawn in §12.2's own run table,
*groups: `header | members | footer`*, and has no row either, so `clients/console/index.html` has
been rendering an empty string there since the day the group scheduler landed. And a real log header
is **exactly two lines** — `# wfg-log 1` and
`# bundle <folder> sha256:<hex>` — so both the `# media <path> <bytes>` line of §11.6 **and the
`# clock sampleRate=… blockSize=… samplesPerTick=…` line this document shows in §7's own example**
are written by nothing at all. The clock line is the one that matters beyond tidiness: §11.5 makes
the launch tick "a pure function of the log header", and the header does not carry the numbers that
function needs. Both are written in 4.1, with the duration beside the size, since that is the moment
the file is read anyway.

**And decision K's load refusal was never built.** A `verified` cue aimed at a mount that cannot be
asked is *reported* to stderr and the show opens anyway, so the cue fires and fails on a timeout in
front of an audience — which is the exact failure the decision was taken to prevent, and it was
settled as "the strictest reading, and the one that cannot fail silently" (§9). It becomes a
refusal. Phase 4 has a second reason to care: §13.6 refuses to pre-send to such a mount too, and two
rules about the same unaskable target should not disagree about how loudly they say so.

### 13.14 What Phase 4 has to measure, and in which order

| | what | why it gates |
|---|---|---|
| **M16** | whether launching a clip in slot *b* of a track stops the clip playing in slot *a* — the launcher's one-playing-slot-per-track question (PRD §6.11) | it decides what a sampler group's claim is (§3.25, *(proposed)*), and §6.11 says it comes **before the allocator is written**. The answer goes to the author with the question |
| **M17** | a clip armed with an offset lands on the sample; and whether `LaunchHandle::nudge` on a playing clip does | load-to-time relaunches or nudges; PRD §6.11's pause-and-resume question is the same measurement |
| **M18** | the liveness analysis over a synthetic 500-cue, 20-slot show under a burst of moves: rebuilds counted, and the wall clock | one mutation, one rebuild — asserted by counting, the M9 shape |
| **M19** | a prepared header of twenty anticipatable osc cues against the mock target: ticks from the pointer landing to `verified`, and the read-before-write round trips | the horizon's cost is known before a designer builds a scene that depends on it |
| **M20** | `solve` over the Phase 3 bundle and over the 500-cue show, wall clock per call | a solve behind a dragged finger has to fit inside a tick with room |
| **M21** | bulk read-back of the WFS-DIY capture: one subtree GET and parse against one GET per written address | decides the probe's shape and the per-trigger cap |

M16 and M17 are PR 4.1's and are taken before the allocator and the jump respectively. The rest sit
with the PR that needs them.

#### What M16 and M17 answered *(PR 4.1, 2026-09-08)*

Both on the Windows box, Debug, through the hosted rig that renders to a buffer. Both **report**
rather than gate, which is the house style for a measurement whose answer belongs to the engine
rather than to us.

| | verdict | the numbers |
|---|---|---|
| **M16** | **A track keeps BOTH slots playing, and they sum** | One track, two slots, two ranges of different material, both armed looping. Slot 0 launched and playing: mean level 0.25, every sampled frame slot 0's material. Slot 1 then launched on the same track: **both slots report playing**, and the mean level goes to 1.0 — the two levels added — with no sampled frame belonging to either segment alone. |
| **M17** | **An offset lands exactly on the sample** | A ramp file whose value is its own position, armed at 0.5 s, 1.25 s and 2.0 s. Every one landed on the sample asked for: **out by 0 samples, 0.000 ms, at all three.** Not one block, not one sample: exact. |

**M16 is the one that changes something, and it changes it against the guess.** PRD §3.25's
*(proposed)* sampler claim was written around Waveform's behaviour — *"if the launcher keeps one
playing slot per track… a member launching stops whatever its track was playing, which is a
sampler's choke group for free"* — and §6.11 asked for this measurement precisely because that
sentence was a hope. It is not what this engine does. Two slots on one track sound together and
their outputs add, so **a sampler group's claim is per slot and not per track**: a bank of eight
cells that can sound at once is eight voices, and the choke group §3.27 wanted comes free from
nothing. That is a question for the author (§13.15) rather than an answer this section may take.

**M17 removes a hedge from §13.9.** *"Relaunch, unless M17 finds `nudge` lands on the sample"* has
half an answer: the arm-side offset is exact, so load-to-time can place a cue anywhere in a file and
trust it. The nudge half is **not measured and cannot be from here** — `LaunchHandle::nudge` is
reachable from Tracktion and from nothing Go.dot exposes, so there is no entry point to measure. So
load-to-time **relaunches**, and a nudge path is a thing to add and measure when something wants it.

#### What M18 answered *(PR 4.4, 2026-09-09)*

Five hundred media cues over twenty declared slots, one claim each, on the Windows box in a
**Debug** build — so the milliseconds are an upper bound with a wide margin, and the counts are
exact under any build.

| | the number | what it says |
|---|---|---|
| twenty publishes with nothing edited | **0 rebuilds** | the guarantee, asserted by counting. The cache is asked and never told |
| twenty `object.move`, each followed by a publish | **20 rebuilds** | one mutation, one rebuild. Not two for a move that is a remove and an add, and not one per publish afterwards |
| one analysis of the 500-cue show | **208 ms**, of which **131 ms** is `ShowDocument::warnings()` | the slot walk itself is ~77 ms in Debug for five hundred cues |
| one `object.move` and the publish after it | **632 ms** | of which the analysis is 208 and the rest is the document half of the tree, seven thousand nodes rebuilt and sorted — Phase 1's cost and untouched by this |

**The number worth carrying forward is the 131.** It is not the liveness analysis at all: it is the
existing reference walk, which asks `findById` once per persisted reference and each of those is a
depth-first walk of the whole show. `ShowDocument::findById` says of itself that *"if this ever
shows up in a profile, the fix is a cache invalidated in one place, not a second map maintained in
five"*. It has now shown up in one, and the cache it asks for is the revision counter this PR just
added. Left alone deliberately: it is a different concern from liveness, it is already paid at most
once per edit rather than once per tick, and it is the whole of what makes `/godot/document/warnings`
affordable to publish at all.

**Nothing here gates.** A wall clock on a shared CI runner is a flaky test that teaches people to
re-run the suite; the counts are what the design promises and the counts are what is asserted.

#### What the other four answered *(PRs 4.4-4.11, 2026-09-08 and 09)*

All on the Windows box, Debug, and all reporting rather than gating.

| | verdict | the numbers |
|---|---|---|
| **M18** | **One mutation, one rebuild** - asserted by counting, which is the only way a cache guarantee can be asserted | A 500-cue, 20-slot synthetic show under a burst of `object.move`: `engine/analysisRebuilds` moves once per applied mutation and not once per read. 208 ms per rebuild, of which `ShowDocument::findById` is 131 ms - a debt named in the close-out rather than a property of the analysis |
| **M19** | **The block settles one tick after the pointer lands** | A prepared header against the mock target: one read-before-write round trip per anticipatable node, and the settle word arrives on the tick after the last of them |
| **M20** | **17.7 ms per solve over 500 cues**, in a Debug build with iterator debugging - roughly an order of magnitude above the shipped one | 167 network cues resolving to 40 distinct addresses. The node is capped at 5 Hz and a dragged aim re-solves at the drag's own rate, so what this has to fit inside is a gesture rather than a tick |
| **M21** | **Per address, by two orders of magnitude** | One subtree GET of the WFS-DIY capture is 1.09 MB and 2 480 nodes and costs **135 ms** to digest - nearly seven ticks, every second. The forty single-value replies a 500-cue show needs cost **0.6 ms**, three per cent of one tick. Break-even is past eight thousand written addresses, which is more than the capture holds |

**M21 decided a shape outright**, which is the strongest thing a measurement can do. §13.10 was
written with "one subtree GET or one GET per address" as an open question and the sweep built either
way; the answer is that a show writes forty addresses and a namespace has two and a half thousand
nodes, so asking about what the show writes is not a compromise but the whole of it. The subtree
number stays here as the figure that says when to revisit it.

**M20 set a rate cap rather than passing a threshold.** Seventeen milliseconds is most of a tick, and
a solve behind a dragged finger would have been a tick spent on a question nobody had finished
asking. So the aim publishes at 5 Hz and the solve is cached on (document revision, aim) - and the
number to beat, if a bigger show ever needs it, is written down.

### 13.15 The direction this phase does not build

**Eviction.** §3.9e's second shared rule — *eviction is a close, not a kill* — is Phase 6's, where a
sampler group's takeover needs it. Nothing here forecloses it: a claim carries a release policy, a
release never ends a run, and the pending queue is already a queue. What Phase 4 must not do is make
a release synchronous with a stop, because a close is exactly a release that waits.

**Strips.** The fourth slot kind. The table has the column and Phase 6 fills it.

**The voices claim shape, which is the author's and now has its measurement.** §3.25 marks it
*(proposed)* and §6.11 said the measurement came first; M16 has been taken and it answers against
the guess (§13.14). A track does not choke: two slots on one track sound together and add. So a
sampler group's members cannot share a voice by sharing a track, and the shapes left are that the
group **declares its voices** and claims that many tracks, spreading members across their slots and
accepting that any two members sounding at once need two tracks — or that a claim is simply per
slot and a bank of eight is eight voices. Phase 6 builds whichever the author says; Phase 4's
allocator must only avoid assuming the choke that is not there, which is why the measurement came
before the table was written.

**The rack's audio.** The pool is declared and allocated; the tracks, the sends and the plugins are
Phase 9's (§3.18). An `Insert` in Phase 4 claims a channel and changes no sound, and the row says so
rather than implying a rack that works.

**Time-tagged bundles to Go.dot's own processors.** §3.12's second paragraph — *"own processors may
receive tick N+1 values during tick N as OSC bundles with timetags, erasing network jitter"* — is
not built, and the reason is on the other side of the wire: spatcore's OSC parser drops bundle time
tags, so the author's own processors would ignore the timetag and apply the values early, which is
worse than not sending them. It wants a spatcore change first, and that is recorded in the reuse map
rather than attempted here.

**Authored waypoints**, per decision R. **A group-level persistent section** and **Esc as a pause on
persistent media**, both *(proposed)* in §3.29 and both left there. **The other three read-back
mechanisms** (§11.7): a polled get-convention, a subscription, a bespoke sync command —
`mount/@readback` names the mechanism precisely so each becomes another word, and Phase 4 adds none.

**PRD amendments this phase will propose at close-out**, recorded now so they are not rediscovered:
§3.13's *"walk back"* against the forward pass (§13.8); §3.9b's *"the processor declares its own
slots"* against decision P; §3.12 gaining the read-before-write sentence that makes revocation
mean something; and §3.9c's *(proposed)* cross-list refusal, which the `shared` mark replaces.

### 13.16 What Phase 4 built, against what §13 drew

Written 2026-09-09, at the head of Phase 5 rather than at close-out, which is where §13's own
opening put it: *"where this section and the code come to disagree, §13.16 at close-out says
which won"* (§13, line 1728). PR 4.11 wrote `docs/godot-phase4-closeout-0.1.md` and
`docs/handoffs/2026-09-09-phase5-handoff.md` and did not write this, so PR 5.0 pays it, from the
close-out's own §1, §2 and §3. A retrospective written a phase late is written from the commit
messages rather than from the week, and the commit messages are better than a memory but they
are not the week.

**§13 was drawn at 798 lines and is 1 311 today, and almost all of the growth is
retrospective.** It was written on 2026-09-07 in PR 4.0 (`7109cf8`), before a line of code. A
six-lens review of it — contradictions, references, completeness against the approved plan,
could-somebody-build-this, voice, and what-would-this-break, with every finding put to an
independent reader whose job was to refute it before it could be reported — returned fourteen
survivors and took the section to **894 lines** (`73482a6`, whose subject line is *"Five things
section 13 got wrong, found by reading it against the code"*).

**The five the review caught are not in the lists below, which is the whole argument for having
it.** A claim issued below `armMedia`'s null-`Player` return would not exist on a replay, which
falsifies §13.4's entire case for having no `claim.land` record. Releasing only in `run.ended`
leaks a slot for the session, because `run.failed` sends no `run.ended` at all. A prepared media
cue in a header would never have sounded, because its prepare is an arm and the phase table had
lost §13.7's own distinction between work and cues. Parenting the prepared arms breaks the
`spawnChild` test that adopts them, PR 3.13's parentless half. And load-to-time never said what
it does with what is already playing, which also made the claim bullet unreadable: every jump
would have landed pending on its own predecessor. Each was a fault in a text, found by reading
the text against the code, at the cost of an afternoon. One caveat is honest and useful: PR
4.5's own commit records the adoption test as something *the black-box found rather than a
reading* — so the reading found the sentence and the driver still had to find the code.

**The five things §13 got wrong once the code read it back.**

- **Durations were drawn coming through Tracktion's `AudioFile`** (§13.8), and PR 4.1 corrected
  the paragraph in place while building it — the first thing in the section the code argued
  with. `te::AudioFile` needs a `te::Engine&` and at the moment a show is read there is not one:
  `wfg tree` and `wfg validate` build no audio at all, and `wfg serve` brings the engine up
  three hundred lines after the first snapshot publishes. Standing one up would also have set
  flush-to-zero on the calling thread for the rest of the process. It is
  `juce::AudioFormatManager` in `audio/MediaInfo.h`, behind a signature naming no JUCE type.
  Cost: nothing, because the sentence was wrong before anything leaned on it.

- **A processor input's claim was drawn footer-timed** (§13.2's table). PR 4.3 released at run
  end; the correcting paragraph was committed in PR 4.4 (`37b1c99`) and signs itself *(PR 4.3,
  2026-09-08)*, with the pull request whose code it corrects rather than the one that typed it,
  which is the convention worth keeping. A group is not done until its members are, so the two
  timings differ only by the footer's own duration — and a slot released by one rule and a voice
  by another is two rules that will one day disagree, in a place where disagreeing means a cue
  holding a processor input nothing can take back. One rule, in `releaseSlotsOf`, reached from
  the three handlers that end a run.

- **A prepared header cannot be run "as a sequence"** (§13.6's phase table). A header phase runs
  its cues one after another because each reports done, and **a prepared media cue never reports
  done** — being armed and not launched is the whole of what preparing one means — so a chain
  that waited for the first would wait for ever. What PR 4.5 waits for instead is that
  everything it issued has *arrived*: an arm armed, a network cue finished, in the header's own
  order.

- **"Positionally after the target" cannot mean the next row** (§13.9). §3.5 lets the pointer
  sit at the top of a list or inside a manual sequence group and nowhere else, so a jump into
  the middle of a timeline scene has to leave the operator *after the whole scene*. PR 4.8's
  test found it as a standby that would not set at all; the walk now answers `onManualPath` for
  every cue and the solver takes the first row after the target that says yes.

- **A revocation was drawn emptying `track`** (§13.6: *"state, track, `endedAtTick`, `warning =
  revoked`"*). It does not. `holdsTrack()` is a track **and** an unfinished run, so ending the
  run is the whole of letting the voice go, and writing `track = -1` would throw away the record
  of which voice was held — exactly the readout an operator wants after a scene was got ready
  and then not wanted.

**Six things §13 drew and the engine turned out to need differently.**

- **An object's ADDRESS and its KIND are two questions** (PR 4.2). §13.12 drew `ownerForElement`
  gaining one line per element, which is what it costs while one element has one owner; §13.2
  then chose to publish two declared kinds at one address space. `ownerForElement` has to answer
  `rackChannel` so `refers` can tell the two apart, and the document's address resolver was
  asking it — so every *read* of a rack channel worked and every *write* was unresolvable.
  `addressOwnerFor` is the second function, and one element makes them differ.

- **RELAX NG content is ordered, and no element had ever had both kinds of child** (PR 4.2). The
  generator emits container children as optional refs and identified ones as a `zeroOrMore`
  choice, one group after the other, so a container child had to come first. `Show` is all
  containers; a `List` and a `Group` are all objects; `Audio` with a `Rack` among its buses is
  the first element with both, and the first place a show could be written in an order its own
  grammar rejected — which the canonical writer would then produce. It is the same class of miss
  as §12.15's third finding, one phase later, in the same generator.

- **A promise looks exactly like a member** (PR 4.5). §13.6 drew the horizon arming under the
  block so that a revocation reaches every arm by following `children`, and did not draw that a
  phase takes charge of the children of its own cues and launches them. A manual sequence took
  the arm and started the member the operator was reading about, with nobody having pressed
  anything and §3.6's *the operator is the parent* gone. The mark that tells them apart is
  `prepare` itself, and clearing it **is** the ask — one function, `askedFor`, rather than four
  assignments, and PR 4.6 added the fifth caller because spawning a run is asking for it too.

- **The read asks once and the verify asks every tick** (PR 4.5's second half). §13.6 drew
  `reading` as a state and drew the order — ask, keep, write, verify. It did not draw that
  asking twice is a fault: a second question in flight lands *after* the write, satisfies the
  verify that follows it, and the cue reports `disagreed` about a desk that agreed perfectly. It
  surfaced as a test that failed one run in three, which is the only reason it was found at all.

- **A rate cap is per node, and a message that is waiting is not a message that failed** (PR
  4.5's third half). §13.6 drew the budget and not the ticket. `pending` had meant "the flush
  never ran", which is a wiring fault; under a cap it means queued, in order, holding the newest
  value, so the cue keeps waiting up to its own timeout. Without that, capping a mount would
  have turned every `sent` cue on it into a failure.

- **A kill needed a field and the run table's memory needed a date** (PR 4.10). §13.11 drew a
  kill as suspending a persistent cue until a load-to-time re-solves. `skipFooter` alone cannot
  say *killed* once Phase 10's double Esc sets it too, so `run.kill` writes the word — and
  because the run table keeps every run for ever, the first version re-suspended the cue at
  every step from a kill the operator had already undone. The lift records its tick, and only a
  kill since then counts.

**What the black-box driver found that no unit test could, which is the part Phase 5 should read
twice.** `tests/blackbox/phase4_prepare.py` is 757 lines and **fifty-two checks**, driven
over UDP and HTTP against the shipped binary with `tests/fixtures/bundles/phase4/` and
`mock_target.py` beside it. Three of the fifty-two failed against real engine faults, and a
fourth fault turned up between the engine and its own replay. **Each is a seam between two
things that are separately correct**, which is why the 628 `TEST_CASE`s the unit target carries
at `2d0504b` could not see any of them:

- a scene whose header is **entirely derived** read `partial` for ever, because `settledWord`
  counted the block's preparable cues against the *written* header's members — so a scene
  prepared entirely from `preset` marks, which is the shape §13.7 encourages, compared one
  against nought. Both counts were right; they were counts of two different things;
- a **plain media cue at the pointer** held its voice and its slots for ever: PR 4.5 revoked
  prepared *blocks* when the pointer moved on, and a media cue armed at the pointer is the
  smallest horizon there is. The sharpest detail is that **three unit tests had encoded the
  leak** — they made a slot's holder by parking on a cue and walking away, which is exactly the
  gesture that should give it back. They fire the cue now, which is the honest scenario anyway;
- a **preset network cue standing second in a scene** stopped the scene: the horizon had already
  run it, so the member phase marked the finished run asked-for and then waited for something
  `armed` to launch, which never came. For a cue that was read, pre-sent and verified there is
  no rest, so the phase walks past it — §13.7's own rule, met from a direction §13.7 did not
  draw;
- and the fourth: **a jump did not replay.** `list.loadToTime` is one record whose handler
  *solves*, the solve needs media lengths, and `wfg replay` had none — so it planned a different
  show and drew three identifiers where the session drew five. It reads them off the log's own
  `# media` header lines now, which is what PR 4.1 wrote them for.

**And the rule §12 left behind was broken once more and caught the same way.** PR 4.5's
`prepareStandby` asked for its children with `run.spawn` — which a live session logs and a
replay then produces a second time, one cue and two runs a tick apart. The Phase 3 black-box
replay caught it in the afternoon it was written, exactly as a replay fixture caught the Phase 2
instance §11.9 records. Twice in three phases, both by a replay, neither by a unit test.

**A seam no fixture could have had.** Since PR 4.3 the runtime half published a slot's `holder`
and `pending`, so `/godot/slot` and `/godot/slot/<id>` were carried by **both** halves of the
snapshot, and `find` searches one and then the other — the answer depended on which it reached
first. Seeing it needs a show with a declared slot *and* the whole-tree walk that counts
addresses, which no fixture had until PR 4.5 put every cue in the same position. `addContainers`
must exclude the containers the runtime half owns, and that sentence is in the handoff because
§13 did not have it.

**The time this phase actually lost was not lost to faults**, which is the fact the closing
paragraph rests on. Three red CI builds to `-Wshadow`, which MSVC does not say and the strict
Linux build says with `-Werror` — twice inside PR 4.5, once on PR 4.9 at `47a7b34`, where a
cache member called `written` met a local that had been there since Phase 3. One whole-file
repair commit (`4578781`) because a script read `Runner.cpp` keeping its line endings and wrote
it converting them again: correct exactly once, and after that three multi-line edits reported
success and changed nothing, **silently**. And four commits after PR 4.9 that do nothing but
make the drivers wait for the thing rather than for the time (`bcd84ef`, `a9659d5`, `0e6ec2c`,
`27112f2`) — one of which was not a flake at all, `phase1`'s echo check asserting something the
protocol does not promise, that a client subscribed fast enough. The handoff's §4 carries all
three as traps, which is where a builder will look for them.

**What §13 drew that was right, and is worth saying so.**

- **The claim needs no records of its own** (§13.4). Argued before anything existed, proved by
  PR 4.3: a claim is issued in an arm, released in the handler that ends the run, granted to the
  head of a queue in that same handler, and creation order *is* the queue, so a replay hands the
  slot to the same run. The approved plan's fourth engine-origin command, `claim.land`, was
  removed while §13 was being written and has not been missed once — and the handoff's rule 2
  still lists it (`:57-61`), which is the one line in that document Phase 5 should not copy:
  there is no `claim.land` anywhere in `src/` outside the comment in `cue/Run.h` explaining its
  absence.
- **A namespace under-determines an implementation, and that is a feature.** §13.2 drew
  addresses where the approved plan carried a `SlotTable` of `SlotClaim` records wired through
  four sites. PR 4.3 put the claims on the run — who holds a slot is a scan of the run table,
  exactly as `isTrackBusy` already answers who holds a track — and contradicted not one line of
  the drawing.
- **The revision counter as a listener rather than a line in each write door** (§13.5, PR 4.4).
  Four doors today, tests that write through `ValueTree::setProperty` directly, and a fifth door
  in some later phase that would have had to remember. Phase 5 inherits it as the invalidation
  source three caches already ask.
- **One walk asked three questions** (§13.1). PR 4.7 found that PR 4.4's liveness walk was the
  same question the solver asks and moved it into `cue/ShowWalk.h` rather than writing a second
  — with the failure a second one would have produced named in advance: a slot warning that
  disagreed with a load-to-time about one show. And **§13.12 named every place the persistent
  section needed code and every place it did not**: PR 4.10 found `stops()` and `findOnPath`
  skipping the section by construction, and the list's own child loop, `orderOf`'s exclusion
  list and `ownerForElement` each needing their line — exactly the three the plumbing list had.
- **§13.13's four debts, eight claims between them, were all real and were paid first**:
  `media/startOffset` read by nothing, two voice leaks, `cue/role` and `run/phase` drawn in this
  very document and emitted by nothing, a log header missing both the `# media` line §11.6
  specifies and the clock line §11.5 calls the launch tick a pure function of, and decision K's
  load refusal never built. That is §12.15's own complaint answered: four claims survived §12's
  review and were paid for later; §13's were audited adversarially and paid in PR 4.1.

**What the measurements did**, since §13.14 already carries the numbers: M16 falsified the guess
§3.25's sampler claim was built on and is now in the PRD, M17 removed a branch before it was
written, M20 set a rate cap rather than passing a threshold, and M21 decided a shape outright,
which is the strongest thing a measurement can do. M18 measured what it was asked and found a
debt in something else entirely, which is the first item below. And the phase proved the thing
§13.8 could only assert: *a solver that disagrees with the scheduler is wrong by definition* is
a test as of `2d0504b`, agreeing at six independent moments across a timeline group and an
automatic sequence, including the two where more than one cue is sounding at once.

**Five PRD amendments came out of the phase, and every one of them is still waiting on the
author.** They are close-out §1, proposed and never applied — `CLAUDE.md` is §4 of the PRD
reproduced byte-for-byte and gated by `scripts/check-claude-md.py`, so a phase that edited the
PRD would either break that gate or silently rewrite the review criterion for its own pull
requests. Each is one sentence that building the thing made exact:

- **§3.9b, *"the processor declares its own slots"*.** The show declares them, under the mount,
  as `Slot` rows carrying a name, an address prefix, a width and the bus that feeds them
  (decision P). What a processor that *can* be asked adds is a **check**, which is a different
  verb: `wfg validate` warns when a declared slot's address is absent from the mounted
  namespace. Most devices run no OSCQuery server at all, and a pool that only existed when a box
  answered would be a pool that vanished when somebody unplugged it during focus.
- **§3.13, what a manual waypoint *is*.** Decision R settled that there is no waypoint object:
  the list's own history of the last sixty-four steps, `<tick>:<cue>:<origin>`, is the thing,
  and each step is a load-to-time target, so going back is picking a row rather than authoring
  anything.
- **§3.12, read before write.** §3.12 says what anticipation is and not what makes it revocable.
  The condition is exact now: a value is pre-sent only where the node is `anticipatable` **and**
  its mount can be asked, and the value that was there is read first and kept on the run as the
  restore — because a value nobody can restore is a value nobody can revoke.
- **§3.9c, the cross-list override.** The *(proposed)* refusal is withdrawn in favour of
  `Feed/@shared`, a mark on either cue saying the sharing is deliberate; the section's own rule
  is *warn, don't refuse*.
- **§3.29, the persistent section's suspensions.** Two *(proposed)* items were taken as yes
  (decision S): a kill on a persistent run suspends it until a load-to-time re-solves, and a
  stop cue before standby aimed at it suspends it because the document says so. A group-level
  section and Esc-as-pause stay *(proposed)*.

**And one amendment §13.15 promised is not among the five.** §13.15 named §3.13's *"walk back"*
against the forward pass (§13.8: *"One forward pass, and §3.13 says backward"*), and the
close-out's §3.13 entry is decision R's waypoint sentence instead — one promised amendment
quietly became a different one about the same section. It is still owed: the solver walks
forward, §3.13's step 1 says backward, and §13.8 argues why forward is right, because a backward
walk cannot know when it is finished until the whole list has been read. Phase 5 carries it as a
sixth. Beside the six sit close-out §4's questions that are the author's rather than the
implementer's — the voices claim shape (§3.25, before Phase 6), §3.30's idle-colour policy,
whether a rack channel's failure policy is *degrade*, and Phase 3's own waiting amendments.
§3.30's is the one Phase 5 walks straight into, because §14.12 builds the spectral colour that
raised it.

**The debts carried out of Phase 4.** `ShowDocument::findById` is a depth-first walk per call
and most of M18's analysis; its own comment asks for a cache invalidated in one place, and PR
4.4 built that place. The equivalence test wants extending from the rig to the four committed
replay fixtures — `chain`, `rounds`, `ambience` and `group-fade`, whose moments have to be read
back out of their own logs. Three replay fixtures, `slots`, `claims` and `persistent`, each of
which would pin a record shape rather than a decision. The in-range offset, which needs
`armRangeInto` to shorten and shift one slot's clip and a measurement of its own. And the
console owes PR 4.2's slot inspector lines and PR 4.3's claims-and-pending on the running pane;
4.4's warnings, 4.5's prepare word, 4.6's derived lines, 4.8's aim bar, 4.9's steps and 4.10's
persistent band are in. It is 1 725 lines, and Phase 5 makes it the operator client.

**One thing about the form, for §14 to copy.** PRs 4.1 to 4.4 corrected §13 in place, in the
register §13.2's own *"this paragraph is a correction"* set; from PR 4.5 to PR 4.10 each pull
request added a **"What PR 4.N built"** subsection instead — six of them, and PR 4.11 added
none, which is how this retrospective came to be owed. Both forms are kept on purpose, and the
rule between them is which reader is being protected: a correction in place is right when the
drawn sentence is simply wrong and would mislead anybody reading it afterwards, and a subsection
is right when the drawing was right as far as it went and the building found more, because
deleting the drawn sentence would delete the evidence that the drawing worked. §14 should do
both, and say which it is doing.

**The practice is confirmed, and Phase 4 came out closer to its drawing than Phase 3 did.**
§12.15 records four faults in §12's drawing and four audited claims that were paid for late;
§13's audit paid its debts in PR 4.1, and the review of the drawing found five more faults that
were in the text and would otherwise have been in the code. The phase then spent more time on
carriage returns, `-Wshadow` and driver waits than on anything the drawing got wrong. The
qualification is that a drawing is only as good as the reading it gets: §13's wrong line
numbers, its five-that-were-six and one enum atom with no producer all survived the author's own
re-reading and were caught by a review whose rule was that every finding be put to an
independent reader — two of whose reviewers were themselves refuted, and whose proposals were
dropped. Draw §14, then have it refuted.

---

## 14. Phase 5 — undo, crash-safe save, the edit lock, spectral colour: what the tree, the commands and the log gain

Written on 2026-09-09, before any of it exists, as §11, §12 and §13 were: the approved Phase 5
plan drawn as a text the pull requests 5.0–5.19 can be reviewed against rather than against
memory. Rows reach `docs/parameters/godot-parameters.csv` with the PR that implements each of
them, never before. Where this section and the code come to disagree, §14.17 at close-out says
which won — and §13.16, owed since 2026-09-07 and never written, is written by PR 5.0 out of the
Phase 4 close-out's own §1–§3 rather than left owed a second time. The hygiene every PR is held
to — `-Wshadow` checked by hand before a push, every standard-library include written out —
stays where it is useful, in the Phase 5 handoff's §4 traps, and is not restated here.

Four decisions the author took with the plan shape it, **T**, **U**, **V** and **W** in §9,
after S:

| | decision | what it shapes |
|---|---|---|
| **T** | the engine half is built first; the console grows into the operator client and is the laboratory where the author designs the layout; the JUCE desktop client is an **outline** in §14.16 until the layout has stopped moving | the order of every PR in the phase, and §14.16 |
| **U** | the devplan's done-when stays as written — *the author runs a simple show from the desktop build in a rehearsal room* — and whether a browser on the booth machine satisfies it is judged in the room, not here | what "finished" means, and nothing else |
| **V** | the console **becomes** PRD §3.17's web client: ES modules served from the same directory, no build step, editable while a show runs, diagnostics behind a *tech* display preset | §14.2, §14.3, and the whole of Half B |
| **W** | the edit lock is an **engine node** every client honours, not a per-client hiding of buttons; while set, document-mutating commands refuse, and GO, standby, every run command and `node.set` on a mounted address keep working | §14.1, §14.7, §14.11 |

Twelve further decisions were taken with the plan rather than by the author. They are numbered
1–12 there and cited here as *plan decision N* — one undo domain, the coalescing window, what
lights `dirty`, the autosave numbers, the timbre window and ramp, the fade-points shape among
them — and each is an implementer's call written into this section so that it can be overruled
early rather than late. Where §9's lettered decisions are law until the author changes them, a
numbered one is a proposal that has been built on, and every subsection that states one says so
in place.

Four words, used precisely from here on. *The console* is the page as it stands, one file at
`clients/console/index.html`; *the web client* is what decision V grows it into; *a client* is
anything at the far end of §14.2's contract — the console, the JUCE client §14.16 outlines, a
Max patch, `curl`. And the thing decision W adds is **the edit lock**, one node and one
predicate (§14.11); *show mode* is what the operator calls the state the lock puts the show in,
and it is never a mode in a client.

**Phase 5 is the first phase with two halves, and the order between them is a decision rather
than a convenience.** Half A is the engine: per-domain undo (§3.20, §4.3), atomic save with a
`dirty` that is true, crash-safe autosave and recovery, the lifecycle commands that do not
exist, the edit lock, and the spectral-colour cache with `run/timbre` (§3.30). Half B is the
console — today one file, 1 725 lines of plain HTML with no build step, served by the engine at
`/ui` — growing into the operator client one view per PR, which is §14.3. *(The Phase 5 handoff
records 1 400 lines at its `:44`: a figure from the middle of Phase 4, around PR 4.5. The file
was 1 280 lines when that phase opened and 1 725 when it closed. This paragraph corrects it in
place, once, rather than leaving two documents disagreeing about a number anybody can count.)*

This section then runs in the order the engine grows. §14.1 and §14.2 come first because every
row and every command after them is a promise to a client, and §14.3 is Half B. Then the tree:
the rows `/godot/document` gains (§14.4), the runtime nodes and the media route (§14.5), the two
new document attributes (§14.6), the operator commands (§14.7) and the one record the engine
writes to itself (§14.8). Then the four mechanisms — undo (§14.9), save, autosave and recovery
(§14.10), the lock (§14.11) and spectral colour (§14.12). Then the plumbing with every new row
in one table (§14.13), the measurements (§14.14), what the phase deliberately does not build
(§14.15), the desktop outline (§14.16) and the close-out that will answer all of it (§14.17).

Half A first, for four reasons that are all the same reason. PRD §3.17 says the desktop layout
is **deliberately undesigned** — *"not a QLab copycat"* — and names it the author's to design,
which means the phase cannot start by implementing it. The PRD never commits the desktop UI to
JUCE at all; only the devplan does, and §9's open question E — in-process or a separate client —
is still open, so building the JUCE client now would answer E by accident. **This section does
not answer E either**, and that is the convention it keeps throughout: E stays in §9's open
table with the fallback recorded there, *assume separate*, and §14.16's outline is drawn on that
fallback rather than on a decision nobody has taken. Everything Half B could draw is a node or a
command the engine does not have yet. And the author designs by looking: a page that reloads
while a show runs closes the loop in seconds, and a compiled client closes it in a build.

*(The handoff opens on a different sentence — "Phase 5 is the first build a human runs a
rehearsal with: a JUCE desktop client, as a pure OSCQuery client" — and calls it the most
important one in the document (`docs/handoffs/2026-09-09-phase5-handoff.md:8-10`). Decisions T
and V change it, and this paragraph says so rather than quoting around it: the client Phase 5
builds is the page, held to exactly the constraint the handoff was insisting on — a pure
OSCQuery client with no privileged access — and the JUCE one starts when the layout has stopped
moving. The rest of that opening is untouched and is what puts Half A in front: a capability the
client needs goes into the engine first, with a row in the parameter table and a command by
name.)*

### 14.1 What a client may hold, and what it may not

**A client holds what is true of the person looking at it, and nothing that is true of the show
or of the engine.** PRD §3.5 settles the hardest case before anybody asks it, by giving the
three kinds of pointer three different scopes:

| Pointer | Scope | §3.5's own words |
|---|---|---|
| **Standby** | one per list, **engine state** | *"what GO acts on; never moves as a side effect of selection or scrolling"* |
| **Edit / selection** | **per client** | *"desktop and tablet each have their own; detached from standby"* |
| **Run** | *n*, one per active group | *"live objects, selectable in the running pane, individually addressable"* |

That table is a general rule wearing a particular hat. A pointer is engine state when the
machine acts on it, per client when a person merely looks through it, and an object when it is a
thing the engine made. Everything a Phase 5 view wants to remember sorts into those three, and
the sorting has a test with three clauses:

- **If a second client should see it, it is a node.** Two surfaces are one console — §3.17 makes
  the tablet *"a second surface, not a second screen"* — and two surfaces that disagree about
  what GO will do are not one console.
- **If a second operator would be confused to find it changed, it is a node.** The confusion is
  the evidence: it means somebody was relying on it, which means it was a fact about the show
  rather than about a viewer.
- **If losing it costs nothing but a moment's re-aiming, it is the client's.** A tab closed at
  04:12 and reopened is one `GET /godot` and a scroll away from where it was. That is the price
  of holding it, and it is the right price.

Worked through case by case, because the interesting ones are the ones that look like the other
kind:

| what | whose | why it falls there |
|---|---|---|
| the **standby** | engine, one per list | §3.5 in those words. It is `persist=state`, so a rehearsal reopens where it was left (`docs/parameters/godot-parameters.csv:29`) |
| which list has **focus** | engine | GO acts on it — see below. `/godot/list/focus`, `rw`, `persist=state` (csv:24) |
| the **aim** | engine | `/godot/list/<id>/aim` is `rw` and drives `solve` and `statePosition` (§13.8, §13.9). A second client watching a load-to-time must see the same target: §3.17's dual-touch has two fingers setting a jump, and the operator at the desk has to see where the designer in the house is about to take the show *before* it goes |
| the **lock** | engine (decision W) | one row, `/godot/document/locked`, and every client honours the same one; §14.4 draws the row, §14.11 argues the refusal |
| a **run** | engine object | `/godot/run/<id>`. Which run is *selected* in the running pane is the client's; the run is not |
| the **selection** | client | `picked` at `clients/console/index.html:669`, whose comment says what it is by saying what it is not: *"the inspected cue - NOT the standby (§3.5)"* |
| the **fold state** | client | `folded`, a `Set` of group ids at `index.html:667` |
| the **display preset** | client, in `localStorage` | `design`, `tech` or `show` is which diagnostics this reader wants on this screen (plan decision 10, §14.3) |
| **scroll position** | client | and it survives a poll only once 5.9 has stopped replacing a pane's `innerHTML` wholesale, which the page does ten times a second today (§14.3) |
| the **slider under a finger** | client — but the *gate* is a node | where the finger is is nobody else's business; that this origin holds this node is everybody's, which is why §3.16's `node.touch` and `node.release` are commands and the touch table is the engine's (`tree/TreeCommands.cpp:260, 287`) |
| the **zoom of a waveform** | client | a view of a file at a magnification one person chose. The pyramid it reads is content-addressed and cacheable (§14.5); the zoom is not worth a byte anywhere but here |

**The *display preset* is named that way here to keep it away from a word the PRD has already
spent.** §3.16's **layout** is the engine-side binding layer that maps a run's timbre onto a
strip's colour cell (§14.12), and PRD §3.23 calls a *layout preset* something the show owns and
show mode locks — split view, script beside cue list. Both are document state and both are Phase
6's. What §14.3 puts in `localStorage` is a different object with a smaller life: which
diagnostics one reader wants on one screen, on this machine, until they change it. The engine
has no opinion about that one and no business keeping one per browser — plan decision 10, here
to be overruled early rather than late, and the cheapest of the twelve to overrule, since moving
it into the tree later is one row.

**Which list has focus is nearly the client's and is not, and the reason is one word: GO.** It
looks like a selection — it is what somebody clicked, it decides what they are looking at, and
moving it changes nothing in the world. But `go` acts on the focused list's standby, so a focus
that lived in a browser would mean the same GO did different things depending on which tab sent
it, which is the fighting §3.5 separates the pointers to prevent. `cue/CueList.cpp:353-358`
records the day it moved: *"Focus was a string on this object: engine state, unpublished,
forgotten on every close. Now it is `/godot/list/focus`, so a client can read which list GO acts
on, a surface can move it, and a show reopens on the list the operator was working in."* The
selection sits two lines away in the same page and goes the other way for the same reason: GO
does not act on it.

**A lock that lives in a client is a lock exactly one client honours, which is why decision W
puts it in the engine** — §14.11 argues the predicate and names the doors. The reason that
belongs to this subsection is the record: a hidden button leaves none, so on the night an act is
stopped by an edit nobody admits to, a lock that was drawn rather than enforced has nothing in
the log, while a refusal is an `R` record carrying a tick, a sequence and an origin — which is
also where the operator learns whose hand it was.

**PRD §3.20 already keeps the selection out of the show file, and per client is a step further
than that.** §3.20 puts *"derived/ephemeral state (playhead, window geometry, selection, meters,
run state)"* in a separate file in the bundle — that file is `state.xml`, and §14.4 shows what
it will hold. But `state.xml` is one file per bundle and records what the *engine* was doing:
today exactly two values, `Lists/@focus` and `list/@standby`, with Phase 5's `Show/@locked` the
third. Two clients cannot both keep their selection there without one overwriting the other, so
a per-client selection is not merely out of the show — it is out of the bundle, out of the
engine, and lives in the tab that made it for as long as that tab is open and no longer.

**And that is constraint 9, which a browser tab satisfies more literally than any hardware ever
will.** *The controller arrives knowing nothing and leaves knowing nothing.* A tab arrives with
an empty map, one `GET /godot` and a WebSocket; it draws a complete console out of the reply;
and closing it leaves the engine byte-for-byte where it was. The one thing it remembers between
visits is the display preset in `localStorage`, which holds nothing about the show, which the
engine never reads and cannot read, and whose loss costs one menu choice. That is also what
makes the rule testable rather than pious: **kill any client at any moment of a show, and a
replacement drawn from `GET /godot` alone is indistinguishable from it**, but for a fold, a
scroll and a preference. Any state that fails that test has been put in the wrong place, and
§14.2 is the contract that keeps it out.

### 14.2 The web client's contract

**A client of this engine reads by polling and writes by datagram, and everything it needs to
draw is in the reply.** What follows is written so that a second client — the JUCE one of
§14.16, an external script, somebody's Max patch — can be built from this subsection and nothing
else. `clients/console/index.html` is its reference implementation rather than its
specification: where the page and this subsection disagree, the page is what 5.9 to 5.18 will
fix (§14.3).

**PRD §3.17 says the web client is TypeScript, and what decision V builds is not.** The
parenthesis is exact — *"The web client (TypeScript over OSCQuery + WebSocket) is what makes the
tablet a genuine fallback"* — and TypeScript is a build step, which decision V forbids for the
reason §14.3 gives. The difference is a language, and every word of the argument §3.17 makes
around it — no install, a namespace the client discovers rather than ships, no hand-maintained
parameter table on two sides — is kept in full by plain modules. So this is not a quarrel with
the PRD but an amendment to propose against it, and §14.15 carries it with the rest of the
phase's list rather than settling it here.

**Reading is one `GET /godot` at 10 Hz, flattened to a map of address to *node*.** `POLL_MS =
100` (`index.html:445`); the OSCQuery reply is a tree of `CONTENTS`, and `flatten` (`:457-467`)
keys every object carrying a `FULL_PATH` by that path. **The whole node, not its value** — that
is the load-bearing half. The inspector is built out of what each node says about itself: its
`TYPE`, its `ACCESS`, its `RANGE` and its `DESCRIPTION`, which is why a row added to
`docs/parameters/godot-parameters.csv` appears in the client without a line being written in it
(`index.html:451-456`). That is the self-description PRD §3.22 builds the whole device-template
argument out of — *"a device that speaks OSCQuery describes itself"* — turned on Go.dot's own
surface, and it is exactly the property WFS-DIY lacked: a parameter table hand-maintained in the
client, which had to be edited in step with the one in the engine and therefore was not. A
client that keeps a second copy of the table has reintroduced that bug; a client that reads
`RANGE` and `DESCRIPTION` off the node gets its bounds and its tooltips for nothing.

Polling rather than subscribing is a decision with a reason and a known cost. The server does
push coalesced OSC over the same socket to whoever sends `LISTEN`, and that is the right answer
for a surface somebody is *operating*; it is not the right answer for a view whose job is still
to be looked at and argued with, because a poll of the whole tree has no decode step to be
wrong, and ten a second is imperceptible against a fifty-hertz engine (`index.html:438-444`). It
is imperceptible to the tick thread too, and for a reason worth stating rather than assuming: a
poll is answered from the published snapshot, swapped whole under `publishMutex` and copied by
`snapshot()` under the same one (`tree/ParameterTree.cpp:1572-1594`), so an HTTP thread never
reads the model and a tenth client costs a `shared_ptr` copy. The measurement that could change
the polling rate is M24, and what M24 measures is the client's own `render()` rather than the
network (§14.14).

**Writing is binary OSC on the WebSocket that answers on the same port** (`index.html:618`), and
the engine's dispatch is three rules long (`oscquery/EngineNamespace.cpp:65-141`):

| the address | what the engine does | where |
|---|---|---|
| under `/godot/cmd/` | a **named command**: `standby.set` is `/godot/cmd/standby/set`, dots to slashes, and the packet's arguments are the command's | `:25`, `:81-85` |
| a trigger address, **over UDP only** | `trigger.fire` for every match, and a match ends it | `:86-122` |
| anything else | `node.set <address> <value>`, the address as the first argument | `:123-141` |

Two consequences a second client has to know. A WebSocket client **is a client**: it has the
whole command set and can send `cue.fire`, so triggers are not matched on that road at all — *"a
trigger fired from one would be a second road to the same place with no advantage and one more
thing to reason about"* (`:88-95`) — and a browser therefore cannot fire a §3.7 trigger by
writing its address. And an argument-less message is not a write of nothing: it has no value to
set, so the engine drops it rather than being asked to store an absence (`:133-137`).

**Values go as text, whatever the node's type is, and that is not a shortcut.** `node.set`
declares its value argument as `'*'` — whatever the target says — and turns whatever arrives
into canonical text before the schema parses it against the row the address resolves to
(`document/DocumentCommands.cpp:362-365`). Text is the road an integer takes anyway, one step
earlier, so a client that sends text never has to guess a type it could get wrong
(`index.html:656-664`). The corollary is a refusal worth meeting here rather than at 04:12, and
it is this subsection's to state once for every subsection that touches a boolean: the parser
takes exactly two words, `true` and `false` (`document/Schema.cpp:591-602`, *"a document is
written by this program and read by a person"*), so an OSC `T` or `F` works, the string `"true"`
works, and an integer `1` comes back `type-mismatch` — which matters most at
`/godot/document/locked`, where `type-mismatch` and `locked` would send an operator to two
different places.

**OSC has no reply channel, so `/godot/engine/lastError` is the only answer a refusal gets, and
a client must show it.** `Namespace::write` returns nothing by design — *"making this return a
status would invent a synchronous answer that the queue hop means the server cannot actually
have"* (`oscquery/OscQueryServer.h:91-98`); a rejection is an `R` record in the log and a
reading in the tree, whose exact spelling and whose origin field are §14.11's. The node is
refreshed in the after-tick only when the error count has moved, which is a mutex and a string
copy avoided fifty times a second (`Console.cpp:2206-2211`) — and because every non-applied
record bumps that count, two identical refusals in a row still both surface. The console states
the consequence better than a rule would: *"a client that does not show it is a client where
editing appears to do nothing at all"* (`index.html:133-136`).

**And a client must say when it has stopped hearing.** §3.17 asks it of the tablet — *"wifi loss
must leave the pane visibly stale, never silently frozen"* — and it is the same requirement here
for the same reason: a view that quietly freezes is worse than one that has gone, because
somebody will act on it. Two states of live, not one: the tree is read over HTTP and every edit
goes out over the socket, so a socket that is down is a page that reads perfectly and changes
nothing, which is worth saying before a note goes missing (`index.html:1491-1496`), and the two
ways a poll can fail are told apart in words rather than left to numbers that stopped moving
(`:1500-1512`).

The contract, then, as five sentences about a client rather than five instructions to one:

1. **A client holds nothing the engine owns** — §14.1's test, and the tree read out fresh every
   poll for everything that fails it.
2. **A client moves the standby only by asking for it.** The engine moves it in the four places
   §12.6 names — the legality check, the write door, the repair when the standby cue is deleted,
   the clear when it is moved away — and a revert or an undo can therefore leave the pointer
   somewhere new (§14.9, §14.10). What a client must never do is move it as a side effect of a
   gesture of its own: not on selection, not on scrolling, and not on a jump-to-cue from
   Choufleur (§3.23), which moves the *edit* pointer and says so. GO advances it; a standby
   command sets it; nothing a client draws touches it.
3. **Every gesture is a named command** (§4.11). A client is an accelerator over a complete
   command set and never the only route to anything: the same act must be reachable from a
   datagram, from `wfg` and from a script, which is why a client sends the command rather than
   reaching into the document. 5.18 will check that mechanically (§14.3).
4. **A client shows the refusal and shows the staleness**, both in words, and never in colour
   alone (§4.8).
5. **A client assumes nothing about the parameter table.** It declines to offer a write on a
   read-only node rather than sending one that will be rejected, and it renders a node it has
   never heard of as its type rather than as an error, because a row that arrives before the
   client knows about it is the normal case here and not a fault.

**What a client may cache is decided by one question: does the thing have a revision?** The
pyramid may be cached for ever. `/media/<hash>/timbre?level=N` is keyed by the content hash of
the file, so the answer cannot change without the key changing, and the route will say so with
`Cache-Control: max-age=31536000, immutable` (§14.5). Anything reached by an address under
`/godot` may not be cached at all: the document half of the tree is itself a cache the engine
invalidates with `markStale`, and a name held past one poll is yesterday's name. **The poll is
the invalidation** — and a client that improves on it by caching a subtree has taken over an
invalidation problem the engine already solved, and will get it wrong on the one edit that
mattered.

### 14.3 The console as the operator client

**Decision V stops the console being a diagnostic page and makes it the client the show is run
from.** PRD §3.17 is unambiguous about what that means and it is not a hedge: *"A web client is
the primary surface, and a native companion is an optional addition on top of it — not an
alternative to it"*, and the web client *"remains primary and must be complete on its own. It is
the redundancy path (§3.5), and a redundancy path that requires an installed app is not one."* A
page that can do nine of the ten things an operator needs is not a redundancy path; it is a
diagnostic with good manners. So Half B's whole job is the tenth thing, one view per pull
request, each sized to earn one round of the author's feedback rather than to be finished.

**The console is already a client and is not yet an operator's client, and the difference is a
list.** The console today draws standby and selection distinctly, fires GO on Space, steps the
standby with the arrows, runs a running pane with `run.kill`, italicises the header's derived
lines, carries the aim bar with its step chips and solve plan, renders the persistent band, and
builds its whole inspector generically out of `TYPE`, `ACCESS`, `RANGE` and `DESCRIPTION`
(§14.2). It sends fifteen named commands and the inspector's `node.set`. What it cannot do is
the operator half: no run pointer on a cue-list row, so a run is visible only in the running
pane; no `run.advance`, `run.prune`, `run.unprune` or `run.stop` though all four exist; no round
pills, no range name on the strip; no bulk edit; no header pane; no curve editor; no display
presets and no show mode; no save gesture at all — `document.save` is never sent — no undo
gestures, and no colour. Half of that list is waiting on Half A, which is the sequencing
argument in the preamble making itself felt.

**Two defects come before any new view, because each of them ends a rehearsal on its own.**
`renderLists` replaces a pane's `innerHTML` wholesale (`clients/console/index.html:900`, and the
running pane at `:987`) ten times a second, so a long list's scroll snaps to the top between one
poll and the next and nothing can ever hold the standby in view — the page is unusable at
exactly the size of a real show, and only at that size, which is why it has survived four
phases. And the aim `<input type=range>` (`:389`) is caught by the keydown guard's
`INPUT|SELECT|TEXTAREA` test (`:1699-1703`), which exists for the right reason — somebody typing
a cue name has every right to a space in it — so once the slider has been touched, Space no
longer fires GO and the arrows move the slider instead of the standby, silently, with the
transport gone deaf and nothing on screen saying so. 5.9 will key rows by cue id and runs by run
id and reconcile a `Map<id, element>` in order, updating attributes and text in place; it will
narrow the guard to text-like controls, blur the slider on `change`, and give `Escape` a
meaning; and it will build `triggersOf` (`:491-501`) once per poll as an index instead of
scanning every address per row per render. **M24** measures `render()` on a 500-cue bundle
before and after, in the PR (§14.14). A view drawn on top of a page that loses the operator's
scroll is a view nobody can judge, which is the whole reason these come first.

**Then the module split, and the no-build rule is load-bearing rather than frugal.** 5.10 will
cut the one file into `plumbing/{osc,link,poll,tree}.js`, `model/{index,selection,layout}.js`,
`views/{strip,transport,didi,gogo,header,inspector,aim,curve}.js`, `gestures/{keys,clicks}.js`
and `styles.css`, with `index.html` reduced to a shell carrying one `<script type="module">` —
plain ES modules, no bundler, no import map, no dependency. The engine already serves them:
`serveClient` handles subdirectories, `.js` is already `text/javascript`, and every file goes
out with `Cache-Control: no-store` and the reason beside it in the source — *"the page is being
edited while the engine is running and a stale copy after a refresh is a minute of somebody
wondering why their change did nothing"* (`oscquery/OscQueryServer.cpp:236-246`). That is the
loop the rule exists for: change a file, refresh the tab, look at it — during the show that is
already running, with the engine untouched and the runs still playing. PRD §3.17 makes the
layout the author's to design and he designs by looking, so the client that gets designed has to
be the one that can be edited between two GOs. A build step costs seconds and, worse, costs a
state of mind; it is the difference between trying a layout and deciding one. `/ui` is a
reserved prefix a mount may not claim (`tree/Mount.cpp:73-77`), and §14.5 reserves `/media`
beside it for the same reason.

The views will then land one per PR, and each is written to be argued with:

| PR | the view | what it has to earn |
|---|---|---|
| **5.11** | run pointers on Didi, Gogo's gestures | a live marker on the row of any cue with a run (`run/order` × `run.cue`) with its state word and position, so the cue list stops being the pane that cannot see the show; then *stop* (footer) and *advance*, round pills from `run/round` and `run/pruned` clicking to `run.prune`/`run.unprune`, the range index and `rangeIteration` on the strip line, and `warning`/`error` **in words** (§4.8) |
| **5.12** | group bulk edit | PRD §3.5's mixed-value semantics exactly — the intersection of field names over the selection, the value when every member agrees, *mixed* otherwise, and typing sets all as *N* `node.set` writes — plus §3.5's kind filter, *"all audio, all video, all OSC, all MIDI"*, so a mixed group exposes one kind's fields rather than the thin intersection of everything. No engine change: it is *N* ordinary writes, which is also what makes it one undo transaction's worth of thinking (§14.9) |
| **5.13** | the header pane | that a member says which of its values it owns and which the header wrote, in type rather than in colour (§4.8): written lines upright and `headerDerived` in italics (§13.7); double-click a derived line to select the member and scroll it into view, which is possible only once rows are keyed; *mark as preset* in the inspector as a `node.set …/preset <ancestorGroup>` over a select of ancestors, so the gesture that makes a preset is the command a script would send |
| **5.14** | display presets, and show mode | `design`, `tech` or `show` in `localStorage` (§14.1); the `#strip` telemetry and the raw addresses behind *tech* rather than deleted, which is what decision V promised the diagnostics; *show* reads `/godot/document/locked` and hides structure, keeping GO, standby and every run gesture — the client honouring the node, not replacing it (§14.11) |
| **5.15** | save, recovery and undo gestures | landed line by line as Half A lands each node, then consolidated here into `views/transport.js` and `gestures/keys.js`: Ctrl/Cmd-S, Ctrl/Cmd-Z, the transaction word beside the button, the recovery banner's *recover* and *discard*. The delete confirm goes away, because the page's own comment says why it is there — *"Undo is Phase 5's and does not exist yet"* (`index.html:1609-1611`) — and by then it does |
| **5.16b** | the curve editor | SVG breakpoints dragged, and a numeric list beside them for the operator who does not want to drag; commits one `node.set …/points`. It reads `values` and never `soleValue`, because a `d*` node's type string grows with its value (§14.6) |
| **5.17** | the coloured bar on Gogo | one `GET /media/<hash>/timbre?level=k` per hash at the bar's width, cached in the page by hash (§14.2), a canvas coloured per frame with the playhead from `run/position` — and the three numbers stay beside it in words, because colour is never the sole carrier (§4.8, §14.12) |

**What the client is not allowed to become is as much of the design as what it draws.** It holds
no state the engine owns: §14.1's test applies to every one of these views, and the ones that
tempt hardest — which run is selected in Gogo, which fields a bulk edit is showing, where a
breakpoint is mid-drag — are all client state precisely because losing them costs a moment's
re-aiming. Every gesture is a named command (constraint 11), which means no view may reach into
the document by building an address a command would have built for it. And the two clients must
not drift on the names: the gesture-to-command table is **data**,
`clients/console/gestures/commands.json`, read by the page and — §14.16 — by the JUCE client
rather than copied into it. **That file does not exist today**; `clients/console/` contains
exactly one file. 5.10 will land it with the split, 5.11 will be where it starts carrying
entries a view depends on, and 5.18 will turn it into a check.

**How the page is tested, said honestly, because the answer is *partly*.**
`tests/blackbox/client_page.py` exists and is registered per locale
(`tests/CMakeLists.txt:694-704`); it drives `GET /ui`, `GET /ui/index.html`, a missing
`/ui/nothing.js`, three path-traversal attempts and the `--ui` notice, and then it drives the
**engine** through seven gestures transcribed into Python by hand. So what is tested is the
server that serves the page and the commands the page is believed to send — not the page. The
page and that transcription can drift apart in silence, and 5.18 will close the half of that gap
which is closable cheaply: `gestures/commands.json` checked from stdlib Python against `wfg
commands` for names and arity, so a gesture naming a command that does not exist fails a test
rather than an evening; and `node --test` over the pure modules — the OSC encoder against the
byte fixtures `tests/OscCodecTests.cpp` already hand-wrote, the keyed reconciler, the bulk
edit's `intersect`/`agree`, `pointsToText` — registered as ctest `console.unit` only where
`find_program (WFG_NODE node)` succeeds and printing a STATUS line where it does not. Browser
automation is refused for the reason §14.15 gives. What that leaves genuinely untested is the
DOM — that the marker is on the right row, that the pill click hits the right run — and §14.15
records it as a gap rather than pretending the two cheaper checks close it.

### 14.4 `/godot/document` grows, and one of its rows is written by a client

**The container is already split down the middle, and the split is the `persist` column rather
than the rhythm of the value.** The document half loops over `rowsForOwner ("document")`, skips
`persist == none` and reads each value off `document.root()` (`tree/ParameterTree.cpp:782-790`),
its comment saying what it leaves behind: *"the show format's own version. The rest of that
container is runtime state and lives on the other side"* (`:780-781`). The runtime half
publishes the `persist == none` rows out of `EngineState`, through a hand-written chain of `else
if`s over the row's name (`:1252-1274`). The Phase 5 handoff's rule — *"anything that changes
while the show does not is in the runtime half"*
(`docs/handoffs/2026-09-09-phase5-handoff.md:26-30`) — governs the population of that second
half, which is the rows that persist nowhere. A row that persists is an attribute of an element
and is published from the document half whatever its rhythm, and `locked` is the case that makes
the distinction worth stating: show mode changes several times a night while the show itself
does not, and it is still a document-half row. That is safe only because `markStale` runs on
every applied command (`Console.cpp:2244-2245`), so a lock written this tick is published this
tick.

| Node | Type | Access | Persist | Cap | What it says | Half | PR |
|---|---|---|---|---|---|---|---|
| `/godot/document/dirty` | `T` | ro | none | 5 | whether the bytes on disk are behind this document's history. The row has existed since Phase 1 and nothing but one test line has ever assigned it, so 5.2 will add no node — it will make an existing promise true, and a reviewer should expect no CSV change beyond the description (§14.10) | runtime | 5.2 |
| `/godot/document/locked` | `T` | **rw** | **state** | 5 | show mode (decision W) — a lock on the show, not a mode in a client (§14.11). The only row in this container a client writes | document | 5.3 |
| `/godot/document/canUndo` | `T` | ro | none | 5 | whether the `document` domain has a transaction to unmake (§14.9) | runtime | 5.4 |
| `/godot/document/canRedo` | `T` | ro | none | 5 | whether it has one to put back | runtime | 5.4 |
| `/godot/document/undoName` | `s` | ro | none | 5 | `getUndoDescription()` — the name of the transaction `undo` would unmake (§14.9), so a menu item reads *Undo cue.create* rather than *Undo* | runtime | 5.4 |
| `/godot/document/redoName` | `s` | ro | none | 5 | the same for `redo`; empty when nothing has been undone | runtime | 5.4 |
| `/godot/document/recovery` | `T` | ro | none | **1** | whether a `recovery/show.xml` was found beside this bundle when it opened, and has not been recovered or discarded since (§14.10) | runtime | 5.5 |

Five of the six new rows take the cap the container's other mutable rows already carry — `dirty`
and `warnings` are both 5 (`docs/parameters/godot-parameters.csv:21-22`) — and `recovery` takes
1, with `path`, `name` and `formatVersion` (`csv:18-20`), because it changes at most twice in a
session: once when the bundle opens and once when the operator answers it. A cap is a
declaration to the client and never a throttle in the engine (§14.5).

**The address reaches the `Show` root today, and nothing has to be invented to make it.**
`ShowDocument::resolve` branches on the number of address segments rather than on the owner
word: three parts after `godot` is a **container address** and four is an object address. Its
header says so — *"`/godot/<owner>/<id>/<attribute>`, or `/godot/document/<attribute>` for the
root. Owner words are the parameter table's: document, list, cue, mount"*
(`ShowDocument.h:231-236`) — and the three-part case reaches `containerElementFor (owner)` at
`ShowDocument.cpp:396`, over the map `containerSegmentFor` keeps at `:187-205`: `Show` to
`document`, `Audio` to `audio`, `Lists` to `list`, *"the elements that are addressed without an
identifier, because there is only one of each"*. The precedent is not hypothetical and it is not
the lock: `document/formatVersion` is already a `document` row that is an attribute of the root,
carried as the `{ "document", "formatVersion" }` entry in the generated schema table
(`SchemaTable.generated.h:208`) and read through `instance().attribute (rootElement,
"formatVersion")` (`Schema.cpp:363-366`); `tests/DocumentTests.cpp:522` already asserts that
`/godot/document/formatVersion` resolves. The single reason `node.set
/godot/document/formatVersion` fails today is the read-only refusal at
`ShowDocument.cpp:462-463` — `if (target.isDerived || target.attribute->access() ==
Access::read)`. Change one column to `rw` and the write lands, through the same choke point
every other written attribute takes (`:452`, `:506-513`). What is genuinely new is smaller than
the plan allowed for and worth naming for what it is: the first `access=rw` row owner `document`
has ever carried, and the first `persist=state` attribute on the root. `lists,focus` is its
nearest relative — a writable `persist=state` attribute on a container addressed without an
identifier, `/godot/list/focus` (`csv:24`), resolved through the same three-part branch and
round-tripped through `state.xml` under test at `tests/CueListTests.cpp:869-908`.

***This paragraph is a correction*** *(PR 5.0, traced 2026-09-10)*. The approved plan books
`EphemeralState` a new entry form for the root — *"`EphemeralState` gains one `<Show
locked="true"/>` root entry"* — on the belief that `collect` visits identified elements only. It
visits containers too, and `Show` is one by that same test: `const auto isContainer = !
ShowDocument::containerSegmentFor (elementName).empty();`, with the element taken when it is
`identified || isContainer` (`EphemeralState.cpp:81-85`, the comment at `:72-80` saying exactly
this case out loud). `containerSegmentFor ("Show")` answers `"document"`, and `collect` is
called on `document.root()`, which is the `Show` node (`EphemeralState.cpp:134`). The read half
is symmetric: a container entry resolves to `/godot/document/<attr>` at
`EphemeralState.cpp:187-204` and is restored through `document.setAttribute` at `:243-263`. So
the lock persists with **no new plumbing at all** — one less piece than PR 5.3 budgeted, and the
correction is subtractive, which is the direction a plan is happiest to be wrong in. §14.13
books what a row does cost, and §14.11 argues the lock itself.

**The persistence rule splits the seven cleanly, and it splits them on §4.10 rather than on
convenience.** The lock persists because show mode is something somebody decided: the operator
locked the show at 19:20 and a restart at 19:45 must come back locked, or the first thing a
rebooted engine does is un-protect a running performance (plan decision 6).
`SchemaTypes.h:63-73` makes §4.10 mechanical — `show` is what someone decided, `state` is where
the machine happened to be — and a lock is the third member of a set that already has a rule:
`list/@standby` and `Lists/@focus` are also decisions of the operator's, and also live in
`state.xml`, because they are the operator's *position* and not the show's content. The undo
four do not persist, because they are readings of a stack that begins empty at every open and a
restored `undoName` would name a transaction no `UndoManager` holds. `recovery` does not persist
because it is a fact about what was found on disk this morning, and `dirty` does not because it
is a comparison against a number that exists only while the process does. Constraint 10 decides
all six the same way, and §14.13 books what each of them costs a PR author in C++.

### 14.5 `/godot/run/<id>/timbre`, `/godot/cue/<id>/hash`, and a route that is not an address

**PRD §3.30 asks for two different things and they are not the same kind of thing at all.** A
run publishes *"its timbre — hue and saturation at its current position — as a read-only node
beside `/godot/run/<id>/position`, updated on the tick thread by a table lookup (§3.4: control
rate, two values per running clip)"*. That is a parameter: numbers that are true at this instant
and false at the next. The pyramid the editor and the Gogo bar read is not: it is kilobytes of
binary per file, it does not change while the file does not, and it has no value *at a moment*.
Two shapes, two carriers. **The tree carries what is true now; a route carries what is true
always.**

| Node | Type | Access | Persist | Cap | What it says | Half | PR |
|---|---|---|---|---|---|---|---|
| `/godot/run/<id>/timbre` | `s` | ro | none | 10 | `"<hue 0..360> <sat 0..1> <light 0..1>"` at the run's current position; empty while the pyramid has not arrived, which §3.30 calls grey | runtime | 5.8 |
| `/godot/cue/<id>/hash` | `s` | ro | none | 1 | the sha256 of this media cue's file, once the analyser has hashed it — the key the route below is addressed by. Empty for every kind but media, and until the hash exists | runtime | 5.8 |

**The node carries three numbers where §3.30 says two, deliberately, and §14.15 carries the
amendment rather than this row carrying the departure in silence.** Lightness is the frequency
axis, and it is what makes constraint 8 hold inside a single channel (§14.12); a client asked to
recover it by inverting the ramp is a client keeping a second copy of the engine's table, which
is the one thing §14.2 tells a client never to do. Publishing it costs one field of a string
that is already being formatted, and plan decision 8 draws the value as `"h s l"` throughout.

**The hash is published at `/godot/cue/<id>/hash`, and there is no `/godot/media` address
space.** *This paragraph is a correction* *(PR 5.0, 2026-09-09)*: the plan drew
`/godot/media/<id>/hash`, and that address cannot be created by adding a `media` row.
`collectCue` builds `base = "/godot/cue/" + id` (`ParameterTree.cpp:557`) and appends the
`media` owner's rows to the cue's own for a `Media` element (`:573-576`), so every `media` row
already publishes under the cue — the table says so in its own voice, `media,duration` being
*"On owner `media` and not `cue`, or every memo, group and fade would grow one"* (`csv:50`),
which clients read as `/godot/cue/<id>/duration`. A hash arrives *after* the analyser has run
and changes while the show does not, so it belongs in the runtime half; but a `media` row
emitted from the runtime half would **duplicate** the address the document half already emits,
and `ParameterTree.cpp:1535-1555` records that hole being closed once already — *a duplicate
would make the answer depend on which it reached first*. The only pattern that works is
`cue/prepare`, and it is a **pair** of changes rather than one: the document half skips it by
name (`if (name == "prepare") continue;`, `:605-606`, argued at `:599-604` — *"this half is a
cache - published from here it would freeze at whatever it was when a cue was last edited"*) and
the runtime half emits it over `declaredCues` (`:1423-1442`). 5.8 will draw `hash` as that pair,
and §14.13 books both halves.

**`timbre` is a table lookup and `position` is the debt underneath it.** `Run::position` is
declared (`cue/Run.h:264`), published (`ParameterTree.cpp:1510`) and assigned nowhere in `src/`,
so the number every client reads today is the literal `0` — which is why the console's playhead
is gated on `position > 0` and draws nothing (`clients/console/index.html:920-921`). 5.1 will
pay it, and where it is paid matters, because §3.30's *"beside `position`"* is what the timbre
row inherits: **position needs its own pass over `runs.all()`, not a line inside
`advanceRanges`.** That loop `continue`s on `run->range < 0` (`Runner.cpp:4592-4593`), and
`Run::range` is `-1` for *"every kind but media and a media cue with no ranges"*
(`Run.h:322-323`, the declaration at `:329`) — which is the ordinary media cue, and the only
case §3.30's bar is drawn for. Two guards go with it: `launchedAtSample` is 0 until the launch
is placed (`Run.h:608`), so an armed-not-launched run would otherwise publish the whole
session's elapsed seconds; and `RangeSpec::in` is not on the run at all, being re-read from the
document per boundary through `Runner::rangesOf` (`Runner.cpp:168-205`).

The lookup needs the file the run is playing, so the run holds its own copy: **`Run::media`,
placed above `armMedia`'s `if (audio == nullptr) return;`** (`Runner.cpp:1945-1946`), beside
`claimSlotsFor` at `:1934` and for the reason its comment gives at `:1928-1933` — a media path
is a fact about the document, not about the audio. The plan said *"copied at arm, as `kind`
is"*, and `kind` is not copied at arm: it is set in `RunTable::create` (`Run.cpp:23-29`). Copied
below that early return, the path would be empty on every replay and on every run whose track
was already reserved, and the copy's whole purpose — that undoing a delete leaves a run playing
and still able to say what it is playing (§14.9) — would be lost in exactly the case it was
written for. `Run::cue` and `Run::kind` are strings today (`Run.h:252-253`); 5.6 will add
`media` beside them.

**`rate_cap 10` is what the node declares, not what the engine does.** The value is copied from
the schema row into the published `Node` (`ParameterTree.cpp:202`) and emitted as OSCQuery's
`"RATE_CAP"` (`tree/OscQueryJson.cpp:171`); the only code in the engine that *acts* on a rate
cap is `MountSender::intervalFor` (`MountSender.cpp:74-79`, applied at `:98`), which throttles
outbound writes to mounted targets. Nothing throttles a `/godot` node. So `timbre` will be
recomputed every tick like everything else, and the 10 is an instruction to the surface that
draws it — §3.30's *"no faster than about ten times a second"*, which is the rate the web client
polls at anyway (§14.2). Two things follow rather than being discovered: M22's cost is measured
against fifty lookups a second, not ten; and `run,timbre` at 10 will be the first row in the
whole table with a cap that is not 1, 5 or 50 — `run,position` and `run,rangeIteration` are both
50 (`csv:98`, `:102`) — so it is a deliberate new class and not a typo.

**The pyramid is a file, and the route that serves it is not an address.** A level in the tree
would put kilobytes of base64 into every `GET /godot` a client makes at 10 Hz, for ever, for
data that has not changed since the file was imported: the description would grow by the size of
the show's media and the poll would stop being affordable. So 5.8 will add a plain `GET` on the
same port, content-addressed:

| Request | Answers | Headers | Refuses |
|---|---|---|---|
| `GET /media/<hash>/timbre?level=N` | that level's frames, exactly the bytes the `.tpy` holds | `application/octet-stream`; `Cache-Control: max-age=31536000, immutable` | 400 for a hash that is not exactly 64 hex characters, or a level that is not a number; 404 for a hash the snapshot does not hold, or a level the pyramid does not have |
| `GET /media/<hash>/timbre?INFO` | the header as JSON: `sha256`, `seconds`, `sampleRate`, `window`, `hop`, and a `levels` array of `{ frames, bytes }` | `application/json`; the same immutable header | as above, minus the level |

`?INFO` exists so that a forty-pixel Gogo bar and a full-width editor waveform each ask for the
level they want in one round trip rather than fetching the finest and throwing most of it away,
which is the whole reason §3.30 asked for a pyramid instead of a frame array.

**`immutable` is the exact opposite of what `/ui` gets, and both are right.** The client
directory answers `Cache-Control: no-store` (`OscQueryServer.cpp:246`) because the page is
edited while the engine runs and a stale module is a bug the author cannot see (decision V,
§14.3). A pyramid is named by the sha256 of its own source, so a given URL can never answer
differently; a year is not optimism about the cache, it is a statement about content addressing.
The same property is what makes the route safe: **the hash is validated as sixty-four hex
characters and looked up in the snapshot, so no request text ever becomes a filesystem path.**
`/ui` needs `file.isAChildOf (clientDirectory)` (`:232-238`) precisely because a request there
*is* a path; this route needs no such check because nothing it receives is one.

Three mechanical facts about where the branch goes, all of which the existing file settles:

- **Beside `/ui`, which is answered first** (`OscQueryServer.cpp:140-142`), above `?HOST_INFO`
  (`:144`), above the snapshot fetch (`:157`) and above the wildcard refusal (`:173-179`) —
  whose exemption it inherits, correctly, since a 64-hex hash carries no wildcard character.
  Below the `target == nullptr` guard at `:118-119`, which returns `false` and falls through to
  juce_simpleweb's own static handler: a `/media` branch above it would serve pyramids from a
  server with no namespace.
- **The route parses its own query.** Simple-Web-Server splits path from query when it parses
  the request line and `request->query_string` is everything after the `?` (`:33-47`, which
  records an earlier version searching `path` for a `?` and silently answering the wrong
  question). The server's only query inspection is `isBareKey`, which **refuses** anything
  containing `=` or `&` because *an OSCQuery attribute query is a BARE key* (`:49-57`). So
  `?level=2` will be the first `=`-bearing query this server has ever accepted, and it is
  accepted only on the one route that is not an address.
- **It answers from memory, never from disk on the request thread.** There is exactly one HTTP
  thread and it is shared with the WebSocket — `SimpleWebSocketServerBase` is a single
  `juce::Thread` named *"Web socket"* whose `run()` ends in `ioService->run()`
  (`ThirdParty/juce_simpleweb/SimpleWebSocketServer.cpp:16`, `:256-259`), the
  `config.thread_pool_size = 4` at `:243` being dead configuration since the server assigns its
  own io_service at `:219` — so a response that blocked on a disk read or on the analyser's
  mutex would stall the OSCQuery poll and every subscription push for its duration: on the night
  the disk is busy, a browser asking for a colour bar would silence the tablet's whole tree. The
  branch therefore copies a `shared_ptr` out of the snapshot under a mutex the analyser never
  holds while working (§14.12). `response->write` takes a `string_view`
  (`webserver/server_http.hpp:155`), so arbitrary bytes in a `std::string` are safe.

5.1 will reserve `/media` against mounts exactly as `/ui` is reserved (§14.13). And one
deliberate consequence: none of it will exist under `wfg replay`, which is correct. A replay has
no files to hash and no HTTP server, so `timbre` is empty and the bar is grey, which is the same
answer §3.30 gives for a clip whose cache has not arrived. §14.8 says why the analyser will
write no record a replay would have to reproduce.

### 14.6 The document grows two attributes

Two, and they are as far apart as two attributes can be: one is a curve somebody drew and lives
in `show.xml`, the other is a switch somebody threw and lives in `state.xml`. Constraint 10 puts
each where it goes, and the same rule sends them to different files.

| Element | Attribute | Type, default | `validate()` refuses | What the generator needs | PR |
|---|---|---|---|---|---|
| `Fade` | `points` | `d*`, empty | an odd count; a `t` outside `0..1`; a `t` that does not strictly ascend; a first `t` that is not 0 or a last that is not 1; a level outside the fade's own `-120..12` | nothing new — `d*` is already emitted for a `rw` row twice | 5.16a |
| `Show` | `locked` | `T`, `false` | nothing — a boolean carries two legal words and the parser takes exactly those (§14.2) | nothing — `document` is already in `KNOWN_OWNERS` | 5.3 |

**The curve was promised in the table three phases ago, in the table's own words.**
`fade,curve`'s description ends *"Two shapes until the curve editor of Phase 5"* (`csv:73`), and
`linear|sCurve` is what a fade has had since Phase 2. `points` is that sentence coming due: `(t,
level)` pairs, `t` a fraction of the fade's duration ascending from 0 to 1, level in absolute
dB, empty meaning `curve` still applies (plan decision 9, open to being overruled early rather
than late). The empty default is what keeps every existing show working without a migration and
keeps `curve` meaningful rather than vestigial — a fade nobody has opened the editor on is still
a linear fade, described by one word instead of four numbers.

**A curve is document content because a curve is a decision.** §4.10 divides on who chose the
value, not on how the value looks: `media/level` is *"the level the cue plays at, as decided"*
and `run/level` is what a fade is doing to it right now (`csv:48`, `:99`), and the same cut puts
a breakpoint list in `show.xml` and the fade's instantaneous output on the run. Somebody sat
down and drew that shape; it is the thing that gets diffed, reviewed at a production meeting and
carried to the next venue. The alternative — a shape derived from a preset name — is what
`curve` already is, and the reason for `points` is precisely that two shapes are not enough for
somebody who has a particular one in mind.

**What a fade with points does when it runs is one function's worth of change, and this is the
sentence that says so.** A fade's level now is `FadeJob::currentDb()` (`cue/FadeJob.h:137-145`),
which turns `ticksDone / ticksTotal` into a level through the free `fadeLevelDb`
(`cue/FadeJob.cpp:33`), and the only caller on the tick side is `Runner::advanceFades`
(`Runner.cpp:2948`, reading `job.currentDb()` at `:3056`). 5.16a will hand `fadeLevelDb` the
breakpoints and have it interpolate **piecewise-linearly in dB** between the two that bracket
`t` — in dB because that is the domain `fade,curve` already claims for `linear`, *"Linear in the
dB domain, which is what a fader feels like"* (`csv:73`), and a curve editor whose straight line
between two points meant something other than the word `linear` would be two definitions of one
shape. Where points exist, `curve` is **ignored rather than combined**: two shapes multiplied
together are a third shape nobody drew, and the operator who placed four points asked for those
four points. The replay fixture `fade-curve.wfglog` pins it, as `undo.wfglog` pins §14.9.

**But no list-typed attribute is writable over OSC today, and 5.16a is where that is discovered
unless this section says it first.** `ShowDocument::setAttribute` has no `isList()` branch: it
calls `Schema::parseValue (*target.attribute, text, value)` on the whole text
(`ShowDocument.cpp:466`), and `parseValue` for a `d*` row parses one double and rejects `"0 -60
1 0"` with *expected a number* (`Schema.cpp:575-590`). `CanonicalXml` has the branch
(`:330-345`), `RelaxNg` has it (`:106`) and `ParameterTree` has it (`:140`) — the **write door
does not**, and read-back is broken the same way, since `toText` for a `number` attribute calls
`osc::formatDouble` on a `String` var (`ShowDocument.cpp:141-142`). The two `d*` rows that exist
are proof by absence rather than precedent: `route,gains` (`csv:52`) has been unreachable
through `node.set` since Phase 2 and `feed,gains` (`csv:157`) since it arrived in Phase 4
(§13.3), both being read only by the canonical reader and the tree, both `persist=show`, and
neither ever written by a client. So 5.16a is a change to the write choke point and to `toText`,
in the same function 5.4 will rebuild around an `UndoManager`, which is why the plan orders it
after 5.4 and why this section says so rather than letting the ordering look accidental. One
consequence the curve editor sits on: a list node's OSCQuery type string grows with its value —
`ddd` for three gains — so `Node::soleValue()` returns nothing for a list and every client must
read `values` (`tree/Node.h:112-125`).

**`Show/@locked` is a CSV line and no plumbing**, for the mechanism §14.4 settles and with the
refusals already in place: the reader and the writer both test `persist() != Persist::show` and
refuse a `state` attribute in `show.xml`, and the message the generic test produces for this row
names it — *"`locked` is engine state; it belongs in state.xml, not in show.xml"*
(`CanonicalXml.cpp:317-322`, the writer skipping it at `:172-182`) — so the split is enforced in
both directions with no Phase 5 code at all. What 5.3 owes that nobody has written yet is a
fixture, which §14.13 books.

**The stop cue's fade half gets no breakpoints, and the reason is not tidiness.** `Stop` carries
`verb`, `duration` and `curve` (`csv:75-77`), and its `curve` keeps the two words it has. A stop
is how a sound is *got out of* — hard, or a fade to silence, or one of the three graceful
boundaries — and its shape is a property of the exit, not a drawing somebody is working on. Give
it a breakpoint list and it becomes a fade cue wearing a stop's name, with two elements that
both accept a `points` attribute, both parse it, both interpolate it, and both have to keep
meaning the same thing for ever. That is the duplication §13.12's shared `slot` owner exists to
prevent, taken from the opposite direction: there, one owner word served two elements so neither
could drift; here, the second element does not get the attribute at all. An operator who wants a
shaped exit writes a fade with the shape and a stop after it, which is two cues that say what
they are.

### 14.7 Operator commands

Six new commands and one gesture that is not a command. Every one of them is reachable from the
console, which is §4.11's requirement rather than a convenience: *every gesture-reachable action
exists as a named command*, so Ctrl-Z is a datagram carrying `undo` and the lock toggle is a
datagram carrying `node.set`. For scale: `registerDocumentCommands` registers exactly fifteen
document commands today (`DocumentCommands.cpp:88, 102, 122, 140, 163, 181, 196, 214, 232, 249,
267, 281, 301, 311, 345`), and `document.save` is not among them — it is registered by
`registerBundleCommands` in another file (`Bundle.cpp:290`), and that split is what lets saving
survive the lock.

| Command | Args | What it decides | Refuses with | Undoable | Moves the standby as a side effect |
|---|---|---|---|---|---|
| `undo` | `[s domain]` | which transaction comes off the named domain's stack; `document` when the argument is absent | `nothing-to-undo`; `locked`; `bad-value` for a domain word the enum does not carry | no — it is the mechanism | **no** |
| `redo` | `[s domain]` | which transaction goes back on | `nothing-to-redo`; `locked`; `bad-value` | no | **no** |
| `document.revert` | — | that the bundle on disk wins: `Bundle::open` into the same object through `adopt`, history cleared, `markStale` | `locked`; `bad-address` when the folder the session names is no longer a readable bundle | no — and it clears the history, so nothing before it is either | **no**. The pointer does change, to whatever `state.xml` says, because loading a document is the whole of what this command does rather than a side effect of something else |
| `document.recover` | — | that `recovery/show.xml` wins: the same `adopt`, history cleared, and the document left **dirty** on purpose (§14.10) | `no-recovery` when there is nothing there; `locked` | no, as above | **no**, as above |
| `document.discardRecovery` | — | that the recovery folder is stale and goes | `no-recovery`; `write-failed` if the folder will not delete | not a document change at all | **no** |
| `document.saveAs` | `s` path | that these bytes are also written somewhere else; the session keeps pointing at the folder it opened (plan decision 7, here to be overruled early rather than late). §14.10 says why the copy is save-plus-a-copy rather than one act | `write-failed` | not a document change | **no** |
| *the lock* — `node.set` | `s /godot/document/locked`, `T` | show mode on or off (decision W) | `type-mismatch` for anything but `T`/`F`/`"true"`/`"false"` (§14.2); never `locked`, since the row is `persist=state` | no — plan decision 3, here to be overruled early rather than late, keeps state rows off the stack | **no** |

**The lock is not a command, and that is the design rather than an omission.**
`/godot/document/locked true` arriving as a datagram becomes `node.set` in
`oscquery/EngineNamespace.cpp:123-141` with no help from anybody, so a `document.lock` command
would be a second door onto one attribute, and the second door is always the one that forgets a
rule. The lock is therefore written by the mechanism every other `rw` node is written by, which
is the one thing §14.2 asks a client author to believe about writing.

**`undo` and `redo` are registered by `registerDocumentCommands`, and this subsection owns the
rule about what `wfg replay` will and will not register.** Replay has two gates, not one.
`registerDocumentCommands` at `Console.cpp:534` sits inside the `if (bundlePath.isNotEmpty())`
block opened at `:497` and closed at `:570`, so a replay with no `--bundle` registers no
document commands at all. `registerBundleCommands` sits deeper still, behind `--out` as well
(`:550-565`), for the reason its comment gives — *"Absent, `document.save` is not registered at
all and replays as a rejection, which is loud, and better than a replay that wrote over the show
it was checking"*. So `undo` and `redo` registered in the first file replay wherever a bundle
was supplied; registered beside `document.save` they would replay as `unknown-command` on every
log opened without `--out`, produce an `R` line and exit 1 (`log/Replay.cpp:94-108`). Neither
file is unconditional, and the honest form of the rule is that **a log carrying an `undo` needs
`--bundle`, and a log carrying a `document.autosave` needs `--out` as well** — which is why both
flags belong in the driver's replay step by name (§14.8, §14.10).

**The five new reason codes**, spelled as the log spells them, in `namespace reason` in
`command/Command.h`, whose own rule is at `:107-109`: *"Reason codes are part of the log format
and therefore a contract; keep them here, in one place, spelled exactly as the log spells
them."*

| Identifier | Text | Said when | PR |
|---|---|---|---|
| `writeFailed` | `write-failed` | bytes did not reach the disk: a full volume, a folder that went away, a replace the platform refused. It replaces the `reason::badAddress` `document.save` uses today (`Bundle.cpp:307`), which is the wrong word for a full disk | 5.1 |
| `locked` | `locked` | show mode is on and the command would have changed the show half of the document | 5.3 |
| `nothingToUndo` | `nothing-to-undo` | the domain's stack is empty | 5.4 |
| `nothingToRedo` | `nothing-to-redo` | nothing has been undone, or an edit since has cleared the redo half | 5.4 |
| `noRecovery` | `no-recovery` | `document.recover` or `document.discardRecovery` on a bundle with no `recovery/` | 5.5 |

`write-failed` will land in **5.1**, with the debt it fixes, rather than in 5.2 with the atomic
write that becomes its second writer (§14.10): 5.1 is where `document.save`'s wrong word is
corrected, and a reason code that waits one PR for its second writer is a smaller oddity than a
PR that fixes a word without being allowed to name it. Two facts about the list itself. It goes
in the **first** of the engine's three reason vocabularies: `wfg::reason` is the log's
(`Command.h:109-163`), `wfg::osc::refusal` is a dropped packet's (`osc/OscCodec.h:138-150`) and
`wfg::cue::runError` is a readout on the run and never a log reason (`cue/Run.h:184-242`). And
the first already carries two codes nothing writes — no `reason::retiredId` or
`reason::malformedPacket` appears anywhere in `src/`, the drop path writing `osc::refusal::*`
instead — so Phase 5 adds five clauses to a contract that has two dead ones, and a contract with
dead clauses invites a sixth.

**Decision W, as a list, because a reader will look for it here.** §14.11 argues the predicate,
names the doors and gives the refusal its text; this table is the command-by-command answer, and
it is the one a driver author copies.

| Under lock | Commands | Why |
|---|---|---|
| **refuses** `locked` | the ten creates — `list.create`, `cue.create`, `route.create`, `range.create`, `slot.create`, `channel.create`, `feed.create`, `insert.create`, `trigger.create`, `mount.create` — plus `object.delete` and `object.move` | they reach `insertObject`, `remove` or `move`, three of the document's four doors |
| **refuses** `locked` | `node.set` on a `/godot` address whose row is `persist == show` | the fourth door, refused beside the read-only check at `ShowDocument.cpp:462-463` and above the parse at `:466`, so a locked show answers `locked` and not `type-mismatch` |
| **refuses** `locked` | `undo`, `redo`, `document.revert`, `document.recover` | they knock at no door — undo writes through JUCE's own actions, and `revert` and `recover` through `adopt`, which replaces the root and the lock with it (§14.11) |
| **refuses only when it would insert** | `group.role`, `list.persistent` | idempotent: on a group that already has a footer they return the existing child before any door and are logged **applied** (`ShowDocument.cpp:808-809`, `:830-831`); on one that has none they reach `insertObject` and refuse |
| **keeps working** | `go`, `standby.set`, `.clear`, `.next`, `.previous` (`cue/CueCommands.cpp:54, 128, 144, 167`), `list.focus`, `list.aim`, `list.loadToTime`, and every `run.*` | they write `list/@standby` and `Lists/@focus`, which are `persist == state` rows, or they write nothing in the document at all |
| **keeps working** | `node.set` on a mounted address | it never reaches the document: `DocumentCommands.cpp:359-360` returns before `setAttribute`. PRD §3.17 has an operator adjusting levels from the house during a show, and this is the line that lets them |
| **keeps working** | `mount.load`, `mount.readback`, `node.touch`, `node.release`, `node.releaseAll` | not because they were exempted but because none of them knocks: `mount.load` takes the document as `const doc::ShowDocument&` (`tree/TreeCommands.cpp:118`) and the rest write only the mount and touch tables |
| **keeps working** | `document.save`, `document.autosave`, `document.saveAs`, `document.discardRecovery` | they write bytes, not the document. Saving during a locked show is the point of locking it |

One consequence the table cannot hold, and it is the one a driver gets wrong: **a command that
changes nothing is not refused for changing nothing**, so a driver asserting *every edit command
refuses under lock* will find `group.role` and `list.persistent` and be red. The sentence it
should assert instead is *every edit that would change the document refuses*. `channel.create`
is the other trap — it mutates before it reaches its door — and §14.11 places the check that
catches it.

**One column in that table is a rule the driver already enforces.** §3.5: *only GO moves the
standby* — a pointer the scheduler also moved would be two things moving one pointer, which is
what `reason::notManualPath`'s comment says at `Command.h:117-127`. The Phase 4 black-box driver
reads a second list's standby before a command and asserts it unchanged after
(`tests/blackbox/phase4_prepare.py:673-678`), and `phase5_document.py` will inherit the check
verbatim. It is a rule about side effects rather than about a pointer that cannot move: §12.6's
four engine-side movers stand, and `document.revert` loads a pointer along with the document it
is in (§14.10).

### 14.8 What the engine reports to itself, and the two things that report nothing

One engine-origin command, registered as §11.4's, §12.3's and §13.4's were, handler
replay-idempotent, origin `engine`:

| Command | Args | When |
|---|---|---|
| `document.autosave` | — | the document is dirty and has been quiet for two seconds (`tick - lastChangeTick >= 100`), or thirty seconds have passed since the last autosave while it stayed dirty (`tick - lastAutosaveTick >= 1500`) — the arithmetic is §14.10's, and it is plan decision 5, taken early so it can be overruled early. The handler writes `recovery/show.xml` and `recovery/state.xml` atomically, and never the authored `show.xml` |

**One decision, one record, and it is the hook that takes it.** Phase 3's rule holds here
exactly as it held for the horizon: the hook decides, the handler applies. The arithmetic over
`DocumentSession` is a decision — *now, rather than in a second's time* — taken by something
watching the clock, and a replay runs no hooks, so a replay that re-derived it would be a second
implementation of one judgement, drifting from the first the moment either changed. Instead the
record carries the tick and the replay re-injects it, which is what makes a replayed session
write the same files at the same ticks as the session it replays. Adding a decision to a handler
is how §12.5's third finding happened, where a round had to be filtered in the hook *and* in the
handler because either alone was wrong.

**It goes in the BEFORE hook, and *this paragraph is a correction*** *(PR 5.0, 2026-09-09)*. The
approved plan puts the autosave decision in the after-tick beside `dirty`, and
`clock/TickThread.h:152-161` already writes the rule and the reason: the before hook runs
*"immediately BEFORE each processTick, so anything it submits is drained by that same tick. The
distinction is not fussiness… submitting it from the AFTER hook would put it in the queue that
the NEXT tick drains, so the log would say it happened one tick after it did."* Every
engine-origin submit from a tick-thread hook in this repository obeys it: `runner.beforeTick` at
`Console.cpp:2139`, whose eight steps are listed at `Runner.cpp:4399-4406` and seven of them
submit, and the clock crossing at `Console.cpp:2150-2151`. The after hook (`:2156-2270`) submits
nothing at all — it flushes, reads state, marks stale and publishes. So the two halves split by
what they are: the **before** hook reads `session.lastChangeTick` and submits
`document.autosave`; the **after** hook assigns `dirty`, `canUndo`, `canRedo`, `undoName`,
`redoName` and `recovery` into `state` before `parameters.publish` at `:2263`. Writing it the
plan's way would not have been visibly wrong — a tick of slippage in an autosave is unobservable
to anyone — and that is precisely why it is corrected here rather than allowed to pass: a rule
that is followed except where nobody would notice is a rule that has stopped meaning anything.
§14.10, which owns the arithmetic, cites this paragraph rather than making the correction a
second time.

The submit takes the form the serve wiring already uses, `engine.submit ({ "engine",
"document.autosave", {} })` — `origin::engine` is the literal `"engine"` (`command/Event.h:56`)
— and it inherits its justification from `audio.editBuilt`, whose comment at
`Console.cpp:2059-2066` is the sentence to keep: *"THE GRAPH EXISTS, AND THAT IS AN EVENT, not a
variable being set."* A save that happened is an event. The bytes it wrote are not.

**And the rule cannot be stated as *a handler never submits*, because there is a shipped
counterexample.** `list.loadToTime`'s handler calls `runner.loadToTime` (`Runner.cpp:5102`),
which submits engine-origin `node.set` at `:1288-1302` — deliberately, per its own comment at
`:1284-1286`: *"Sent through the ordinary write, so a replay reproduces it exactly and, having
no sender, does not move the rig."* The accurate form is that **a handler submits only what a
replay must re-derive identically**, which is a narrower licence than it sounds: everything else
a handler learns is a fact about the world, and a replay has no world. Phase 5 adds exactly one
engine-origin record and the analyser thread adds none.

**Two facts about the record a driver author has to know before writing one.** The record needs
`--out` on a replay (§14.7), and that replay *creates* `<out>/recovery/` as a side effect of
reproducing the session, which is a fact about the destination and not a fault. And the record
survives a kill but not a power cut: `EventLog::writeLine` does `*file << line << '\n';
file->flush();` (`log/EventLog.cpp:241-250`) — a stream flush to the OS, not `fsync`, which is
exactly what `EventLog.h:74-76` claims and no more. That is why the 5.5 driver can kill the
engine and read `document.autosave` back out of the log, and why the kill must be a real one
(§14.10).

**Now the two mechanisms that will produce no record at all, and both are conclusions rather
than omissions.**

**The media analyser takes no decisions.** It will be handed the file names the show declares at
load, and any name a `media/file` edit introduces; it hashes; it finds the pyramid in
`media/.timbre/<sha256>.tpy` or builds one; it publishes an immutable snapshot. Every step is a
derivation from bytes: the same file gives the same hash gives the same pyramid, on this machine
and on the Mac mini and next year. A record would be a record of nothing. That is §13.4's
`claim.land` argument word for word — *"adding a `claim.land` record would be adding a second
way for the model to reach a state it can already reach, and the two would eventually disagree"*
— and it applies here more cleanly than it did there, because a claim at least depends on what
else was holding a slot and a spectrum depends on nothing but the file. `wfg analyse` will be a
verb rather than a command for the same reason (plan decision 12, §14.12).

**And the readouts it feeds are not logged either, which is a rule the table wrote down before
this phase existed.** `run/position` is *"a READOUT and never a model input… not written to the
log, because a log of every position would be a log of the clock rather than of anything anybody
decided"* (`csv:98`), and `run/rangeIteration` says the same at `csv:102` — *"a bed looping for
four hours would otherwise write a record every few seconds for something nobody decided."*
`run/timbre` is computed at publish from `position` and a table (§14.5), so it joins them by the
same rule and the same sentence, and `cue/hash` joins them by being a fact about a file. Four
readouts, no records, one argument.

**Undo is emphatically not in that category**, and the reason is what `wfg replay` cannot see:
it compares records and never the document or the undo stack, so an undo the engine was not told
about would leave the replayed document one edit ahead of the live one for ever while the replay
passed green. §14.9 owns that argument, the applied arguments that turn a divergent stack into a
record mismatch, and where inside `applyEvent` the transaction hook fires.

### 14.9 Undo — one transaction per applied command, and the three things it does not undo

PRD §3.20 promises that the *"per-domain undo histories from WFS-DIY port directly"*, and §4.3
puts undo first among the three things that are *"why anyone trusts show software"*. Both hold
with the qualification the reuse map carries (`docs/godot-reuse-map-0.1.md:531-537`): the
**mechanisms** port from `spatcore/control/state/TreeParameterStore` — an `UndoManager` per
domain, a `ScopedUndoSuppression` so a cue-driven recall cannot bury an operator's edit, a write
interceptor, a post-write hook — and the API does not, being `(paramId, channelIndex)` shaped
and belonging to a renderer with channels. Go.dot's write is an address and a schema row.

**The two reasons there has been no `UndoManager` are both measured, and both are answered
before one is attached.** `ShowDocument.h:40-46` states them, and they are not the same kind of
hazard:

| the hazard | what it is, exactly | what becomes of it |
|---|---|---|
| `var::equals` | With `nullptr`, `SharedObject::setProperty` calls `properties.set` (`juce_ValueTree.cpp:141-145`) and `NamedValueSet::set` compares `equalsWithSameType` — type-strict (`juce_NamedValueSet.cpp:154-167`). With a manager attached the guard becomes `if (*existingValue != newValue)` (`juce_ValueTree.cpp:150-151`), which is `var::equals` and type-loose (`juce_Variant.cpp:671-674, :701`), so a typed `1` written over a stored `"1"` would be dropped with no refusal and no log record | **Cannot fire through the write door — but by coincidence today, not by construction, so 5.4 pins it with a test rather than a comment** |
| the clock in `ActionSet` | `ActionSet` carries `Time time { Time::getCurrentTime() };` (`juce_UndoManager.cpp:73`), a default member initialiser evaluated at `new ActionSet (newTransactionName)` inside `UndoManager::perform` (`:151`) — inside the apply path | **Real, kept, and named as the one sanctioned wall-clock read in this engine** |

**The first hazard is answered by construction only if `toVar` is the only place a typed value
becomes a `juce::var`, and it is not.** The header claims one place; the reader is a second.
`CanonicalXml.cpp:352-372` writes properties through a hand-duplicated copy of `toVar`'s switch,
`:341` stores a canonicalised list as a `String` var, and `insertObject` writes `idProperty` as
a raw `juce::String` (`ShowDocument.cpp:580`). The switches agree today, so no fixture holds a
var of a type `toVar` would not produce — but nothing enforces the agreement, and the day one
row's reader type drifts from its writer type is the day an `UndoManager` silently drops the
first write to that attribute: the exact WFS-DIY defect `ShowDocument.h:27-32` was written to
prevent, reintroduced by the phase that attaches the manager. So 5.4 will pin the two switches
against each other in `UndoTests`. It is the one hazard here no reviewer can see in a diff.

**The clock read is real, is on the apply path, and is the one exception this engine grants.**
The rule it breaks: nothing inside the apply path reads the wall clock, because a replay must
produce the same records from the same inputs however long after the show it runs, and the
engine's own notion of time is the tick index (`TickThread.h:32-36`). Three facts grant the
exception. It happens **once per non-empty transaction** — `beginNewTransaction` sets a flag and
a name and reads nothing (`juce_UndoManager.cpp:223-227`), and an `ActionSet` is allocated only
when an action is performed (`:151`). The stamp is **stored and never observed**:
`LogRecord::toLine()` (`EventLog.cpp:110-142`) has no field for it, and JUCE's two readers,
`getTimeOfUndoTransaction()` (`juce_UndoManager.cpp:334-341`) and `getTimeOfRedoTransaction()`
(`:342-350`), are called by nothing here and must stay that way. And the alternative is
re-implementing `SetPropertyAction`, `AddOrRemoveChildAction`, `MoveChildAction` and their
coalescing — four pieces of arithmetic whose bugs would be this engine's, in the subsystem §4.3
says trust rests on. `UndoTests` will hold the other end: no published node and no log field
carries a wall-clock time.

**And the seam the plan reported is not cut — this paragraph is a correction** *(PR 5.0,
2026-09-09)*. The plan reads *"the seam is pre-cut and nothing is behind it"*, echoing
`ShowDocument.h:45-46`: *"The write choke point takes an `UndoManager*` from the start and is
handed `nullptr` until that phase arrives."* The header is stale. `setAttribute` takes no
manager (`ShowDocument.h:224`, `ShowDocument.cpp:452`), and neither does `insertObject`
(`ShowDocument.h:334-337`), `remove` (`:211`) or `move` (`:216`); the `nullptr` at
`ShowDocument.cpp:510` is a literal handed to `juce::ValueTree::setProperty`, not a parameter
threaded in; and nothing on the public surface can open, name, commit or query a transaction. PR
5.4 is a new member on a class with a hand-written move, plus a new hook, plus four call sites,
and should be reviewed at that size. The header sentence is corrected in the same PR.

**The manager cannot be an array of managers, and the reason is in the class it would live in.**
`juce::UndoManager` is `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`
(`juce_UndoManager.h:270`), which user-declares the copy members and so suppresses the implicit
move members: it is neither copyable nor movable, while `ShowDocument` has a hand-written move
constructor and move assignment (`ShowDocument.h:381-382`, `ShowDocument.cpp:288-308`) because
the listener behind `revision()` is registered *by address* (`ShowDocument.h:364-377`). So the
plan's *"an array so Phase 6's parameter domain is one line"* is the first line that would fail
to build. The member will be an `std::array<std::unique_ptr<juce::UndoManager>, domainCount>`,
and **the move will reconstruct the histories empty rather than carrying them**: the only place
a `ShowDocument` is moved is a test helper returning a freshly read document
(`tests/DocumentTests.cpp:75-85`), and a move that silently carried actions holding `Ptr`
handles into a tree, across the one seam this class hand-writes, is the same class of bug as the
listener registration that seam exists to fix. It adds no dependency: `ShowDocument.h:56`
already includes `<juce_data_structures/…>`.

**Nothing will attach a `ChangeListener` to a document `UndoManager`, and that is a rule rather
than an accident.** `UndoManager` is a `ChangeBroadcaster` whose `perform`, `undo`, `redo` and
`clearUndoHistory` all `sendChangeMessage()` (`juce_UndoManager.cpp:92, :162, :266, :285`),
inert only while nobody has called `addChangeListener` (`juce_ChangeBroadcaster.cpp:77-81`). The
day a JUCE desktop client (§14.16) attaches one to grey out an **Undo** menu item, the tick
thread posts to the message manager once per applied edit. So `canUndo`, `canRedo`, `undoName`
and `redoName` are read in the after-tick like every other readout (§14.4), and no client
subscribes.

**One domain today, and the second is reserved so that its arrival is a row rather than a
redesign.** `enum class UndoDomain { document }`, with Phase 6's parameter and binding writes
taking the second entry. That there is one and not two is plan decision 1, here to be overruled
early rather than late.

| domain | holds | why it is separate |
|---|---|---|
| `document` | everything a `persist == show` row or a structural door writes: names, kinds, order, routes, ranges, fades, mounts | it is what someone decided (§4.10) |
| *(reserved, Phase 6)* | parameter and binding writes — the mounted half | **an operator riding a level during a show must not be able to take back a cue rename by pressing Undo, and must not have to** |

That second row is the argument for the first. A fader ridden through an act emits hundreds of
writes; folded into one history they would bury the three edits somebody actually made, and an
operator reaching for Undo after a mistyped cue name at 04:12 would get their level back instead
— a fader jumping during a show, from a keystroke whose whole purpose was to undo a piece of
typing. Phase 5 builds neither the second stack nor undo of mounted writes (§14.15); it builds
the enum, so the day the second arrives nothing has to be untangled.

**One transaction per applied command, opened in one place, and the place is inside
`applyEvent`.** `Engine` gains `setBeforeApply (std::function<void (const Command&, const
Event&, std::int64_t tick)>)` beside `setLogging` — vendor-free, as `Engine.h:36-39` requires,
mirroring `TickThread::setBeforeTick` (`TickThread.h:162-168`) — and serve and replay both
install `document.beginTransaction (command.name, tick, origin, args)` (§14.13). Where it fires
is load-bearing and the plan does not say it: **between `Engine.cpp:117` and `:119`, after
`check.ok`, on `check.args`.** Above `checkArgs` (`:105`), a datagram about to be rejected for
arity would still set `newTransaction` and split a coalescing run the operator experienced as
one drag — and would do so *identically* on replay, since a rejected record is a rejected
record, so the divergence would be between the live stack and nothing, invisible to the one
check built to catch it. And `event.args` are the submitted arguments while `check.args` are the
coerced ones (`Command.h:25-26`, `CommandRegistry.h:81`), so a hook reading `event.args[0]`
would key coalescing on a value the handler never saw.

**The transaction is named after the command, and the name is what the operator reads.**
`undoName` publishes `getUndoDescription()` — the name of the transaction `undo` **would
unmake**, which is the last one that actually performed an action and not the one the hook has
just opened, because `beginNewTransaction` allocates nothing. So it says `node.set`,
`cue.create`, `object.delete`: the words §4.11 makes every gesture-reachable action carry, and
"Undo *object.delete*" is a sentence a client writes without a lookup table.

**The coalescing rule: same address, same origin, within twenty-five ticks.** Consecutive
`node.set` matching all three join the open transaction; everything else opens a new one named
by its command. Twenty-five ticks is half a second at 50 Hz — plan decision 2, and the only
figure in this subsection that a week of using the page can settle better than an argument can.
The rule matters because of what a client emits: a number field dragged in the inspector sends
one `node.set` per change event, a slider under a finger one per frame, and an undo that took
back one of those is a keystroke that has to be held down — which is how an operator overshoots
into the edit before the one they meant. Keyed also on the **origin** (§14.11), so two people
editing one address from two tablets get two steps, which is honest when two hands were
involved; and replay preserves the origin (`Replay.cpp:44`), so coalescing keyed on it
reproduces exactly.

| does not join the open transaction | why |
|---|---|
| a different address | one drag is one step; two fields are two edits |
| a different origin | two operators are two decisions, however close together |
| more than twenty-five ticks later | a pause is where a person stopped and looked |
| anything that is not `node.set` | every create, delete and move is a structural act somebody meant, and each gets its own step named after itself |
| the first write after an `undo` or a `redo` | `undo()` and `redo()` call `beginNewTransaction()` themselves before returning (`juce_UndoManager.cpp:265, :284`), so the window closes on its own — and a test that undoes, then writes the same address twice expecting a merge back into the pre-undo transaction fails for that reason and not for a bug |

**What JUCE merges natively is one action less than the plan claims, and the difference shows on
the first drag.** `createCoalescedAction` returns `nullptr` outright when `isAddingNewProperty
|| isDeletingProperty` (`juce_ValueTree.cpp:457-468`, the guard at `:459`), and this document
omits defaults: an absent attribute **is** its default (`ShowDocument.h:226-230`). So the first
write to an attribute a cue does not yet carry produces an *adding* action (`:156-157`) that
never merges with the next, and ten `node.set` on one fresh address are **one transaction of two
actions** — in a fresh show, every attribute nobody has touched. The design is unharmed, because
the undo *step* is the `ActionSet` and not the action; what would be harmed is a test written to
the parenthetical, so `UndoTests` will assert undo steps and never count actions.

**All three structural doors take the manager, or none of them do.**
`AddOrRemoveChildAction::undo` for an add removes the child **by index**, with a `jassert` that
the index is still in range (`juce_ValueTree.cpp:503-518`, the assertion at `:513`); it is
correct only while every structural change to that parent is itself undoable. The doors are
`insertObject`'s `parent.addChild` (`ShowDocument.cpp:604`), `remove`'s `parent.removeChild`
(`:954`) and `move`'s `moveChild` or `removeChild` plus `addChild` (`:1027`, `:1031-1032`). One
left on `nullptr` makes undo remove the wrong cue — silently, and only in shows where somebody
used both doors, which is every show. Two details a reviewer needs with the file open. **The
argument position is not uniform**, and a reader who learns "third" will get one of them wrong:
`setProperty`, `addChild` and `moveChild` take the `UndoManager*` third (`juce_ValueTree.h:251,
:336, :375`), while the `removeChild (const ValueTree&, UndoManager*)` overload used at `:954`
and `:1031` takes it **second** (`:348`). And the attribute writes inside `insertObject`
(`ShowDocument.cpp:600-601`) stay on `nullptr` deliberately: a child is built completely before
it is added (`ShowDocument.h:37-39`), so the node is not in the tree when they run and the one
action at `:604` carries the whole finished object, identifier and all. A cross-parent `move` is
then two actions in one transaction, undone in reverse (`juce_UndoManager.cpp:52-59`); a
within-parent move is one `MoveChildAction`, which coalesces consecutive moves of the same
parent on its own (`juce_ValueTree.cpp:558-565`), so the rule above is what keeps two ▲ presses
on the console's own gesture from collapsing into one step — noticed on the first day by whoever
pressed ▲ twice and got both back.

**Undo is a logged command, and the alternative is a replay that diverges in silence.** `undo [s
domain]` and `redo [s domain]` — defaulting to `document` — will be ordinary commands, `mutates
= true`, applied on the tick thread inside `applyEvent`, with the two new reasons §14.7 spells
and registered where §14.7 argues they belong. The rejected alternative is rewinding the tree
from a client gesture with no record, and it fails for the reason every hook in this engine is a
submitted command (§13.4): a replay runs no gestures, only records, so a log of `cue.create`,
`node.set`, `node.set` would replay into a document that still had the edits while the live
session's did not — same inputs, different show, and `wfg replay` exiting 0 because the records
were identical. What stops it is the applied arguments. Replay compares `toLine()` against
`toLine()` record by record (`EventLog.cpp:110-142`, `Replay.cpp:94-108`) and never compares the
document or the stack, so `appliedArgs = [domain, transactionName]`, and an `undo` that pops a
differently named transaction than the recorded session popped writes a different line and fails
on that record with both names on screen. It is the `go` pattern (§12.6) applied to a stack: log
what was *applied*, not what was asked.

**The identifier registry has to be rebuilt after an undo or a redo, and the reason is not the
one the plan gives.** Undo touches `IdRegistry` not at all: after undoing a delete the `id`
properties are back, because `removeChild` with a manager builds one action holding a
ref-counted `Ptr` to the child and its `undo()` re-adds that same `SharedObject`
(`juce_ValueTree.cpp:503-518`). But `remove` released every identifier under the node on the way
out (`ShowDocument.cpp:956-957`, `Ids.cpp:163-166`), so `findById` answers and `isTaken` says
no. The failure is therefore not the plan's *"a redo of a create that hands out a different
identifier"* — a redo returns the **same** identifier, always, the id being a property of the
re-added object (`:580`) and an applied argument in the record (`Engine.cpp:129-134`). The
document is self-consistent under undo; the registry is what has to be told, and the failure is
the **next** create: `generate()` draws from 2^40 and inserts whatever it finds free
(`Ids.cpp:126-142`), so an identifier the registry has forgotten is free and a cue created after
an undone delete can be handed the one a restored cue is already using — two objects with one
identity, every `refers` row pointing at a coin toss, failing not at the gesture but at the next
save or the next GO. The rebuild is `registry.clear()` plus one `reserve` per identifier from
the existing `collectIds` walk (`ShowDocument.cpp:847`) — **not** a fresh `IdRegistry`, because
`clear()` empties `taken` and keeps the splitmix64 `state` (`Ids.cpp:172-176`) while a new
registry would re-seed from the system entropy source, making a second entropy consumer inside
the class whose comment says there is exactly one (`Ids.h:88-92`) — the property that lets the
log carry every drawn identifier and a replay re-supply it.

**Three things undo does not touch, and each absence is a decision.**

| not undone | why | the consequence to expect |
|---|---|---|
| **state rows** — `list/@standby`, `Lists/@focus` | the operator's position is not an edit, and a GO writes only standby (`Runner.cpp:5204`), so undoing after a GO would take back the GO's pointer move and fill the stack with presses nobody would call edits (plan decision 3) | a delete's standby repair and a move's standby clearing **stay where they went** |
| **mounted writes** | `node.set` on an address outside `/godot` forks to the mount table before the document is reached (`DocumentCommands.cpp:359-360`); §3.1's load-bearing wall says the rig has state and Go.dot has content | Undo never moves a fader, and the fader it would have moved belongs to a processor that may be on another machine (§14.15) |
| **runs** | a run is not a decision, it is a thing that happened; §4.5's *"honest that the audio already escaped"* | undoing the delete of a cue does not stop, start or rewind anything that is playing |

The first row is the one that will be reported as a bug. `remove` repairs the containing list's
standby **after** the removal — the reasoning at `ShowDocument.cpp:874-895`, the arithmetic at
`:929-952`, the removal at `:954`, the write at `:959-960` — and `move` clears the vacated
list's pointer when the cue lands off the manual path (`:1038-1040`), both through
`setAttribute` on a state row and therefore both handed `nullptr`. So deleting the cue GO was
pointing at moves the pointer, and undoing the delete brings the cue back and leaves the pointer
where it went. That is honest rather than broken: undo restores what someone *decided*, and
where the operator is standing is not among those things. A pointer that jumped backwards on
Ctrl-Z would be the machine moving standby, which §3.5 forbids for the same reason it forbids a
trigger doing it. The cue comes back; the operator decides where to stand.

**`ScopedUndoSuppression` earns its place around `adopt` and, today, nowhere else.** `adopt`
swaps `showNode` wholesale (`ShowDocument.cpp:315-332`) and the stack's actions hold `Ptr
target` into the old `SharedObject` graph, so an uncleared stack would keep the previous show
alive in memory and undo into a tree nobody can see. `EphemeralState::read` needs no suppression
on its own account — it restores only `persist == state` rows and refuses anything else
(`EphemeralState.cpp:243-249`), and those are handed `nullptr` regardless. The counted shape is
right for Phase 6, where a cue-driven recall will write parameter rows in a domain that does
have a history; in Phase 5 it guards one call site, said plainly so that a reviewer finding it
used once does not think something is missing.

**The transaction machinery will allocate on the tick thread, and that is legal here.**
`UndoManager::perform` takes ownership of an action the caller newed (`juce_ValueTree.cpp:154`,
`:156`), news an `ActionSet` on a new transaction (`juce_UndoManager.cpp:151`) and does
`OwnedArray::add` (`:157`). §4.2's lipogram is the **audio** thread; the tick thread already
allocates a `std::string` per log record (`EventLog.cpp:112-113`). It is named here because *"no
allocation"* is the sentence a reviewer carries into a PR.

**Nothing falls out of this subsection towards §4.5's revert.** `go` writes precisely one thing
into the document — the list's `standby`, a state row handed `nullptr` (`Runner.cpp:5204`) — and
then spawns runs, which are not in the document at all, so the stack after a GO contains nothing
to pop and restoring the pointer would produce a revert that lies. §14.15 defers it and says
what it waits on. The lock refuses `undo` and `redo` outright, in their handlers rather than at
the doors, for the reason §14.11 gives.

**What the black-box driver checks, and the one seam a unit test cannot see.**
`phase5_document.py` (5.3 → 5.5 → 5.19) will do the undo half against a shipped binary over UDP
and HTTP — `undoName` after a rename, ten drags returning one press, an `undo` refused with
`lastError` ending `locked undo`, and, as the only assertion that proves the registry rebuild, a
group with children deleted, undone, and the same identifiers read back out of
`/godot/list/<id>/order`, where a colliding identifier shows up as a duplicate and nowhere else;
§14.10 carries the save-and-recovery half of the same script. The replay fixture `undo.wfglog`
will make the reproduction a ctest, with `diff -r` against the saved bundle at zero. The seam no
unit test reaches is **deleting a cue whose run is playing, and undoing it**: it needs a live
run holding a voice, a document edit removing the cue the run points at, and a publish
afterwards, in one process at one moment — a test that assembled a runner, a run table, a tick
loop and a document would be this driver with the transport taken off. The design's answer is
that the run table holds its own copies (`Run::cue`, `Run::kind`, and `Run::media` from 5.6 —
§14.5), none of them tree handles, so the delete cannot stop the sound and the undo cannot
restart it; the driver asserts exactly that.

### 14.10 A save that cannot be half-written, and an autosave nobody asks for

**A save must never destroy the file it is replacing, and today's does, on purpose and first.**
`writeBytes` (`Bundle.cpp:51-71`) opens a `juce::FileOutputStream` on the real `show.xml`
(`:53`), seeks to zero (`:61`), calls `truncate()` (`:62`) and only then writes the show
(`:64`). The order is not incidental: `truncate` flushes before it shortens the file —
`FlushFileBuffers` then `SetEndOfFile` on Windows (`juce_Files_windows.cpp:475-483`), `fsync`
then `ftruncate` on POSIX (`juce_SharedCode_posix.h:541-555`) — so the empty file is committed
to the platter, and the payload that follows is never explicitly flushed at all. Phase 1's save
is durable about the deletion and casual about the content, and the window between those two
facts is where a laptop's battery goes, a disk fills or somebody closes a lid. What is left is
not yesterday's show: it is a zero-length `show.xml` where the good one was. §4.3 names
crash-safe autosave as one of the three reasons anyone trusts show software, and a save that can
subtract the show is the wrong foundation to build the other two on.

**The bytes stay Go.dot's, and the fix is a sibling rather than a reversal.** The obvious cure —
`juce::File::replaceWithText` — is the one `Bundle.cpp:44-50` already refused, and the refusal
holds: it writes through a `TextOutputStream`, which turns every `\n` into `\r\n` on Windows,
and every file in a bundle is specified LF on all three platforms so that a show written on
Windows and one written on a Mac mini are the same bytes — what makes §3.20's *canonical,
editable, diffable* checkable at all, and what `tests/BundleTests.cpp:141-167` asserts file for
file. So `writeBytesAtomically` will keep the raw write and move the target: a sibling
`<name>.tmp-<pid>` **in the same directory**, written, flushed, closed, then `replaceFileIn`.

**Same directory is the load-bearing word, and JUCE is the reason.** `replaceFileIn`
(`juce_File.cpp:323-336`) has three branches and only one is atomic. Same path returns `true`
having written nothing. A target that does not exist delegates to `moveFileTo` (`:329`) — so the
first save into a folder with no `show.xml` is a move, and this section claims no atomicity for
it. A target that does exist reaches `replaceInternal` (`:331`): `ReplaceFile` on Windows
(`juce_Files_windows.cpp:372-379`), and on POSIX `moveInternal`
(`juce_SharedCode_posix.h:428-431`), which is `rename(2)` and atomic **within one filesystem**.
Across a volume boundary the rename fails, and JUCE's fallback is not a clean failure: it falls
through to `copyInternal` then `deleteFile` (`juce_SharedCode_posix.h:409-426`), and on Linux
`copyInternal` deletes the destination first and streams the bytes afterwards
(`juce_CommonFile_linux.cpp:38-58`) — the very hazard this paragraph exists to remove,
reintroduced by the call meant to remove it. The temp is a sibling because a copy is not a
replace, not because siblings are tidy.

**What is left when the replace itself fails is the right failure.** `replaceFileIn` returns
before the `deleteFile()` at `juce_File.cpp:334`, so the temp survives: **the old file, whole,
and a temp file beside it.** The show that was on disk at 18:00 is still the show on disk, and
nothing an operator trusted has become shorter. The three writes keep their order and their
reason (`Bundle.cpp:273-278`), and each becomes atomic on its own:

| write | what a crash between this write and the next costs |
|---|---|
| `show.xml` (`CanonicalXml::write`) | nothing — either the new show is committed or the old one is |
| `state.xml` (`EphemeralState::write`) | a standby position and a focus one tick old, which §3.20 puts in a separate file precisely because losing it is not losing work |
| the manifest | **nothing at all, for a bundle that had one.** `manifestText()` is a pure function of `Schema::formatVersion()` (`Bundle.cpp:38-42`), so the third write is byte-for-byte what was there, and `contentHash` never reads it (`:209-220`). The one casualty is a save into a folder that never had a manifest, where the next `Bundle::open` refuses with *has no `<name>.wfg`* (`:118-121`) |

The dangerous windows were always **inside** writes one and two, never between them, and
temp-and-replace closes exactly those.

**The retry is blind, because the API will not say what went wrong.** `replaceFileIn` returns
`bool` and swallows `GetLastError`, so "retry on a sharing violation" is a distinction no code
here can make; 5.2 will write one blind retry after a short pause. And the file at risk is the
**target**, not the temp: `FileOutputStream::openHandle` takes `GENERIC_WRITE, FILE_SHARE_READ`
(`juce_Files_windows.cpp:429-433`) and the temp is closed before the replace, whereas
`ReplaceFile` fails while another process holds `show.xml` open without `FILE_SHARE_DELETE` — an
editor, an indexer, a sync client. One retry, because the failure JUCE will not name is almost
always transient and held on the far side.

**The reason gets its word in 5.1 and does not get its sentence.** `reason::writeFailed`
replaces `document.save`'s `reason::badAddress` (`Bundle.cpp:307`) with the debt rather than
with 5.2's atomic write, for the reason §14.7's table gives. What that does **not** fix is the
silence: `Bundle::save` builds a `ReadResult::failed (error)` carrying the full path (`:57`,
`:66`, `:279`) and the handler throws it away (`:306-307`), so an operator whose disk is full
reads `write-failed document.save` and learns that much and no more. This subsection claims the
word and not the diagnosis; §14.15 says why the free-text half is not opened here.

**`dirty` has been published since Phase 1 and has never once been true.**
`EngineState::documentDirty` is declared at `ParameterTree.h:123` and published at
`ParameterTree.cpp:1262`, and the only assignment anywhere in the repository is
`tests/TreeTests.cpp:450`. Every client that has ever read `/godot/document/dirty` — the console
among them — has been told the show was saved, continuously, since the node existed. Phase 5
makes it true, and the definition is `document.showRevision() != session.savedRevision`.

**The dot counts the show half only, because the operator's position is not an unsaved change**
— plan decision 4, here to be overruled early rather than late. `revision()` counts *changes*
and is bumped by the `juce::ValueTree` listener rather than by the doors
(`ShowDocument.h:345-354`), which is what makes it impossible to miss a writer; `showRevision()`
is the same listener asking one more question — structural changes bump unconditionally, a
property change looks the row up (`Schema.cpp:389-395`, `persist()` at `Schema.h:67`) and bumps
only for `persist == show`. A GO writes `list/@standby` through the same choke point every edit
uses (`Runner.cpp:5204`, `CueCommands.cpp:43`, `CueList.cpp:360, 365`), so it moves `revision()`
and not `showRevision()` — §3.20's line, drawn where it puts the playhead and the selection in a
separate file, and the difference between a light that means something and a light that means
nothing: an operator told there are unsaved changes after every GO stops reading the dot by the
second act, and then on the night it is right, it is still not read. In words beside the colour
(§4.8). Two facts that would otherwise arrive as bug reports: `id` never fires the listener,
because `insertObject` writes it at `ShowDocument.cpp:580` before the node joins the tree at
`:604`; and **the dot does not go out by undoing**, `showRevision` being monotonic while an undo
writes through `setProperty (…, nullptr)` (`juce_ValueTree.cpp:447`), which the listener counts
as readily as the edit. The dot means *the file on disk is not this document's history*, not
*this document differs from the file* — and §14.15 records the second question as deliberately
not asked.

**Autosave is a decision the engine takes on its own, so a hook takes it and a handler applies
it.** A `juce::Timer` is refused on the grounds `TickThread.h:23-36` refuses it for the clock —
Spike 05 measured a 20 ms timer on an *idle* message thread at 0.76 ms median and 2.60 ms
lateness at the 99th percentile — and, more decisively, the message thread is not the document's
thread: `ShowDocument.h:48-50` says *"THREADING: none of its own. The engine's tick thread owns
this object and is its only writer and only direct reader"*, which is what makes it safe to
serialise the model at all. So the **before** hook will compute `autosaveDue (session, tick)` —
a pure function in `document/DocumentSession.h`, testable at its edges without a disk — and
submit engine-origin `document.autosave`; the handler writes the bytes; the log records an `A`;
`wfg replay` re-applies it. That the arithmetic is in the before hook and not, as the plan drew
it, the after hook is §14.8's correction, made once there. The after hook keeps what belongs to
it: `session.lastChangeTick` is stamped there when `showRevision()` has moved, and
`state.documentDirty` is assigned before `parameters.publish` at `Console.cpp:2263` together
with `canUndo`, `canRedo`, `undoName`, `redoName` and `recovery` — or all six publish one tick
stale, the bug the comment at `:2170-2175` describes about `state.tick`.

| condition | ticks | in seconds | why this one |
|---|---|---|---|
| not dirty | — | — | nothing to write, and a quiet show writes nothing at all |
| `tick - lastChangeTick >= 100` | 100 | 2 s of quiet | a designer who has stopped typing has finished a thought; writing mid-drag would fire on every intermediate value |
| `tick - lastAutosaveTick >= 1500` | 1500 | 30 s ceiling | a designer who has *not* stopped — a long drag, a bulk edit over forty cues — still gets a floor under what a crash can cost |

The two numbers are plan decision 5 and are here to be overruled early rather than late; a
fortnight of tech will settle them better than this paragraph can.

**And this is the one place Phase 5 comes near §4.1, so the constraint gets an answer rather
than a measurement.** Constraint 1 is *the GO path is small, boring and merciless; GO never
blocks*, and this phase proposes an unrequested disk write on the thread that applies GO,
decided by the engine rather than by a person. The write is one canonical serialisation of the
show plus one `replaceFileIn`, on a document nothing else is touching, at most once every two
seconds of quiet and once every thirty otherwise. What it can cost **a GO** is narrower, and
that is the part that answers the constraint: GO is not what autosave contends with. A GO
already submitted is applied first, because the queue is FIFO and the before hook's submit joins
it behind every datagram already there; a GO arriving during the write waits the remainder of
one tick, exactly as it waits behind any other applied command. There is no lock, no allocation
the tick thread does not already make, and no path by which a disk stall can hold GO longer than
the overrun of the tick it landed in. So the exposure is a single late tick — which is what
**M23** measures, and §14.14 carries the threshold and the writer-thread fallback above it. The
record is identical either way and a replay never touches the disk, so the fallback changes no
fixture and no assertion: drawn here it is a threshold, drawn in review it would have been a
redesign.

**The bytes go to `<bundle>/recovery/`, never to the authored file, and that is not a matter of
taste.** `recovery/show.xml` and `recovery/state.xml`, written by the same
`writeBytesAtomically` that writes the real pair. A designer who spent an afternoon of tech
moving cues and then decides the afternoon was wrong must be able to throw it away by not
saving, and an autosave that wrote into `show.xml` would take that gesture away — silently, and
from the person who most wanted it. §4.10 says the document holds what someone decided; nobody
decided to save. The folder sits inside the bundle so that a bundle carried to another machine
carries its own unfinished work, and `Bundle::contentHash` covers `show.xml`, `state.xml` and
the non-recursive contents of `namespaces/` and nothing else (`Bundle.cpp:209-220`), so
`recovery/` does not disturb the log header. One asymmetry is worth deciding here rather than
discovering unattended: `Bundle::save` calls `folder.createDirectory()` (`:267`), parents and
all, so a `document.save` against a bundle somebody moved mid-session silently makes a new one
at the old path holding three files and no media. That stays true of `document.save`, because
somebody asked for it. It will not be true of autosave, which is rejected with `write-failed`
when the folder has gone — an unattended writer that invents a folder is how a bundle acquires a
twin.

**A log with an autosave in it needs both of replay's flags** (§14.7): `--bundle` for
`registerDocumentCommands` and `--out` on top of it for `registerBundleCommands`, where
`document.autosave` is registered because it writes bytes. Every session from 5.5 onward writes
such a log, and `phase1_session.py:310-312` already passes `--out={workspace}/replayed` for
exactly this reason with `document.save`.

**Recovery will be a client's decision, because the engine is headless and has nobody to ask.**
On open, `wfg serve` will look for `recovery/show.xml`; finding it, it will print `wfg: recovery
available`, publish `/godot/document/recovery = true`, and do nothing else. Adopting it silently
would be wrong three times over. It would make the show on screen differ from the file the
operator opened with no gesture in between. It would decide on their behalf that the abandoned
afternoon was worth keeping, which is the one decision autosave exists to leave open. And
`adopt` replaces the root wholesale (`ShowDocument.cpp:315-332`), lock included, so a silent
adopt would also change `Show/@locked` — a show that came back unlocked because a file on disk
said so, during a performance, is §14.11's nightmare arriving through this subsection's door.

| command / flag | what it does | notes |
|---|---|---|
| `document.recover` | adopts `recovery/show.xml` and `recovery/state.xml`, clears the undo history, `markStale` | leaves the document **dirty**, deliberately: the recovered work is not on disk as the show, and the dot is telling the truth. `reason::noRecovery` when there is nothing to adopt |
| `document.discardRecovery` | deletes the folder | `no-recovery` likewise; refusing an empty gesture is cheaper than pretending it worked |
| `wfg serve --recover` | applies the recovery before the first publish | for scripts and for the black-box driver, which has no person to click. It goes in the **usage string** as well as the parser (`Console.cpp:2421-2423`): `--device` and `--device-type` are parsed at `:1891-1923` and appear in no usage line, an omission already one phase old, and the usage string is what an operator reads at 04:12 |

A successful `document.save` deletes `recovery/`, because the work has become the show. A clean
exit deletes it **only when the document is not dirty** (`Console.cpp:2315-2329`, after
`ticks.stop()` has joined the only writer) — a tidy shutdown with unsaved work is the case the
folder exists for. And of the four verbs that open a bundle through `Bundle::open` — serve at
`Console.cpp:1440`, `wfg tree` at `:871`, `wfg validate` at `:1010`, `wfg replay --bundle` at
`:509` — only `validate` will say the word: one line noting that `recovery/` is present and was
not validated, because `validate` is what somebody runs on a bundle they suspect. A tree dump
and a replay are not asking about unfinished work.

**The lifecycle, and the one command that is not built.**

| command | what it does | what it deliberately does not |
|---|---|---|
| `document.revert` | `Bundle::open (folder, document)` back into the same object through `adopt`, undo history cleared, `markStale`, and `savedRevision` **re-stamped** from `showRevision()` | it does not leave the dot lit. `adopt` ends `++changeCount` — *"A load is the largest change there is"* (`ShowDocument.cpp:329-331`) — and `EphemeralState::read` bumps it again per restored value (`:258`), so a revert that did not re-stamp would report unsaved changes at the instant the document matched the disk. `document.recover` re-stamps nothing, for the same reason read the other way |
| `document.saveAs <s path>` | `Bundle::save` into the named folder, **plus** a directory copy of `namespaces/` | it does not re-point the session, and it is save-plus-a-copy rather than one act. `Bundle::save` writes exactly three files (`Bundle.cpp:276-278`) and refuses `namespaces/` as a principle (`Bundle.h:80-83`), so `saveCopy` copies them and does not pretend the copy is a save. The manifest name comes free: `manifestFile` is `<folder name>.wfg` (`Bundle.cpp:75-78`) |
| `document.load` | **not built** — a process restart, and §14.15 says why | |

`saveAs` not re-pointing the session is the half most likely to be argued about. A *save a copy
for the archive* gesture that silently makes the archive the live document is a trap with a
delay fuse: the operator's next Ctrl-S goes somewhere they did not name, and they find out at
the next load. A client that wants to work in the copy opens it, and opening is a restart.

One plumbing note, because it is the whole difference between a session record that works and
one that quietly does not: `doc::DocumentSession { juce::File folder; std::uint64_t
savedRevision, autosavedRevision; std::int64_t lastChangeTick, lastAutosaveTick; }` lives in the
serve scope and is captured **by reference**. The existing lambda captures `folder` by value
(`Bundle.cpp:294`); the pattern for a mutable per-session record is one call earlier, where
`registerDocumentCommands` is handed a lambda capturing `[&mounts, &sender]` from the serve
scope (`Console.cpp:1494`). Captured by value, `savedRevision` would stay at its initial value
for the life of the process and the dot would never go out.

**And the driver ends the way nothing else can end it.** `phase5_document.py` will copy a bundle
(`common.copy_bundle`, `common.py:602-608`), serve it, rename a cue and wait for `dirty`; wait
for the `document.autosave` record **and** for `recovery/show.xml` to exist, which are two
different claims — one that the engine decided, one that the disk agrees; kill the process and
start a new one on the same folder, which must print `wfg: recovery available` into the notices
`Server` already collects (`common.py:451-528`) and publish `recovery == true`; then
`document.recover`, and the renamed cue is back, dirty; then `document.save`, and both flags go
false; then an edit and `document.revert`, and the old name is back with the dot out. The kill
has to be `process.kill()` and not the harness's default `Server.stop()`, which calls
`process.terminate()` (`common.py:536-543`) — on POSIX a `SIGTERM` that `wfg serve` handles into
a clean shutdown (`Console.cpp:1156`, `:1162-1175`, `:2315-2329`) and only on Windows a
`TerminateProcess` no handler sees, so a driver using it would test shutdown on two platforms
and crash recovery on one. That last step is the check no unit test can make: `BundleTests` can
prove the recovery bytes round-trip in one process, and should; what it cannot do is die.

### 14.11 The edit lock — one predicate, four doors and a hatch

**A lock that only one client honours is not a lock.** §3.2 makes desktop UI, tablet, surface
bridge, MCP and external scripts *clients of the same surface*, and the law under it — *nothing
the UI can do that the API cannot* — means a client that hides its own buttons has locked
exactly one of them. The tablet in the house, the MCP client answering a designer's question and
the script somebody wrote to renumber a scene all reach `node.set` and `object.delete` through
the same socket, and none consults another client's `localStorage`. The failure the lock exists
for is §3.17's, undressed: *"Accidental contact is real (a forearm on a screen in the dark)."* A
promise the operator relies on has to be kept by the thing they are all talking to. That is
decision **W**, and it is why `locked` is an engine node (§14.4) and the console's show-mode
preset (§14.3, PR 5.14) is a *presentation* of it — buttons hidden because the engine will
refuse them, never instead of the engine refusing them.

**And it is one predicate, in exactly the places the document can change.** The alternative — a
flag on `Command`, consulted per command — is a lock with a hole in it the day a command is
added, and Phase 5 adds seven: §14.7's six operator commands and §14.8's one engine-origin
record, which writes bytes and knocks at no door. The document has four doors and every write in
the engine goes through one: `setAttribute` (`ShowDocument.cpp:452`), `insertObject`
(`:540-606`), `remove` (`:856`) and `move` (`:965`). All twelve insertions funnel through
`insertObject` (`:611, 634, 652, 672, 695, 711, 727, 755, 779, 815, 833, 842`) — the ten creates
plus `group.role` at `:815` and `list.persistent` at `:833`, which is why the idempotent pair
below reaches a door at all; nothing outside `ShowDocument.cpp` calls `setProperty`, `addChild`,
`removeChild` or `moveChild` on the live tree. A predicate at the doors cannot be got past by
forgetting to ask. Two things complicate that, and both belong in the diff before somebody
reviews one against it.

**`createRackChannel` mutates the document before it reaches its door.** It makes the `<Rack/>`
container on demand — `rack = juce::ValueTree ("Rack"); audio.addChild (rack, -1, nullptr);` at
`ShowDocument.cpp:691-692` — and only then calls `insertObject` at `:695`. A check written
inside `insertObject` therefore lets a locked show gain an empty `<Rack/>`, bump `revision()`
through `valueTreeChildAdded` (`ShowDocument.h:350`), light the dirty dot, and *then* refuse the
command: a document mutated by a refusal, in the phase whose whole subject is trusting the save.
It is the only create with a mutation ahead of the door — `createRole` (`:808-809`) and
`createPersistent` (`:830-831`) return the existing child before inserting anything, and every
other create only reads. So the predicate will be a private `refuseIfLocked()` called at the top
of `createRackChannel` as well as inside the four doors, and 5.3 is reviewed with that line in
the diff or it is not finished.

**And there is a fifth writer, which is not a door but a hatch.** `ShowDocument::adopt`
(`:315-332`) removes the listener, moves in a new root and a new registry, re-attaches and bumps
the counter; it goes through none of the four and it is the mechanism behind `Bundle::open`,
`document.revert` and `document.recover`. Two consequences. The lock check for `revert` and
`recover` lives in *those handlers*, because `adopt` has no door to put it in. And because
`locked` is a property of the root, `adopt` replaces the lock with whatever the loaded
`state.xml` says — a revert of an unlocked bundle would silently unlock a locked session, which
is why the commands that open the hatch must refuse rather than the hatch being guarded. Four
doors for the edits, one hatch that does not edit the show but replaces it, lock included.

**`undo` and `redo` refuse in their handlers for a sharper reason still**: they do not use the
doors at all. `SetPropertyAction::undo` writes through `target->setProperty (name, oldValue,
nullptr)` (`juce_ValueTree.cpp:447`) and `AddOrRemoveChildAction::undo` through `addChild` and
`removeChild` on the SharedObject directly (`:507`, `:514`). JUCE's actions hold the tree, not
the document, so a lock at `ShowDocument`'s doors would see none of it. Refusing the command is
the only place the refusal can go, and the only place it *should* go: half a transaction undone
is worse than none.

**`persist == show` only, and the state half is exempt — as a rule, not as a property of
`locked`.** Stated as a property of the lock row — *the lock is exempt from the lock it sets* —
that is true and invisible, and the next `state` row would inherit the exemption without anybody
deciding to grant it. Stated as a rule it is the better sentence: **the lock is a lock on the
show half; the state half is where the operator is standing, and a lock has no business freezing
that.** It is §4.10's split applied to a gesture (`SchemaTypes.h:63-73`). The load-bearing
reason is the one that would otherwise be found by a fixture: `EphemeralState::read` restores
every saved state value through `document.setAttribute` (`EphemeralState.cpp:252-259`), whose
comment says why — *"Through ShowDocument's one write path, so a value restored from a file is
checked exactly as a value written over OSC would be."* A lock that refused state writes would
make a locked bundle refuse to load its own `locked="true"`.

**The predicate is `persist == show` and nothing else — not `!= state`, which would freeze the
aim.** The table has a hundred and sixty-one rows, and exactly three are writable and not
`persist=show`: `lists,focus` (row 24) and `list,standby` (row 29), the only two `persist=state`
rows in it, and `list,aim` (row 30), the only `rw` row whose persist is `none`.
`document,locked` will be the third state row and the fourth the predicate lets through, and the
four are three different reasons rather than one — the operator's position, a lock that cannot
refuse its own release, and a question about where somebody is pointing that is not in the
document at all. Said as *three state rows and no more*, a reviewer implementing `if (persist !=
state) refuse` passes the stated test and refuses `node.set /godot/list/<id>/aim` on a locked
show. Nothing breaks today if they do — the `list.aim` command never reaches the door, writing
`runner.listState()` instead (`Runner.cpp:5035-5060`) — which is exactly why the mistake would
survive review and be found by a client.

**What keeps working is §14.7's table, and not one entry of it is an exemption**: every command
in those rows is untouched *by construction*, writing a state row through the one door or
knocking at no door at all, which is what makes the lock cheap to reason about a year from now.
The mounted write is the one that would have been easy to get wrong: §3.17's third role for the
tablet is *"parameter adjustment while walking the house"* — an operator in row H riding a level
on a processor because that is where the show sounds wrong. Locking the document while the show
runs is the point of show mode; locking that would be locking the mixing, which is the opposite
of the point.

**Why `mutates` could not be the predicate.** `Command::mutates` (`Command.h:103`) is the
obvious candidate and it fails three ways, of which the third settles it. It is too coarse: `go`
(`Runner.cpp:5165`), `run.kill` (`RunCommands.cpp:515`) and `node.set`
(`DocumentCommands.cpp:345`) all declare `mutates = true` (`:5170`, `:519`, `:348`), so a lock
keyed on it would refuse GO. It is wrong about itself: `list.aim`'s own description reads
*"Points at a position in a list … Changes nothing"* and it is registered `mutates = true`
(`Runner.cpp:5035-5040`), while `document.save` — the command a locked show most needs —
declares `true` at `Bundle.cpp:293` and would refuse itself. And it is **unread**: nothing in
`src/` consults the field. The only reader in the repository is one assertion
(`tests/EngineTests.cpp:83`), and the ten commands that set it `false` are `noop` plus nine
engine-origin *reports* (`Engine.cpp:31`; `AudioCommands.cpp:24`; `RunCommands.cpp:27, 61, 82,
152, 215, 284, 314, 354`). What it marks is reports against requests, not writes against reads,
and giving the lock to a member whose correctness nothing has ever checked is how a predicate
becomes documentation.

**The lock is `persist=state`**, so a show locked at 20:40 whose engine was restarted at 20:44
comes back locked: §14.4 argues that placement against §4.10 and books the mechanism, and
nothing here is new plumbing.

**A lock that silently drops a gesture is worse than one that refuses.** OSC has no reply
channel, so a write the engine will not take comes back only as a reading (§14.2) — and the
console makes the same argument about a button it declines to hide: a cue the pointer may not
stand on is *"refused by the engine and the refusal says which, which is a better answer than a
button that is not offered"* (`clients/console/index.html:1191-1194`). Show mode is that
argument at document scale, and §4.8 is the other half — a greyed-out button is colour carrying
the whole of it. So: **`inline constexpr const char* locked = "locked";`**, in `namespace
reason` in `command/Command.h`, where the header states the rule itself (`:107-108`). It reaches
`/godot/engine/lastError` as `"<tick> <seq> <origin> <reason> <command>"` (`Engine.cpp:70-74`),
so an operator reads

```
4812 17 ws:192.168.1.7:53412 locked cue.create
```

and knows three things: that the show is locked, which gesture was refused, and — because the
origin is `udp:<host>:<port>` for a datagram (`UdpEndpoint.cpp:37`) and `ws:<ip>:<port>` for a
WebSocket write, juce_simpleweb's connection id being already `<ip>:<port>`
(`OscQueryServer.cpp:371-378`) — *which machine* sent it. The tablet in the house and the booth
machine are distinguishable in the one line the refusal gets. That origin is also what §14.9
keys coalescing on.

**The refusal goes beside the read-only check at `ShowDocument.cpp:462-463`, before the parse at
`:466`, and the position is not a detail.** `:462-463` is `if (target.isDerived ||
target.attribute->access() == Access::read) return EditResult::failed (reason::readOnly);`, and
the lock's check goes immediately after it at `:464`. Placed instead at the far end of the
function, next to the `setProperty` call at `:509-510`, a locked show would answer
`type-mismatch` for a badly typed value and `locked` only for a well-typed one — sending an
operator to look at their encoder when the answer was that the show is fixed. The refusal that
costs the least to understand is the one that arrives first. And the gesture that sets the lock
is a plain datagram rather than a command (§14.7), with §14.2's answer for the client that sends
the integer `1` instead of `true`.

### 14.12 Spectral colour — a cache, a pyramid, and a test before anything is drawn

**The colour says what the sound is made of, the brightness says how high it is, and the numbers
say both again in words.** PRD §3.30 is explicit about which dimension carries which: per
window, on a log-frequency axis, the **spectral centroid** gives the hue along the ramp, the
**spectral flatness** gives the saturation — a sine saturated, broadband noise grey — and
amplitude stays what the waveform's shape already carries. Then the sentence the design turns
on: **lightness is monotonic with frequency**, dark low to bright high. That is most of what
makes §4.8 hold *on its own terms* rather than by an exception: a colourblind operator reading a
desk of eight sampler faders separates the bass bed from the high effect by brightness alone,
because brightness is the frequency axis. §3.30 then offers an exemption — *timbre has no other
carrier, and it is an aid to mixing rather than a state the show depends on* — and this phase
does not take it. **The bar carries the colour and the row carries the numbers.** The timbre
reading is a string a client prints as readily as it paints (§14.5 owns the node and the route
the bar reads its frames from), and printing it beside the bar keeps §4.8 true by construction
rather than by argument. A surface that shows the bar and not the numbers is relying on the
exemption; one that shows both does not need it, and 5.17 will build the second (§14.3).

**The ramp is a starting point and not a palette.** Six stops — 40 Hz near-black purple, 150 Hz
deep blue, 500 Hz red, 1.5 kHz orange, 4 kHz yellow, 12 kHz green — with saturation `1 −
flatness`, lightness climbing 0.15 → 0.85 in the log-centroid, window 2048 and hop 1024. Those
are plan decision 8 and are here to be overruled early rather than late; the exact colours are
the author's once the sine/noise/sweep bundle is on screen, which is the whole reason that
bundle exists before anything is drawn.

**An analysis is regenerable from the file, so it is not something anybody decided.** §4.10 says
the document holds what someone decided and never what the machine happened to be doing, and
§3.20 puts derived state in a separate file or outside the authored half entirely. A spectrum is
further out than any of those: it is not even a decision the *engine* took, it is arithmetic
over bytes that already exist. So it will live in **a cache beside the media, keyed by content
hash, like a peak file**: `<bundle>/media/.timbre/<sha256>.tpy`. Keying by content rather than
name buys two things worth the hash: **the same file under two names is analysed once**, and **a
renamed file keeps its colours** — which matters because renaming media is a normal
document-time act (§3.20's batch tools) and re-analysing a gigabyte because somebody fixed a
typo teaches people not to tidy. The cache is invisible to the duration walk by construction:
`mediaDurations` visits the document's `Media` elements and never enumerates the folder
(`MediaInfo.cpp:70-91`), and `Bundle::contentHash` reads three things, none of them in `media/`
(`Bundle.cpp:209-220`).

**And here is the mismatch §3.30 hands Phase 5 to resolve.** §3.30 says *"PR 4.1's `MediaInfo`
side table, keyed by path and already holding a file's duration, is where the cache is looked
up"* — but that table is keyed by **path** and the cache by **content**, and the two are not the
same key. The answer is not a second parallel map from path to hash, which is a second thing to
keep in step and, per §13.4's rule, *the one that is wrong is always the copy*. The answer is
**one record per file**. What exists today is not an object at all: `audio/MediaInfo.h:78-79`
declares one free function, `mediaDurations`, returning the map **by value** into a `const auto
durations` local of `runServe` (`Console.cpp:1546`) whose address is then handed to the tree and
the runner (`:1553`, `:1555`). 5.6 will turn that into a `MediaInfo` object owning a `map<path,
MediaRecord>` — `struct MediaRecord { double seconds; std::string contentHash;
std::shared_ptr<const TimbrePyramid> pyramid; }` — keyed by the same bundle-relative path the
document writes. Path is the key because path is what a cue names; hash and pyramid are *fields*
of the record, arriving late.

**The durations half must stay frozen after load, and that is law rather than an aside.** The
code already says so, in a comment nobody is currently obliged to read:

> *"Compared by ADDRESS: the map is filled once when the show is opened and handed over by
> pointer, so a different pointer is a different show's media and the same pointer is the same
> numbers."* — `SlotAnalysis.h:202-205`

`SlotAnalysis::ensureBuilt` caches on `builtAt == document.revision() && builtWith == durations
&& builtAt != 0` (`SlotAnalysis.cpp:38`), and `ParameterTree::publish` calls it first thing,
**every publish** (`ParameterTree.cpp:1185`). Both halves are sharp edges. If the accessor
returned by value, or a map reached through a swapped `shared_ptr`, the address comparison would
fail on every publish and the slot walk — about 77 ms of §13.14's 208 ms Debug-build analysis —
would rebuild fifty times a second on the tick thread. And if the analyser ever wrote a
corrected duration back into the same map, the cache would *not* notice — same pointer, changed
numbers — and every slot overlap would be computed from stale seconds. So
`MediaInfo::durations()` returns `const std::map<std::string,double>&` into a member whose
address never moves and whose contents never change after load, and the hash and the pyramid
live where the pointer-keyed cache never looks. `ParameterTree::setMediaDurations` sets `stale =
true` (`ParameterTree.h:165-169`), a full document-half rebuild, which is the second reason it
is called once and never again.

**`wfg replay` is a third owner of that table and it is not a `MediaInfo`.** Replay declares its
own `std::map<std::string, double> durations;` at `Console.cpp:490`, hands it to the Runner by
pointer at `:491` *before the log is parsed*, and fills it at `:581-596` from the log's own
`media` header lines — because the lengths a replay must use are the ones that were true when
the log was written, not the ones in today's folder. That stays as it is: a replay has no files
to hash, and a `MediaInfo` in the replay verb would mean `wfg replay` hashing a bundle it was
explicitly told not to trust.

**One window per hop at the finest level, then halvings, down to 64 frames.** Window 2048, hop
1024 — 46.9 frames per second at 48 kHz — four bytes per frame (hue, saturation, lightness,
peak), each level built from its predecessor by circular mean of hue, arithmetic mean of
saturation and lightness, and max of peak. Format `WFGT` plus version, rate, window, hop and a
level table, little-endian. The reason is one sentence of §3.30's and it is the whole design:
*"so the editor at any zoom and a forty-pixel Gogo bar both read one level and nothing
recomputes."* A Gogo bar forty pixels wide over a six-minute clip is sixteen thousand
finest-level frames averaged into forty, and doing that per frame per redraw is a decision to
make the running pane the most expensive thing on screen. With a pyramid the client picks the
level whose frame count is nearest its pixel count, reads it once, and every subsequent redraw
is a blit — which is why §3.30 can put the run's reading beside `/godot/run/<id>/position`,
*"updated on the tick thread by a table lookup"*.

**`media/.timbre/<sha256>.tpy` would be the first thing the engine has ever written inside a
bundle's `media/` folder**, and that deserves a sentence because the tree is otherwise unanimous
about not doing it. Every existing interaction with `media/` is a read — `Runner::armMedia`
resolves and `existsAsFile()`s the named file (`Runner.cpp:1954-1966`), the message thread opens
it through `AudioHost::setTrackSource` (`AudioHost.cpp:840-859`), `wfg serve` stats each file
for the log header (`Console.cpp:1626-1638`) — and Tracktion's settings and the placeholder WAV
go to the per-user cache precisely so that *"a directory that appeared inside it the first time
somebody pressed play"* does not travel with the bundle (`:2009-2015`). The cache is different
in kind, derived from the media and belonging with it, which is what makes it like a peak file;
but no code path today establishes that `media/` is writable, and a show on a read-only mount
has never needed it to be. So the rule is stated here rather than discovered on tour: **when the
cache cannot be written, the analyser builds the pyramid in memory, publishes it, and says
nothing.** The session gets its colours; the next session pays again. A refusal would trade the
feature for a diagnostic nobody asked for, on the night somebody ran the show off a share. One
implementation note the nearest precedent gets wrong: `Bundle::contentHash` assembles its whole
payload in memory (`Bundle.cpp:229-244`) before hashing at `:251`, and copying that shape for
media would put a gigabyte of WAV in RAM — `juce::SHA256 (const juce::File&)` and `SHA256
(InputStream&, int64)` (`juce_SHA256.h:88, :82`) stream, and 5.7 will use one of those.

**A background job at import, the way plugin scanning is off the show.** §4.2 puts it off the
audio thread and §4.1 off the GO path, and neither is close: the analyser will be its own
`std::thread`, `MountProbe`'s shape exactly — mutex, condition variable, a deque of files, a
de-duplicating in-flight set, `stop()` that sets the flag, notifies, joins and then clears
(`MountProbe.h:128-141`, `MountProbe.cpp:32-59`, `:106-121`) — with the slow work done
**outside** the lock. It will publish by swapping an immutable `shared_ptr<const map<path,
MediaRecord>>` under a short mutex, the pattern `ParameterTree::publish` and `snapshot()`
already use (`ParameterTree.cpp:1572-1594`), and that is the mutex §14.5's route copies out of.
**It will produce no records at all**, which is §14.8's conclusion and §13.4's before it. Show
load never waits for it either — and this section says which read *is* on the critical path, so
nobody attributes a slow open to the pyramid: `mediaDurations` reads every distinct file's
header on the thread that opens the show, before the first publish and before any socket is open
(`Console.cpp:1537-1551`), and has done since Phase 4. That cost is unchanged; the analyser
starts after the first publish.

**A clip whose cache is missing draws grey, and grey is the right missing value.** Black is
wrong because black is *on the ramp* — 40 Hz is near-black purple — so a clip drawn black would
read as a bass bed, a lie in exactly the dimension the colour exists to carry. A spinner is
wrong because it is an animation in a pane whose whole job is to be legible at a glance from
three metres away in the dark, and because it promises a wait the operator has no reason to care
about. Grey is right because grey is what the analysis itself produces for a sound with no
centroid worth naming. That raises a collision worth resolving in the design rather than in the
UI: **white noise is grey too.** So the engine does not publish a grey reading for a missing
pyramid — it publishes **nothing**, an empty node, and the client draws its own grey for the
empty case, while noise publishes a real reading with a real lightness and a saturation below
0.2. A client can always tell *not analysed yet* from *analysed and broadband*, which is the
difference between a bar that will improve and a bar that will not.

**§3.30 asks for the check by name and this section will not let the phase forget it: a 1 kHz
sine must come out saturated at 1 kHz's hue, white noise grey, and a sweep must walk the ramp —
a black-box check on the cache alone, in the style of the routing spike.** A colour ramp is
exactly the kind of thing that looks right and is wrong: any roughly monotonic mapping from
*something spectral* to *something on a gradient* produces a picture that reads as correct, and
a log axis computed from bin index instead of bin centre frequency, an off-by-one in the
window's centre bin, a flatness on power where it should be on magnitude, or a window that leaks
and drags every centroid upward would each shift the picture rather than break it — and a
shifted ramp over unfamiliar material is indistinguishable from a correct one, because nobody
knows what colour that clip *should* be. Which is why **a test that generates its own signals is
the only honest check.** A 1 kHz sine has its centroid at 1 kHz because that is what a sine is,
so the expected hue is a lookup in the ramp table known before the code runs; white noise has a
flatness near 1 and therefore a saturation below 0.2, by definition and not by inspection; a 100
Hz → 8 kHz sweep has a hue that increases monotonically frame over frame, and *monotonic* is a
property a machine can check and an eye cannot. Three signals whose answers are known in
advance, which is the difference between a test and a screenshot. It is written twice,
deliberately, and the second is the one that counts:

| where | what it asserts |
|---|---|
| `tests/TimbreTests.cpp` (new) | the sine is saturated above 0.8 at the ramp's 1 kHz hue within tolerance; noise is under 0.2; the sweep's hue is monotonic along frames; level *k* equals the pairwise means of level *k−1*; write/read round-trips byte-identical; a second `analyse` reports zero work |
| `tests/blackbox/timbre_cache.py` | writes the three WAVs with stdlib `wave`, builds a bundle naming them, runs `wfg analyse`, decodes the `.tpy` with its **own** `struct` reader, and asserts the same three facts |

The second reader is not duplication: it is `common.py`'s standing rule — stdlib only, and *a
separate codec written from the specification so a driver cannot share a mistake with the code
under test* (`tests/blackbox/common.py:18-33`). A `.tpy` decoded by the code that wrote it
proves the writer is self-consistent and nothing else. What 5.7 must budget in CMake rather than
discover is §14.13's.

**`wfg analyse <bundle> [--force]` will be a verb, not a command** — plan decision 12, here to
be overruled early rather than late — for the same reason `validate` and `replay` are: a cache
rebuild is not a document action, it takes no decision the show records, and it must be runnable
on a machine with no audio and no socket. It will run the same code synchronously and print, per
file, path, hash, seconds, frames, milliseconds of work and bytes — the instrument **M22** reads
(§14.14), and the handle the black-box driver pulls. The cost is measured before the design is
judged affordable, not after.

**What Phase 5 must only avoid foreclosing, it does not foreclose.** §3.30 is careful that *"the
strip is a binding, not a D700 feature"*, and this phase honours that by publishing an ordinary
read-only node and drawing one bar from it; the strip's colour cell, the D700 route and
per-channel timbre are §14.15's to defer. §3.30's idle-colour policy — *authored colour at idle,
timbre while sounding, with timbre a layout option that can be off* — is *(proposed)* and stays
the author's: the timbre reading is a node a §3.16 layout may read or ignore, the authored
colour is a separate row, and nothing in the engine decides which of them a surface shows.

### 14.13 The document layer — plumbing, and the rows in one place

Eleven pieces of plumbing, each done once, each of them a thing a later pull request would
otherwise discover by failing. Naming them together is what stops the eleventh from being found
by a fixture, and one of them corrects the approved plan rather than transcribing it — which is
what a section drawn before the code is for.

- **A new row will cost two generators, two commands and three gates, and the plan said one.**
  `scripts/generate-schema.py` writes one file, `SchemaTable.generated.h` (`:14-18`, `:52-53`),
  gated by the ctest `schema.generated` (`tests/CMakeLists.txt:419-420`). The RELAX NG grammar
  is written by the **binary** — `wfg schema --out=docs/schema/show.rng`, which the tool prints
  as the remedy (`Console.cpp:836`) — and gated twice more, by `wfg.schema.C` and
  `wfg.schema.fr_FR` (`:436-441`), under both locales because a range facet goes through the
  number formatter, and then by `schema.fixtures`, which puts the published grammar in front of
  lxml over every fixture (`:489-490`). So a PR author who regenerates the header, sees
  `schema.generated` green and pushes goes red on `wfg.schema.C`, once per row-bearing PR and
  three times in this phase. The order is CSV, generator, binary — and a description carrying a
  comma must be quoted, the field-count check being a hard failure
  (`generate-schema.py:174-200`).
- **`KNOWN_OWNERS` is unchanged, and the asymmetry that decides what a row costs in C++ is the
  most useful fact in this subsection.** `document`, `run`, `media` and `fade` are already in
  the twenty-six-word tuple the generator checks the `owner` and `refers` columns against
  (`generate-schema.py:66-74`), so what differs is `persist`. A `persist != none` row on an
  owner the **document half** already loops over costs no C++ at all:
  `ParameterTree.cpp:782-790` is generic over `rowsForOwner ("document")`. A `persist == none`
  row is published from the **runtime half** and costs a field on `EngineState`
  (`ParameterTree.h:119-123`) and an `else if` in the hand-written chain at `:1252-1274`. So
  `locked` is free, and the undo four and `recovery` are five fields and five branches, each
  assigned in the after-tick **before** `parameters.publish` at `Console.cpp:2263` or published
  one tick stale. Worth knowing before somebody sizes 5.4 by counting rows.
- **What the lock's persistence lacks is not code but a fixture.** §14.4 gives the mechanism —
  the `Show` root is a container element and `EphemeralState` has written and read container
  entries since Phase 3, so `EphemeralState.cpp` gains no line. Two things follow that a fixture
  author has to have right. An unset `locked` is simply **absent** from `state.xml`, because
  `CanonicalXml::attributeText` omits an attribute only when the property is missing and never
  when it equals its default (`CanonicalXml.cpp:75-81`, the `EphemeralState` loop at `:88-96`),
  while `ShowDocument::setAttribute` writes the property unconditionally (`:509-510`) — so the
  first `node.set /godot/document/locked false` after a lock leaves `<Show locked="false"/>` on
  disk and it round-trips, and a fixture must assert the round trip rather than the absence. And
  no bundle carries an example: every `state.xml` under `tests/fixtures/bundles/` holds one
  `<List>` entry and nothing else (`phase4`'s carries only an id), so the container branch
  stands on one unit assertion (`tests/CueListTests.cpp:891`) and on nothing lxml or `wfg
  validate` has ever seen. 5.3 will land a fixture bundle carrying `<Show locked="true"/>`,
  which puts the new `State.Show` define in front of `schema.fixtures` and an outside opinion.
- **The write choke point cannot write a list, and the fade curve is the first row that needs it
  to** — so 5.16a is a change to `ShowDocument::setAttribute` and to `toText` rather than a row
  plus a validate rule, and it lands **after** 5.4, in the function 5.4 has just rebuilt around
  an `UndoManager`. §14.6 carries the citations and the two existing `d*` rows that are proof by
  absence.
- **Five reason codes, into a vocabulary that already carries two nobody writes** (§14.7). What
  belongs here is where the dead ones survive: `reason::malformedPacket` in three test files and
  one fixture (`CommandTests.cpp:264`, `EngineTests.cpp:209`, `:267`, `EventLogTests.cpp:79`,
  `:85`, `tests/fixtures/logs/skeleton.wfglog:10`), and `reason::retiredId` nowhere but its
  declaration at `Command.h:154`.
- **Static files are served as text, and the MIME table promises types it mangles.**
  `serveClient` resolves a subdirectory correctly and refuses anything that is not `isAChildOf
  (clientDirectory)` on the **resolved** file rather than on the request text
  (`OscQueryServer.cpp:211-239`) — then serves every byte through
  `file.loadFileAsString().toStdString()` (`:244`), while `mimeFor` (`:75-88`) already promises
  `.png` and `.woff2`, so a client shipping either gets it back through a UTF-8 round trip that
  is not one and nothing says so. 5.1 will swap in `loadFileAsData` and teach `mimeFor` `.mjs`,
  `text/javascript` as `.js` already is (`:80`). The assertion does not go where the plan put
  it: `serveClientFrom` has no unit-test coverage at all — its only caller is
  `Console.cpp:1796`, and `tests/OscQueryTests.cpp` mentions neither `/ui` nor `mimeFor` — but
  it is driven end to end by `tests/blackbox/client_page.py`, a locale pair at
  `tests/CMakeLists.txt:694-704`, which is where a one-pixel PNG is served and compared byte for
  byte. Two harness additions go with it: `OscQueryTests::get` (`:241-293`) throws every header
  away and must keep them, since `Content-Type` is the half a MIME change breaks and
  `Cache-Control` is what §14.5's route is asserted on; and `common.py`'s `http_get` decodes to
  text (`:195`), so a bytes-returning sibling comes before any driver can compare a pyramid.
- **`/media` is reserved against mounts exactly as `/ui` is.** `Mount::prefixIsUsable` refuses
  `/ui` and `/ui/…` by name (`tree/Mount.cpp:73-77`), beside its refusal of `/`; `/media` joins
  that line in 5.1, or a show could mount a namespace over the route that answers with pyramids
  and the failure would read as a cache miss rather than a collision.
- **`Engine::setBeforeApply` is installed by the serve verb AND the replay verb, and forgetting
  the second is the bug that takes a week.** The hook is vendor-free and mirrors
  `TickThread::setBeforeTick` (`clock/TickThread.h:162-168`), as `Engine.h:36-39`'s ban on JUCE
  types in that surface requires; where inside `applyEvent` it fires, and why, is §14.9's. The
  week is this: a replay running no hook reproduces every existing fixture perfectly, because
  what it compares is records (§14.9), and diverges only where coalescing mattered — thousands
  of records into a log nobody has recorded yet.
- **What the replay verb registers, and it is not everything.** *This paragraph is a correction*
  *(PR 5.0, 2026-09-09)*: the plan reads as though `registerDocumentCommands` were unconditional
  under `wfg replay`, and it is not — the call at `Console.cpp:534` sits inside `if
  (bundlePath.isNotEmpty())`, opened at `:497` and closed at `:570`. §14.7 owns the rule that
  follows, and the consequence for a driver is that a log carrying an `undo` needs `--bundle`
  exactly as a log carrying a `document.save` needs `--out`, so both flags belong in the replay
  step by name.
- **`run/position` is declared, published and assigned by nothing**, so what every client has
  read for two phases is the literal `0`; 5.1 will pay it before anything is built on it, and
  §14.5 says why it needs its own pass over `runs.all()`.
- **Test wiring three pull requests would each discover separately.** `tests/UndoTests.cpp` and
  `tests/TimbreTests.cpp` do not exist until CMake is told: they join the source list at
  `tests/CMakeLists.txt:60-95`, which ends `OscQueryTests.cpp)` at `:95`, and neither 5.4's nor
  5.7's file list in the plan mentions it. `MediaCueTests.cpp` is already in that list (`:83`),
  so 5.6's two assertions — that the durations map is byte-for-byte what it was before the
  `MediaInfo` object owned it, and that a snapshot taken from another thread is the one the tick
  thread published — cost no CMake line at all. 5.2's `dirty` assertion goes into
  `phase1_session.py` beside the `--out` it already passes for the replay step (`:310-312`),
  which is the driver that already saves. The new drivers register per locale on the phase4
  pair's shape at `:669-678` — though not every driver is a pair: `blackbox.device.C` is
  registered once (`:623-627`) because `device_serve.py` *"SKIPS ITSELF on a machine with no
  audio device, which is every CI runner, and that is honest rather than convenient"*
  (`:620-622`), and `timbre_cache.py` is a candidate for that shape if `wfg analyse` wants a
  format this build's reader lacks and the pair if it does not.

**Everything the parameter table gains.** Every row lands with the pull request that publishes
it, never before. This table is canonical for the mechanical columns; the argument for each row
is in the subsection that owns the mechanism, cited beside it. It spells read-only `ro`, as §13
does; the CSV column itself carries `r`.

| owner | address | type | access | persist | rate cap | PR | notes |
|---|---|---|---|---|---|---|---|
| `document` | `locked` | `T` | `rw` | `state` | 5 | 5.3 | §14.4 for the row and its persistence, §14.11 for what it refuses. That it survives a restart is plan decision 6, here to be overruled early rather than late |
| `document` | `canUndo` | `T` | `ro` | `none` | 5 | 5.4 | `persist == none`, so an `EngineState` field and an `else if` — the expensive half of the asymmetry above |
| `document` | `canRedo` | `T` | `ro` | `none` | 5 | 5.4 | As above (§14.9) |
| `document` | `undoName` | `s` | `ro` | `none` | 5 | 5.4 | `getUndoDescription()` (§14.9). Empty when there is nothing to undo, so a client greys its menu item from one node |
| `document` | `redoName` | `s` | `ro` | `none` | 5 | 5.4 | As above |
| `document` | `recovery` | `T` | `ro` | `none` | **1** | 5.5 | §14.10 for what it reports, §14.4 for why the cap is 1 and not the 5 of its livelier neighbours |
| `document` | `dirty` | `T` | `ro` | `none` | 5 | — | **No new row** (`csv:21`). 5.2 will make it true (§14.10) and rewrite its description to say *to `show.xml`*, because after 5.5 there is a second file it could have meant |
| `media` | `hash` | `s` | `ro` | `none` | 1 | 5.8 | Addressed **`/godot/cue/<id>/hash`** and emitted as the `cue/prepare` pair, both halves of it §14.5's |
| `run` | `timbre` | `s` | `ro` | `none` | 10 | 5.8 | `"<hue> <saturation> <lightness>"` — one string rather than three nodes, because it is one lookup and a client reads it as one. That shape and that cap are plan decision 8, here to be overruled early rather than late (§14.5, §14.15) |
| `fade` | `points` | `d*` | `rw` | `show` | 50 | 5.16a | Plan decision 9, open to the same overruling. The third list-typed row and the first anybody can write, which is why it lands after 5.4 (§14.6) |

Commands, engine-origin records and reason codes are drawn in §14.7, §14.8 and §14.11 rather
than repeated here.

### 14.14 What Phase 5 has to measure, and in which order

Phase 4's numbering ended at M21.

| | what | why it gates |
|---|---|---|
| **M22** | seconds of analysis per minute of audio, and bytes of cache on disk per minute, at window 2048 / hop 1024, on the Windows box **and** the Mac mini (PRD §6.11 asks for this one by name) | whether import can afford the analysis silently, which is §3.30's whole claim; taken before 5.7 is judged affordable |
| **M23** | the canonical write plus atomic replace of the 500-cue show, on the tick thread, in milliseconds, both machines | whether autosave's bytes stay on the tick thread; taken **before 5.5 lands** |
| **M24** | `performance.now()` around the console's `render()` on a 500-cue show, before and after keyed rows | 5.9's claim that the page can hold a real show — which is decision T's premise, not a detail of it |

**M22 — can a show be imported without anybody being told an analysis is happening?** §3.30 says
the cache is *"built by a background job at import, off the audio thread and off the GO path the
way plugin scanning is off the show"*, and *the way plugin scanning is off the show* is a
promise about a cost nobody has measured. It decides whether the analyser is a thread nobody
mentions or a thing with progress in front of it, and those are different designs rather than
different constants. **How:** `wfg analyse <bundle> --force` (§14.12) will run the same code
synchronously and print per file the hash, the seconds of audio, the frames, the milliseconds of
work and the bytes written; three runs over a few minutes of real material, both machines,
Release. **What each answer changes:** if the work is a small fraction of real time, the
analyser stays one background thread queued at load and the operator is told nothing, which is
what §3.30 wants; if it is near or above real time on the Mac mini, either the finest level
coarsens to hop 2048 — halving the work and the bytes, at the cost of a frame every 42.7 ms
instead of every 21.3 — or import becomes explicit, with the queue's depth published and a
client drawing it. Either way show load never waits, because the analyser starts after the first
publish and `timbre` is empty until a pyramid exists. The bytes half decides a location rather
than a design: four bytes a frame at 46.9 frames a second is about 11 kB per minute at the
finest level, so an hour of material carries something near a megabyte and a half **inside its
own bundle** — and an answer an order of magnitude worse reopens that, because a bundle travels
and `media/.timbre/` would be the first thing this engine has ever written into `media/`.

**M23 — does a 500-cue autosave fit inside a tick?** `document.save` already writes on the tick
thread, and `Bundle.h:126-130` says out loud why that was allowed and why it is not the end of
the argument: *"The cost is a file write inside a tick, and that is why this is Phase 1's answer
rather than Phase 5's — crash-safe autosave (PRD §4.3) is a background writer working from a
snapshot, and it is a different piece of work."* This phase proposes to keep the bytes on the
tick thread, which is a change of mind about a sentence already in the tree; it is taken before
5.5 lands because nobody presses save mid-cue and autosave fires on its own, so what it measures
is a tick going late during a show. **How:** `CanonicalXml::write` plus `writeBytesAtomically`
timed around the existing handler on the synthetic 500-cue show M18 already generates, a hundred
saves, median and 99th percentile, Release, both machines. **What each answer changes:** at or
under a quarter tick — **5 ms** — the bytes stay where `document.save` already puts them and the
header's sentence is answered rather than obeyed; above it, 5.5 will take the snapshot on the
tick thread, which it must do in either shape, and hand the bytes to a writer thread on
`MountProbe`'s shape (`MountProbe.h:128-141`, the slow work outside the lock at
`MountProbe.cpp:115-121`), at which point the GO path stops sharing a thread with a disk at all.
The log record is `document.autosave` either way, so the fallback is a switch and not a
redesign, which is why §14.10 answers §4.1 without waiting for the number. What M23 does not
measure is the atomic write itself, which §14.10 argues for independently of any number.

**M24 — can the page hold a real show?** Decision T rests on the console being the operator
client and the laboratory both, and a page that stutters at five hundred cues is neither. It
will be answered inside 5.9, because 5.9 is the pull request that claims it. **How:**
`performance.now()` around `render()` on the 500-cue bundle, ten polls, median and worst, in the
PR description — and §14.3's two defects measured separately, or the before-and-after says
nothing about which paid, since keying the rows fixes only the first of them. **What each answer
changes:** if the after-number fits well inside the 100 ms poll, the page is the operator client
and decision T's sequencing holds without further argument; if it does not, the strip will
render a window rather than a list — a real design change to a view the author is still moving —
or the JUCE client's `ListBox`, which recycles components for exactly this, starts earlier than
decision T says. That second answer would be the one measurement here that makes the layout stop
moving for a reason other than the author being satisfied with it.

**The calendar and the importance disagree, and it is worth saying which is which.** M23 comes
round first, because 5.5 is the first pull request that waits on a number. M22 matters most — it
is the only one whose answer changes a *shape* rather than a placement, and PRD §6.11 asks for
it by name — but it cannot be taken before its instrument exists, and `wfg analyse` lands with
5.7, which the plan's order puts after 5.5. M24 lands whenever 5.9 does, which decision T lets
the author pull forward to just after 5.3, so it may in practice be taken first of all. None of
that corrects the plan, which orders each measurement by the pull request that needs it rather
than against the other two; it says only which comes round first, so that nobody plans a week
around taking M22 early. And none of them gates a test: a wall clock on a shared CI runner is a
flaky test that teaches people to re-run the suite — Phase 4's own sentence — so what ctest
asserts is counts, as M18's rebuild count is, and what is written down here is milliseconds.

### 14.15 The direction this phase does not build

**Undo of a GO.** Constraint 5 makes it *revert*, not undo: restore standby, release bindings,
re-assert pre-GO state, and be honest that the audio already escaped. Nothing falls out of the
`UndoManager` for free (§14.9). A revert needs an inventory: which runs this GO created, which
claims they hold and which they are waiting on, which pre-sent values the prepare before it
asserted, and what the read-back said they were before. That is the inventory constraint 4's
**Go Doh!** has its specification deferred until: one inventory, two features, and building
either against a partial one is how the two come to disagree in the dark. Half the mechanism
exists and is worth naming so nobody builds it twice — `run.revoke` (§13.4) already puts
pre-sent values back, which is the anticipation half of *"re-asserts pre-GO state"* and the half
that makes a double-GO caught inside the window recoverable.

**A load command.** Said honestly rather than deferred: `wfg serve` cannot switch bundles, and
this phase does not teach it to. `document`, `mounts`, `runner` and `parameters` are all locals
of `runServe` (`Console.cpp:1429-1560`), `registerBundleCommands` is handed the folder at
`:1528` and captures it by value and not `mutable` (`Bundle.cpp:294`), and the log's header
carries the bundle's content hash before the first tick (`:1617`), from a bundle a second would
not share. Re-pointing all of that is not a command, it is the serve verb turned inside out —
and a load is a process restart, which is what an operator does between shows anyway.
`document.revert` and `document.recover` are in the phase because both re-adopt the **same**
folder through a hatch that already exists (§14.10, §14.11).

**Undo of mounted-parameter writes.** Phase 6's domain, and the enum reserves the slot.
`node.set` on an address that is not under `/godot` never reaches the document at all — one
fork, at `DocumentCommands.cpp:359-360` — so nothing here could undo a fader by accident. And
the fader is why it belongs in a domain of its own: a designer riding a level for twenty minutes
would fill a shared stack with a thousand transactions and bury the cue rename somebody actually
wants back.

**Free text on a refusal.** `/godot/engine/lastError` carries tick, sequence, origin, reason and
command and no more (`csv:16`), which is why an operator whose disk is full gets the word and
not the diagnosis (§14.10). A free-text field on that node is a second contract about a line the
log format already fixes — `Command.h:107-109` calls the reason vocabulary a contract for
exactly this reason — and it should be made once for every reason code rather than opened for
one command's benefit in the phase that adds five.

**A dirty dot that compares.** `dirty` says the bytes on disk are behind this document's
history, not that the two differ, and §14.10 argues why. A content comparison against the last
saved bytes would cost a canonical write per tick to answer a question nobody asks that often,
and would still be wrong the moment two edits cancelled by coincidence rather than by undo.

**The strip's colour cell and the D700 route.** §3.30's own last sentence assigns them: Phase 5
for the cache, the editor and Gogo; Phase 6 for the strip. Phase 5 publishes `run/timbre` and
stops there. The binding to §3.16's colour cell, the profile that quantises and rate-limits, and
the D700's colour route are Phase 6's, and the measurement they need — §6.11's colour write
rate, now whether seventeen elements repainted ten times a second over MIDI is tolerated — has
not been taken. *This sentence is a correction (2026-09-10), following the author's §3.30
correction of the same day:* the D700 takes colour over MIDI, as note-on at each element's own
button note, and not over HID, so the interval at which back-to-back HID writes fault — which
this sentence first named — no longer matters to anything this phase defers.

**Per-channel timbre.** One colour per run, from a mono fold. A multichannel bed whose surrounds
carry different material reads as their sum, and for a forty-pixel Gogo bar that is the right
answer rather than a compromise. Per channel is a pyramid per channel, N times the cache, and a
bar nobody has designed.

**A curve on the stop cue's fade half.** `Fade/@points` lands on `Fade` and `Stop` is untouched
(§14.6). A stop's fade is a release, and the shapes a release wants are what `curve`'s `linear |
sCurve` already offers; giving `Stop` breakpoints before anybody has asked would double the
validate rule and double the editor's target for a gesture nobody has made.

**Browser automation on CI.** No workflow installs a JavaScript runtime today, and Playwright
means a browser download on three runners, on every job, to assert what the two cheaper checks
5.18 will add cover between them (§14.3). That is plan decision 11, here to be overruled early
rather than late. What it leaves untested is the DOM, and the one piece of DOM worth a test is
5.9's keyed reconciler, written as a pure function over a `Map<id, element>` and tested there
instead. Playwright when the page has stopped moving, if at all.

**The JUCE desktop client beyond an outline**, per decision T, and the `WebBrowserComponent`
shell beyond the paragraph §14.16 gives it. What would make it start is written there and is one
sentence long: the layout stops moving.

**Esc, double Esc and Go Doh!** Phase 10, per constraint 4, whose third level is
specification-deferred in the law itself. And plainly, because it is better known than
discovered: **the console has no abort key at all today.** There is no `Escape` handler beyond
the blur-and-cancel 5.9 will add for dirty fields, and of the run commands the engine registers
only `run.kill` is offered, though `run.advance`, `run.prune`, `run.unprune` and `run.stop` all
exist — 5.11 will add those four (§14.3). That is on-plan and it stays on-plan, but a page that
looks like an operator client and has no panic key is a page somebody will reach for at 04:12
and not find, so it says nothing about the three levels of stop rather than implying half of
one.

**Drag-and-drop reordering on the page.** The console's own comment declines it and the reason
has not changed: *"Dragging is what a desktop UI will do and is not what a first pass should try
over a poll: a row that moves under the pointer while the tree is being re-fetched is a fight
nobody wins. Two buttons say the same thing and cannot half-happen"*
(`clients/console/index.html:1176-1184`). ▲ and ▼ over `object.move` stay the gesture until the
author asks otherwise — with the one wrinkle undo introduces, that two ▲ presses inside one open
transaction collapse into one undo step whether anybody wanted that or not (§14.9).

**PRD amendments this phase will propose at close-out**, recorded now so they are not
rediscovered: §3.30's *"PR 4.1's `MediaInfo` side table… is where the cache is looked up"*
against a cache that must live **beside** the durations rather than in them, since that map is
compared by address and has to stay frozen and pointer-stable (§14.12); §3.30's and §7's *two
values per running clip* against the three the node carries, lightness being the frequency axis
§4.8 rests on inside one channel (§14.5, §14.12); §3.30's *(proposed)* idle-colour policy, which
stays the author's until the sine/noise/sweep bundle is on a screen; §3.20's derived-state
sentence gaining the recovery folder by name, because it says a cache is *"never in the show"*
without saying where a crash-safe autosave lands; §3.17's *"deliberately undesigned"* gaining
what decision V settles, the web client being both the tablet's primary surface and the
laboratory the desktop layout is designed in; §3.17's *"(TypeScript over OSCQuery + WebSocket)"*
becoming *(ES modules over OSCQuery + WebSocket, served by the engine and editable while a show
runs)*, since the parenthesis names a language where the sentence's argument is about a contract
— no install, a self-describing namespace, no hand-maintained parameter table — that the
plain-module client keeps in full (§14.2); and §4.3's *"crash-safe autosave"* gaining the word
that says what it is safe **from**: an unclean kill, which the event log survives because every
record is flushed to the OS as it is written (`EventLog.h:74-76`), and not a power cut, which
neither the log nor the autosave has ever promised (§14.8).

The five Phase 4 amendments are carried forward unchanged, together with the sixth §13.15
promised and the close-out did not carry — §3.13's *walk back* against §13.8's forward pass,
which §13.16 records as still owed — and the Phase 3 ones still waiting. They are sentences,
delegated per the standing rule, and applied when the author says so.

### 14.16 The desktop client — an outline, and what would make it start

**§9's question E is not answered here, and this outline assumes E's own recorded fallback: a
separate client.** The question — the desktop UI in process or a client of its own — has been
open since Phase 2, §9 files it with the fallback *assume separate, because that is the stricter
assumption*, and nothing in T, U, V or W settles it. What has changed is that evidence has begun
to arrive, and it points the same way. PRD §3.2 states the law — *"nothing the UI can do that
the API cannot. The UI is built as a client"* — and an in-process UI is a client with a shortcut
available to it; the shortcut erodes the law not on the day it is taken but on the day somebody
takes it because a datagram round trip was inconvenient and the document was right there on the
same heap. The page is that evidence and not the verdict: it holds selection, fold state and a
slider under a finger and nothing else (§14.1), and has driven this engine since Phase 3 without
one hole opened for it. What would settle E is the day a second client needs a hole the first
did not — and by then the second client exists, which is why decision T defers the question
rather than this subsection answering it. If the author wants E closed it becomes a lettered
decision in §9, not a sentence here.

Decision T makes what follows an outline and not a plan: the client starts when the layout has
stopped moving. Decision U leaves the done-when's own judgement — *the author runs a simple show
from the desktop build in a rehearsal room* — to the room. They are two judgements and the
outline should not blur them.

**The CMake target, and what it costs.** `JUCE_MODULES_ONLY` is `ON` globally at
`cmake/WfgThirdParty.cmake:62-63` and cannot be enabled for one target: it is read before the
early return that creates the helper targets, so juceaide exists for the whole configuration or
for none of it. The comment beside it names the flip — *"If Phase 5 wants `juce_add_gui_app`,
flip this ON->OFF and nothing else in this file changes"* (`:61`). So either a second plain
`add_executable (wfg-client …)` with `START_JUCE_APPLICATION` and the app defines written by
hand, which buys no `.app` bundle, no icon and no plist but costs nothing on any CI job; or the
flip and `juce_add_gui_app` for a real bundle, at roughly sixteen seconds more per configure on
every job in every matrix. The first for the phase, the second as the one-line change the
comment already names, taken when the client ships to somebody who is not the author.

**The client layer, and the rule it would set aside in the open.** `wfg::client::EngineClient`
polls `GET /godot` on a client thread through Go.dot's **own** `oscquery::OscQueryClient`, which
says in capitals what it is — *"IT BLOCKS, AND IT MUST NEVER RUN ON THE TICK THREAD… MountProbe
is what owns the thread this runs on; nothing else may call it"* (`OscQueryClient.h:46-51`).
That second sentence forbids the reuse and has to be answered rather than stepped over: it is a
rule about the **engine** process, where `MountProbe` owns the one thread allowed to block
against a device that has gone away. A client process has no tick thread and no deadline of its
own, so `wfg-client` would be a sanctioned second owner — and the honest form of that is the
header sentence gaining the words *inside the engine* in the pull request that adds the caller.
The WebSocket half comes from juce_simpleweb's client side, as WFS-DIY's
`Plugin/Source/Shared/OscQueryClient.h` already does it; a snapshot model reaches the message
thread and every write is a datagram. That is §14.2's contract unchanged: two clients, one
contract, neither with a door the other lacks.

**The component tree, and the reuse named rather than assumed.** `MainWindow` → `Transport` /
`Didi` (a `juce::ListBox` keyed by cue id, standby and selection drawn as distinctly from each
other as the page draws them, since constraint 8 makes colour never the sole carrier) / `Gogo` /
`Inspector` / `Header` / `Curve`. XOA's
`Source/GUI/{Binding,Layout,Selection,Widgets,XoaLookAndFeel.h,ColorScheme.h}` is a generalised
kit still under construction, lifted where it fits and forked where it does not; spatcore's
`ui/EQDisplayComponent.h` is the curve editor's nearest relative, a draggable breakpoint over a
log axis being one whatever it edits. The reuse map records the rule: the mechanisms port and
the APIs do not.

**One `ApplicationCommandManager`, which is what would make constraint 11 structural rather than
aspirational.** *Every gesture-reachable action exists as a named command* is a discipline on
the page — nothing in a browser stops a click handler from calling a function directly — and a
property of the framework in JUCE: a component does not bind a key, it invokes a command id, and
a command id with no `ApplicationCommandInfo` does not exist. Each `perform` emits exactly one
engine command; a gesture that wants two is either two gestures or one engine command that does
not exist yet — and that second answer is the useful one, because it turns a UI convenience into
a question about the command set.

**One gesture-to-command table, read by both clients from one file.** The desktop client would
read `clients/console/gestures/commands.json` — which 5.10 will land, 5.11 will fill and 5.18
will check against `wfg commands` (§14.3) — rather than a copy, so neither client can drift and
the check pays twice, once for the page and once for a client that does not exist yet. The
keystroke half stays per client: a Mac menu bar's ⌘ and a browser's Ctrl are not the same
gesture, and pretending they are is how a page swallows a shortcut the browser had first. A
client-side test then needs no engine: a `FakeLink` capturing bytes, and `ClientTests.cpp`
asserting that *Space* produces `/godot/cmd/go` and a field commit produces `node.set <address>
<text>`, against the byte fixtures `OscCodecTests.cpp` already hand-wrote — the `common.py`
rule, that a client test sharing an encoder with the engine cannot catch a mistake they share.

**The alternative, offered for the author to weigh and not recommended.** A JUCE shell hosting
the web client through `juce::WebBrowserComponent` — WebView2 on Windows, a resource provider
serving `clients/console/`, and native functions for the three things a page cannot do: a file
dialog for `saveAs`, an audio device list, and a global GO hotkey the browser will not
surrender. It buys one layout maintained in one place and keeps the page as the laboratory
permanently, which is decision T's premise made durable rather than temporary. Its weakness is
exactly the native client's strength: a `ListBox` over ten thousand rows that recycles its
components, and a native menu bar, where a stage manager looks for the command whose key they
have forgotten. The author weighs that once the layout has stopped moving; this section does
not.

**What makes the client start: the layout stops moving.** Concretely, so that it is a
recognisable state rather than a mood — the author stops asking for a view to be moved and
starts asking for one to be faster; M24's after-number becomes the interesting one; and three
consecutive Half B pull requests change only what is inside a pane and not which panes there
are. Until then a compiled client is an argument that takes a rebuild to have and the page is an
argument that takes a refresh. A client may start before the done-when is met and it may never
start at all; what it must not do is start while the layout is still being designed.

### 14.17 What Phase 5 built, against what section 14 drew

*Owed at close-out and written by PR 5.19, as §11.9, §12.15 and §13.16 were — a heading here
rather than an obligation to remember, because §13.16's own closing argument is that a
retrospective written a phase late is written from the commit messages rather than from the
week.*
