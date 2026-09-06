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

### Open, with the subphase that forces each

| # | Question | Forced by | Fallback if undecided |
|---|---|---|---|
| E | Does the Phase 5 desktop UI run **in-process or as a separate client**? | Phase 5, but it shapes Phase 2's plugin-parameter handover | assume separate, because that is the stricter assumption and the one PRD §3.2 reads most naturally |
| J | **Should PRD §4.2 record what Tracktion does inside the callback?** Its device callback takes one uncontended `std::shared_lock` per block and its node-player pool uses semaphores; the lipogram can be *enforced* on Go.dot's code and only *measured* on Tracktion's (§11.5). A PRD amendment is the author's to make. | the lipogram test (PR 2.2) | enforce on Go.dot's scopes, report Tracktion's count separately, never hide it |
| ~~K~~ | *(settled 2026-09-06, in PR 2.6, the way this table recommended — see below)* **How does a mount declare what it can do?** `transport` says how to *send* and nothing says whether the target can be *asked*, so `wait: verified` against a write-only device is a cue that cannot succeed and nothing notices until the show. Chataigne carries two booleans per module, `hasInput` and `hasOutput`, for exactly this. Also: whether the answer names the *mechanism* (`oscquery` \| `poll` \| `subscribe` \| `none`) or only the capability. | `verified` (PR 2.6) | a mount-level `readback` enum defaulting to `none`, and a `verified` cue against `none` refused at load — the strictest reading, and the one that cannot fail silently |

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

*Written at close-out.*
